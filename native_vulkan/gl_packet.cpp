#include "gl_packet.h"
#include "vk_packet.h"

#include "gl_state.h"
#include "gl_shader.h"
#include "vk_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// gl_vk_bridge.h 会拉入 gl_compat.h（hxcpp/CFFI），这里只需要两个函数，单decl。
namespace lime {
VkRenderer* gl_vk_get_renderer ();
uint32_t gl_vk_fbo_target (int fboName);
bool gl_vk_texture_size (int glName, int* w, int* h);
// gl_texture_params.cpp：按 GL 纹理名取当前采样参数（texParameteri 的镜像）
bool gl_vk_texture_sampler_state (int glName, int* minFilter, int* magFilter,
                                  int* wrapS, int* wrapT);
}

namespace lime {
namespace pkt {

// ---------------------------------------------------------------- 归一化
// 把 GL 枚举收敛成 PktDrawDesc 里的小枚举；认不出的一律 0xFF（= 未指定，走默认）。

static uint8_t NormMinFilter (int gl) {
    switch (gl) {
        case 0x2600: return 0;   // GL_NEAREST
        case 0x2601: return 1;   // GL_LINEAR
        case 0x2700: return 2;   // GL_NEAREST_MIPMAP_NEAREST
        case 0x2701: return 3;   // GL_LINEAR_MIPMAP_NEAREST
        case 0x2702: return 4;   // GL_NEAREST_MIPMAP_LINEAR
        case 0x2703: return 5;   // GL_LINEAR_MIPMAP_LINEAR
        default:     return 0xFF;
    }
}

static uint8_t NormMagFilter (int gl) {
    if (gl == 0x2601) return 1;   // GL_LINEAR
    if (gl == 0x2600) return 0;   // GL_NEAREST
    return 0xFF;
}

static uint8_t NormWrap (int gl) {
    switch (gl) {
        case 0x812F: return 0;   // GL_CLAMP_TO_EDGE
        case 0x2901: return 1;   // GL_REPEAT
        case 0x8370: return 2;   // GL_MIRRORED_REPEAT
        case 0x2900: return 0;   // GL_CLAMP（老语义取 border 色，这里按 clamp_to_edge 兜底，最接近）
        default:     return 0xFF;
    }
}

static uint8_t NormBlendFactor (int gl) {
    switch (gl) {
        case 0x0001: return PKT_BLEND_ONE;
        case 0x0000: return PKT_BLEND_ZERO;
        case 0x0302: return PKT_BLEND_SRC_ALPHA;
        case 0x0303: return PKT_BLEND_ONE_MINUS_SRC_ALPHA;
        case 0x0304: return PKT_BLEND_DST_ALPHA;
        case 0x0305: return PKT_BLEND_ONE_MINUS_DST_ALPHA;
        case 0x0300: return PKT_BLEND_SRC_COLOR;
        case 0x0301: return PKT_BLEND_ONE_MINUS_SRC_COLOR;
        case 0x0306: return PKT_BLEND_DST_COLOR;
        case 0x0307: return PKT_BLEND_ONE_MINUS_DST_COLOR;
        case 0x8003: return PKT_BLEND_CONST_ALPHA;
        case 0x8004: return PKT_BLEND_ONE_MINUS_CONST_ALPHA;
        default:     return PKT_BLEND_UNSET;
    }
}

// ---------------------------------------------------------------- GL 常量

enum {
    GL_BLEND = 0x0BE2,
    GL_CULL_FACE = 0x0B44,
    GL_DEPTH_TEST = 0x0B71,
    GL_STENCIL_TEST = 0x0B90,
    GL_SCISSOR_TEST = 0x0C11,

    GL_FRONT = 0x0404,
    GL_BACK = 0x0405,
    GL_FRONT_AND_BACK = 0x0408,
    GL_CW = 0x0900,

    GL_PNAME_CULL_FACE_MODE = 0x0B45,
    GL_PNAME_FRONT_FACE = 0x0B46,
    GL_PNAME_DEPTH_FUNC = 0x0B74,
    GL_PNAME_DEPTH_WRITEMASK = 0x0B72,
    GL_PNAME_BLEND_SRC_RGB = 0x80C9,
    GL_PNAME_BLEND_DST_RGB = 0x80C8,
    GL_PNAME_BLEND_SRC_ALPHA = 0x80CB,
    GL_PNAME_BLEND_DST_ALPHA = 0x80CA,
    GL_PNAME_BLEND_EQ_RGB = 0x8009,
    GL_PNAME_BLEND_EQ_ALPHA = 0x883D,
};

// ---------------------------------------------------------------- 运行时

struct FrameGate {
    bool all = false;
    bool any = false;
    int from = 0;
    int to = 0;
    bool parsed = false;
};

struct Runtime {
    int enabled = -1;              // -1 未探测，0/1 结果
    vkpkt::Arena vertex;
    vkpkt::Arena index;
    vkpkt::Arena uniform;
    bool arenasReady = false;
    uint32_t frameGen = 0xFFFFFFFFu;
    uint32_t nextSeq = 1;
    uint32_t utag = 0;
    bool utagValid = false;
    FrameGate gate;
    uint64_t decoded = 0;
    uint64_t rejected = 0;
    uint64_t executed = 0;
    int exec = -1;                 // -1 未探测，0/1 结果（VK_PKT_EXEC）
};

static Runtime g_rt;

static int ClampI16 (int v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return v;
}

static void EnsureArenas () {
    if (g_rt.arenasReady) return;
    vkpkt::ArenaInit(&g_rt.vertex, vkpkt::kArenaVertexInit);
    vkpkt::ArenaInit(&g_rt.index, vkpkt::kArenaIndexInit);
    vkpkt::ArenaInit(&g_rt.uniform, vkpkt::kArenaUniformInit);
    g_rt.arenasReady = true;
}

static void ResetArenas () {
    if (!g_rt.arenasReady) return;
    vkpkt::ArenaReset(&g_rt.vertex);
    vkpkt::ArenaReset(&g_rt.index);
    vkpkt::ArenaReset(&g_rt.uniform);
}

// ---------------------------------------------------------------- 帧段门控

static void ParseGate () {
    if (g_rt.gate.parsed) return;
    g_rt.gate.parsed = true;
    const char* v = getenv("VK_PKT_FRAMES");
    if (!v || !v[0]) return;
    if (strcmp(v, "all") == 0) {
        g_rt.gate.all = true;
        g_rt.gate.any = true;
        return;
    }
    int a = 0, b = 0;
    int n = 0;
    if (sscanf(v, "%d..%d", &a, &n) == 2 || sscanf(v, "%d-%d", &a, &n) == 2) {
        b = n;
    } else if (sscanf(v, "%d", &a) == 1) {
        b = a;
    } else {
        return;
    }
    g_rt.gate.any = true;
    g_rt.gate.from = a;
    g_rt.gate.to = b;
}

static bool GateOpen (uint32_t frameGen) {
    ParseGate();
    if (!g_rt.gate.any) return false;
    if (g_rt.gate.all) return true;
    int f = (int)frameGen;
    return f >= g_rt.gate.from && f <= g_rt.gate.to;
}

// ---------------------------------------------------------------- 状态抓取

static int Q1 (int pname, int fallback) {
    int v = fallback;
    if (glstate::QueryInteger(pname, &v, 1) == 0) return fallback;
    return v;
}

static void FillFace (int face, vkpkt::PktFace* out) {
    memset(out, 0, sizeof(*out));
    int func = 0, sfail = 0, dpfail = 0, dppass = 0;
    uint32_t vmask = 0, wmask = 0;
    glstate::GetStencilFace(face, &func, &vmask, &wmask, &sfail, &dpfail, &dppass);
    out->func = (uint16_t)func;
    out->valueMask = vmask;
    out->writeMask = wmask;
    out->sfail = (uint16_t)sfail;
    out->dpfail = (uint16_t)dpfail;
    out->dppass = (uint16_t)dppass;
}

static void GatherState (vkpkt::StateInput* s) {
    memset(s, 0, sizeof(*s));

    s->blendEnabled = glstate::IsEnabled(GL_BLEND) ? 1 : 0;
    s->depthTestEnabled = glstate::IsEnabled(GL_DEPTH_TEST) ? 1 : 0;
    s->stencilEnabled = glstate::IsEnabled(GL_STENCIL_TEST) ? 1 : 0;
    s->scissorEnabled = glstate::IsEnabled(GL_SCISSOR_TEST) ? 1 : 0;

    if (glstate::IsEnabled(GL_CULL_FACE)) {
        int mode = Q1(GL_PNAME_CULL_FACE_MODE, GL_BACK);
        s->cullMode = (mode == GL_FRONT) ? 2 : (mode == GL_FRONT_AND_BACK) ? 3 : 1;
    } else {
        s->cullMode = 0;
    }

    s->frontFace = (Q1(GL_PNAME_FRONT_FACE, 0x0901) == GL_CW) ? 1 : 0;
    s->depthFunc = (uint16_t)Q1(GL_PNAME_DEPTH_FUNC, 0x0201);
    s->depthMask = Q1(GL_PNAME_DEPTH_WRITEMASK, 1) ? 1 : 0;

    int cm[4] = {1, 1, 1, 1};
    glstate::GetColorMask(cm);
    s->colorMaskBits = (uint8_t)((cm[0] ? 1 : 0) | (cm[1] ? 2 : 0) | (cm[2] ? 4 : 0) | (cm[3] ? 8 : 0));

    s->blendSrcRgb = (uint16_t)Q1(GL_PNAME_BLEND_SRC_RGB, 1);
    s->blendDstRgb = (uint16_t)Q1(GL_PNAME_BLEND_DST_RGB, 0);
    s->blendSrcAlpha = (uint16_t)Q1(GL_PNAME_BLEND_SRC_ALPHA, 1);
    s->blendDstAlpha = (uint16_t)Q1(GL_PNAME_BLEND_DST_ALPHA, 0);
    s->blendEqRgb = (uint16_t)Q1(GL_PNAME_BLEND_EQ_RGB, 0x8006);
    s->blendEqAlpha = (uint16_t)Q1(GL_PNAME_BLEND_EQ_ALPHA, 0x8006);

    int sc[4] = {0, 0, 0, 0};
    glstate::GetScissorBox(sc);
    s->scissorX = (int16_t)ClampI16(sc[0]);
    s->scissorY = (int16_t)ClampI16(sc[1]);
    s->scissorW = (int16_t)ClampI16(sc[2]);
    s->scissorH = (int16_t)ClampI16(sc[3]);

    int vp[4] = {0, 0, 0, 0};
    glstate::GetViewportBox(vp);
    s->vpX = (int16_t)ClampI16(vp[0]);
    s->vpY = (int16_t)ClampI16(vp[1]);
    s->vpW = (int16_t)ClampI16(vp[2]);
    s->vpH = (int16_t)ClampI16(vp[3]);

    s->stencilRef = (uint32_t)glstate::GetStencilRef(GL_FRONT_AND_BACK);
    FillFace(GL_FRONT, &s->frontStencil);
    FillFace(GL_BACK, &s->backStencil);

    float cc[4] = {0, 0, 0, 0};
    glstate::GetClearColor(cc);
    for (int i = 0; i < 4; i++) s->clearColor[i] = cc[i];
    s->clearDepth = glstate::GetClearDepth();
    s->clearStencil = glstate::GetClearStencil();

    // GLSL 向量语义下的 Y 翻转由 shader 承担，这里只能是 0（见 spec §5.4）。
    s->positionScaleY = 0;
    s->yFlip = glstate::GetClipSpaceYFlip() ? 1 : 0;
}

// ---------------------------------------------------------------- 属性 / 纹理抓取

static void GatherAttrs (vkpkt::AttrInput out[8], uint16_t* attrCount) {
    for (int i = 0; i < 8; i++) {
        vkpkt::AttrInput& d = out[i];
        memset(&d, 0, sizeof(d));
        AttrSnap snap;
        if (!SnapshotAttr(i, &snap)) {
            d.kind = vkpkt::ATTR_DISABLED;
            continue;
        }
        d.bufferName = snap.bufferName;
        d.byteOffset = snap.byteOffset;
        d.stride = (uint16_t)(snap.stride > 0 ? snap.stride : 0);
        d.size = (uint8_t)(snap.size > 0 ? snap.size : 0);
        d.glType = (uint32_t)(snap.glType < 0 ? 0 : snap.glType);   // 完整 GL 枚举，不许截断
        d.normalized = 0;
        if (!snap.enabled) {
            d.kind = snap.constSet ? vkpkt::ATTR_CONSTANT : vkpkt::ATTR_DISABLED;
        } else if (snap.client) {
            d.kind = vkpkt::ATTR_CLIENT;
        } else {
            d.kind = vkpkt::ATTR_ARRAY;
        }
        if (snap.constSet) {
            for (int k = 0; k < 4; k++) d.constValue[k] = snap.constValue[k];
        }
    }
    uint16_t count = 0;
    for (int i = 0; i < 8; i++) {
        if (out[i].kind != vkpkt::ATTR_DISABLED) count = (uint16_t)(i + 1);
    }
    *attrCount = count;
}

static void GatherTex (int program, vkpkt::TexInput out[8], uint16_t* slots) {
    for (int i = 0; i < 8; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].unit = vkpkt::kTexSlotUnused;
    }
    if (program <= 0) {
        *slots = 0;
        return;
    }
    int n = gl_shader_sampler_count(program);
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        int unit = gl_shader_sampler_unit(program, i);
        if (unit < 0 || unit > 7) {
            out[i].unit = vkpkt::kTexSlotUnused;
            continue;
        }
        int name = SnapshotTexBound(unit);
        out[i].unit = (uint32_t)unit;
        out[i].texName = (uint32_t)(name > 0 ? name : 0);
        out[i].imageId = out[i].texName ? SnapshotTexVk(name) : 0;
        out[i].samplerId = 0;
    }
    *slots = (uint16_t)n;
}

// ---------------------------------------------------------------- arena 物化

static uint32_t ElemBytes (uint8_t typeCode, uint8_t size) {
    uint32_t unit = 0;
    switch (typeCode) {
        case vkpkt::ATYPE_FLOAT: unit = 4; break;
        case vkpkt::ATYPE_UBYTE: unit = 1; break;
        case vkpkt::ATYPE_BYTE: unit = 1; break;
        case vkpkt::ATYPE_USHORT: unit = 2; break;
        case vkpkt::ATYPE_SHORT: unit = 2; break;
        default: unit = 0; break;
    }
    return unit * size;
}

// 把"本次实际读取的顶点区段"拷进帧代 arena。返回 false 表示越界/无主属性，
// 调用方会记一条 err= 日志但继续发 [vkexec]（状态字段仍然要能对账）。
static bool CopyVertexRange (const vkpkt::WirePacket& w, const vkpkt::AttrInput attrs[8],
                             const uint8_t* idxData, uint32_t idxBytes,
                             vkpkt::DrawRecord* rec) {
    // 主属性：(kind=array/client) 且 size>=2 的最低槽，通常是 loc0 的 x,y。
    int primary = -1;
    for (int i = 0; i < 8; i++) {
        if ((attrs[i].kind == vkpkt::ATTR_ARRAY || attrs[i].kind == vkpkt::ATTR_CLIENT) &&
            attrs[i].size >= 2) {
            primary = i;
            break;
        }
    }
    if (primary < 0 || w.count <= 0) return false;

    const vkpkt::AttrInput& a = attrs[primary];
    // 用共享映射（vk_packet.h 里 constexpr + static_assert 护栏），不再本地 switch：
    // 本地 switch 曾因字段被截成 uint8 而恒落 default → err=vb_missing → draws=0（R1）。
    uint32_t typeCode = vkpkt::AttrTypeFromGL(a.glType);
    uint32_t elem = ElemBytes((uint8_t)typeCode, a.size);
    if (!elem) return false;
    uint32_t stride = a.stride ? a.stride : elem;

    // client 指针与 VBO 内容二选一。
    const uint8_t* base = nullptr;
    uint32_t baseBytes = 0;
    if (a.kind == vkpkt::ATTR_CLIENT) {
        AttrSnap snap;
        if (!SnapshotAttr(primary, &snap) || !snap.clientPtr) return false;
        base = (const uint8_t*)snap.clientPtr;
        baseBytes = 0xFFFFFFFFu; // 未知长度：只信 first/count 推导
    } else {
        if (!SnapshotBuffer((int)a.bufferName, &base, &baseBytes) || !base) return false;
    }

    int64_t minIdx = 0, maxIdx = 0;
    if (w.kind == vkpkt::KIND_DRAW_ARRAYS) {
        minIdx = w.first;
        maxIdx = (int64_t)w.first + w.count - 1;
    } else {
        if (!idxData || idxBytes < 2) return false;
        int64_t n = (int64_t)w.count;
        if (n <= 0) return false;
        if ((int64_t)w.first + n > (int64_t)(idxBytes / 2)) return false;
        uint16_t mn = 0xFFFF, mx = 0;
        for (int64_t k = 0; k < n; k++) {
            uint16_t v = idxData[(size_t)(w.first + k) * 2];
            v = (uint16_t)(v | (idxData[(size_t)(w.first + k) * 2 + 1] << 8));
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        minIdx = mn;
        maxIdx = mx;
    }
    if (minIdx < 0) return false;

    int64_t start = (int64_t)a.byteOffset + minIdx * stride;
    int64_t bytes = (maxIdx - minIdx) * (int64_t)stride + (int64_t)elem;
    if (start < 0 || bytes <= 0) return false;
    if (baseBytes != 0xFFFFFFFFu) {
        if ((uint64_t)start >= baseBytes) return false;
        if ((uint64_t)(start + bytes) > baseBytes) {
            bytes = (int64_t)baseBytes - start;
            if (bytes <= 0) return false;
        }
    }

    uint32_t off = 0;
    if (!vkpkt::ArenaAllocCopy(&g_rt.vertex, base + start, (uint32_t)bytes, 4, &off)) {
        return false;
    }
    rec->vertexArenaOff = off;
    rec->vertexArenaBytes = (uint32_t)bytes;
    rec->vertexStride = (uint16_t)stride;
    return true;
}

static void CopyIndexRange (const vkpkt::WirePacket& w, vkpkt::DrawRecord* rec) {
    if (w.kind != vkpkt::KIND_DRAW_ELEMENTS) return;
    if (w.count <= 0) return;
    const uint8_t* data = nullptr;
    uint32_t bytes = 0;
    if (!SnapshotBuffer((int)w.ibo, &data, &bytes) || !data) {
        fprintf(stderr, "[vkexec] err=idx_missing ibo=%u s=%u\n", (unsigned)w.ibo, (unsigned)w.seq);
        return;
    }
    uint64_t start = (uint64_t)w.first * 2u;
    uint64_t need = (uint64_t)w.count * 2u;
    if (start >= bytes || start + need > bytes) {
        fprintf(stderr, "[vkexec] err=idx_bounds first=%d count=%d bytes=%u s=%u\n",
                (int)w.first, (int)w.count, (unsigned)bytes, (unsigned)w.seq);
        return;
    }
    uint32_t off = 0;
    if (!vkpkt::ArenaAllocCopy(&g_rt.index, (const uint8_t*)data + start, (uint32_t)need, 4, &off)) {
        fprintf(stderr, "[vkexec] err=arena index oom bytes=%u s=%u\n", (unsigned)need, (unsigned)w.seq);
        return;
    }
    rec->indexArenaOff = off;
    rec->indexArenaBytes = (uint32_t)need;
    rec->indexVkOffset = off + (uint32_t)start;
}

static void CopyUniform (uint32_t program, uint32_t seq, vkpkt::DrawRecord* rec,
                         const char* uHexFallback) {
    (void)uHexFallback;
    if (program == 0) return;
    const uint8_t* data = nullptr;
    uint32_t bytes = 0;
    if (!gl_shader_uniform_block((int)program, &data, &bytes) || !data || !bytes) return;
    uint32_t n = bytes;
    if (n > vkpkt::kMaxDrawUniform) {
        fprintf(stderr, "[vkexec] err=ubo_over size=%u prog=%u s=%u\n",
                (unsigned)bytes, (unsigned)program, (unsigned)seq);
        n = vkpkt::kMaxDrawUniform;
    }
    uint32_t off = 0;
    if (!vkpkt::ArenaAllocCopy(&g_rt.uniform, data, n, 256, &off)) {
        fprintf(stderr, "[vkexec] err=ubo_oom size=%u prog=%u s=%u\n",
                (unsigned)n, (unsigned)program, (unsigned)seq);
        return;
    }
    rec->uniformOffset = off;
    rec->uniformSize = n;
}

// ---------------------------------------------------------------- 执行（最小链路）
//
// 本版把解码结果送到渲染器唯一一条已经验证过的绘制入口 VkRenderer::DrawRaw：
// 它按"像素坐标 x,y + uv + 逐顶点 alpha"把顶点收进当前渲染目标的批次，在
// PresentTarget 里由 vkCmdBindPipeline/BindDescriptorSets/BindVertexBuffers/CmdDraw 提交。
//
// 为什么不是本文件直接 vkCmd*：VkRenderer 目前只在 PresentTarget 里开 render pass
// 并一次性吃完整个目标的顶点，录制期间没有打开的 pass、也没有暴露 pipeline/
// descriptor/vertex buffer 绑定接口。要"每 draw 立即 vkCmdDraw*"必须先给渲染器
// 加第二条命令路径（那是渲染器改动，不属于 packet 侧），见本轮回报。
//
// 默认关闭：记录侧与 GL 正常路径并行，两套都画会重复叠加。仅调试用 VK_PKT_EXEC=1 打开。
static int ExecEnabled () {
    if (g_rt.exec < 0) {
        const char* v = getenv("VK_PKT_EXEC");
        g_rt.exec = (v && v[0] == '1') ? 1 : 0;
    }
    return g_rt.exec;
}

// 供 GL 侧接线用：执行路径真的在工作时，lime_gl_draw_* 直接返回，避免画两遍。
// 必须同时满足 VK_PKT=1：没有记录侧就没有 packet，只开 VK_PKT_EXEC 会跳过 GL 绘制
// 却没有任何东西可画（全黑）。两个开关一起开才算执行路径生效。
bool ExecActive () { return ExecEnabled() != 0 && lime_vk_pkt_mode() != 0; }

struct AttrRead {
    const uint8_t* base = nullptr;
    uint32_t bytes = 0;     // 0xFFFFFFFF = 长度未知（client 数组）
    uint32_t stride = 0;    // 0 时按 elem 紧凑排布
    uint32_t off = 0;
    uint32_t elem = 0;
    uint32_t type = 0;      // ATYPE_*
    uint32_t size = 0;
};

// 取属性的数据源。与物化阶段的规则一致：array 走 buffer，client 走指针。
static bool ResolveAttrRead (int idx, const vkpkt::PktAttr& a, AttrRead* out) {
    if (a.kind != vkpkt::ATTR_ARRAY && a.kind != vkpkt::ATTR_CLIENT) return false;
    uint32_t elem = ElemBytes((uint8_t)a.type, (uint8_t)a.size);
    if (!elem) return false;
    out->elem = elem;
    out->type = a.type;
    out->size = a.size;
    out->stride = a.stride ? a.stride : elem;
    out->off = a.byteOffset;
    if (a.kind == vkpkt::ATTR_CLIENT) {
        AttrSnap snap;
        if (!SnapshotAttr(idx, &snap) || !snap.clientPtr) return false;
        out->base = (const uint8_t*)snap.clientPtr;
        out->bytes = 0xFFFFFFFFu;
    } else {
        const uint8_t* p = nullptr;
        uint32_t n = 0;
        if (!SnapshotBuffer((int)a.bufferName, &p, &n) || !p) return false;
        out->base = p;
        out->bytes = n;
    }
    return true;
}

// 只解 FLOAT：位置/纹理坐标/alpha 在 lime 的顶点数据里都是 float32。
// 其它类型不解（不猜归一化语义），由调用方打 err=pos_range 并跳过这笔。
static bool ReadFloatComp (const AttrRead& ar, int64_t vidx, int comp, float* out) {
    if (ar.type != vkpkt::ATYPE_FLOAT) return false;
    if (comp < 0 || (uint32_t)comp >= ar.size) return false;
    int64_t pos = (int64_t)ar.off + vidx * (int64_t)ar.stride + (int64_t)comp * 4;
    if (pos < 0) return false;
    if (ar.bytes != 0xFFFFFFFFu && (uint64_t)pos + 4u > ar.bytes) return false;
    memcpy(out, ar.base + pos, 4);
    return true;
}

// ---------------------------------------------------------------- 逐笔 trace（只读诊断）
//
// 开关：VK_TRACE_DRAW=1；只在帧号命中 VK_TRACE_FRAME（默认 5）时打印，
// 单帧最多 VK_TRACE_MAX（默认 64）行。字段与 GL 侧 [vkdraw] 对齐，便于逐笔对账。
// 本段除 fprintf 外不改任何执行行为（不读显存回读，采样色一律 px=no_readback）。

// 帧号列表：VK_TRACE_FRAME 支持逗号分隔（"5,2700"），一次跑就能同时拿标题帧和菜单帧。
static const int kTraceMaxFrames = 8;
static int g_traceFrames[kTraceMaxFrames];
static int g_traceFrameCount = 0;

static int TraceEnv (int* maxOut) {
    static int on = -1;
    static int mx = 64;
    if (on < 0) {
        const char* v = getenv("VK_TRACE_DRAW");
        on = (v && v[0] == '1') ? 1 : 0;
        const char* vf = getenv("VK_TRACE_FRAME");
        const char* vm = getenv("VK_TRACE_MAX");
        if (vm && vm[0]) mx = atoi(vm);
        char buf[256] = {0};
        if (vf && vf[0]) {
            int n = 0;
            const char* p = vf;
            while (*p && n < kTraceMaxFrames) {
                while (*p == ' ' || *p == ',') p++;
                if (!*p) break;
                int v = atoi(p);
                if (v >= 0) g_traceFrames[n++] = v;
                while (*p && *p != ',') p++;
            }
            if (n > 0) {
                g_traceFrameCount = n;
                snprintf(buf, sizeof(buf), "%d", g_traceFrames[0]);
                for (int i = 1; i < n; i++) {
                    size_t l = strlen(buf);
                    snprintf(buf + l, sizeof(buf) - l, ",%d", g_traceFrames[i]);
                }
            }
        }
        if (g_traceFrameCount == 0) { g_traceFrames[0] = 5; g_traceFrameCount = 1; snprintf(buf, sizeof(buf), "5"); }
        if (on) fprintf(stderr, "[trace] enabled frame=%s max=%d\n", buf, mx);
    }
    *maxOut = mx;
    return on;
}

// 本笔是否要打：开关 + 帧号命中（列表任一）+ 本帧预算未用完。
static bool TraceWant () {
    int mx = 64;
    if (!TraceEnv(&mx)) return false;
    bool hit = false;
    for (int i = 0; i < g_traceFrameCount; i++) {
        if ((int)g_rt.frameGen == g_traceFrames[i]) { hit = true; break; }
    }
    if (!hit) return false;
    static uint32_t gen = 0xFFFFFFFFu;
    static int used = 0;
    if (gen != g_rt.frameGen) { gen = g_rt.frameGen; used = 0; }
    if (used >= mx) return false;
    used++;
    return true;
}

struct TraceInfo {
    int texName = 0;
    int tw = 0, th = 0;
    int pixelUv = 0;                          // 1 = 走像素坐标归一化分支
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0;     // 归一化前
    float nu0 = 0, nu1 = 0, nv0 = 0, nv1 = 0; // 实际写进顶点的值
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    int asrc = 0;                             // 0=const 1=arr
    float amin = 1.0f, amax = 1.0f;
};

// 不占 draw 行预算的判定：canvas3x 这种"附加对账行"专用（仍然受帧号列表约束）。
static bool TraceWantExtra () {
    int mx = 64;
    if (!TraceEnv(&mx)) return false;
    for (int i = 0; i < g_traceFrameCount; i++) {
        if ((int)g_rt.frameGen == g_traceFrames[i]) return true;
    }
    return false;
}

static void TraceSkip (const vkpkt::WirePacket& w, const vkpkt::DrawRecord& rec, const char* verdict) {
    if (!TraceWant()) return;
    int texName = 0;
    for (int i = 0; i < (int)vkpkt::kMaxTexSlots; i++) {
        if (rec.tex[i].unit == vkpkt::kTexSlotUnused) continue;
        texName = (int)rec.tex[i].texName;
        break;
    }
    // 几何：尽力从位置属性解析包围盒；clear 行（n=0）/索引绘制/解析失败一律 pos=na
    char posBuf[80];
    snprintf(posBuf, sizeof(posBuf), "pos=na");
    if (w.count > 0 && w.kind != vkpkt::KIND_DRAW_ELEMENTS) {
        int posIdx = -1;
        for (int i = 0; i < (int)vkpkt::kMaxAttrSlots; i++) {
            if ((rec.attrs[i].kind == vkpkt::ATTR_ARRAY || rec.attrs[i].kind == vkpkt::ATTR_CLIENT) &&
                rec.attrs[i].size >= 2) { posIdx = i; break; }
        }
        AttrRead ar;
        if (posIdx >= 0 && ResolveAttrRead(posIdx, rec.attrs[posIdx], &ar)) {
            bool ok = true, first = true;
            float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
            for (int i = 0; i < (int)w.count; i++) {
                float x, y;
                if (!ReadFloatComp(ar, i, 0, &x) || !ReadFloatComp(ar, i, 1, &y)) { ok = false; break; }
                if (first) { x0 = x1 = x; y0 = y1 = y; first = false; }
                else {
                    if (x < x0) x0 = x; if (x > x1) x1 = x;
                    if (y < y0) y0 = y; if (y > y1) y1 = y;
                }
            }
            if (ok && !first) {
                snprintf(posBuf, sizeof(posBuf), "pos=[%.1f..%.1f, %.1f..%.1f]", x0, x1, y0, y1);
            }
        }
    }
    fprintf(stderr, "[trace] f=%u prog=%u s=%u n=%d tex=%d %s px=no_readback verdict=%s\n",
            (unsigned)g_rt.frameGen, (unsigned)w.program, (unsigned)w.seq, (int)w.count,
            texName, posBuf, verdict);
}

static void TracePrint (const vkpkt::WirePacket& w, const vkpkt::DrawRecord& rec,
                        const TraceInfo& ti, const PktDrawDesc& d, const char* verdict) {
    if (!TraceWant()) return;
    bool whole = (!ti.pixelUv && ti.u0 <= 0.001f && ti.u1 >= 0.999f &&
                  ti.v0 <= 0.001f && ti.v1 >= 0.999f);
    // sc_src：pkt=本笔画了有效裁剪框；default=本笔没带框，沿用 pass 打开时下发的那一次
    const char* scSrc = d.scissor.valid ? "pkt" : "default";
    fprintf(stderr,
            "[trace] f=%u prog=%u s=%u n=%d tex=%d tsz=%dx%d "
            "uv=[%.3f..%.3f, %.3f..%.3f] nuv=[%.4f..%.4f, %.4f..%.4f] "
            "pos=[%.1f..%.1f, %.1f..%.1f] norm=%d whole=%d "
            "asrc=%s amin=%.3f amax=%.3f blend=%d(%u,%u) "
            "vp=%d,%d,%d,%d,%d sc=%d,%d,%d,%d,%d sc_src=%s "
            "depth=%d depthcmp=0x%x dwrite=%d px=no_readback verdict=%s\n",
            (unsigned)g_rt.frameGen, (unsigned)w.program, (unsigned)w.seq, (int)w.count,
            ti.texName, ti.tw, ti.th,
            ti.u0, ti.u1, ti.v0, ti.v1,
            ti.nu0, ti.nu1, ti.nv0, ti.nv1,
            ti.x0, ti.x1, ti.y0, ti.y1,
            ti.pixelUv, whole ? 1 : 0,
            ti.asrc ? "arr" : "const", ti.amin, ti.amax,
            (int)d.state.blendEnabled, (unsigned)d.state.blendSrc, (unsigned)d.state.blendDst,
            (int)d.viewport.valid, (int)d.viewport.x, (int)d.viewport.y,
            (int)d.viewport.w, (int)d.viewport.h,
            (int)d.scissor.valid, (int)d.scissor.x, (int)d.scissor.y,
            (int)d.scissor.w, (int)d.scissor.h,
            scSrc, (int)d.state.depthTest, (unsigned)rec.depthFunc, (int)d.state.depthWrite,
            verdict);
}

static bool ExecuteDraw (const vkpkt::WirePacket& w, const vkpkt::DrawRecord& rec, uint32_t rtId) {
    VkRenderer* r = gl_vk_get_renderer();
    if (!r) return false;
    // 清屏行（n=0）：pass 已在 BindRenderTarget 时打开，这里只做一次幂等对齐，
    // 保证执行日志与 packet 一一对应（没有绘制命令要发）。
    if (w.count <= 0) {
        TraceSkip(w, rec, "skip_n0");
        // 只有当 clearMask 含颜色位时才算"要清色"；纯深度/模板 clear 不能整幅抹掉颜色
        bool colorClear = (rec.colorMaskBits & 0x0F) != 0;
        return r->PktBeginTarget(rtId, rec.clearColorF[0], rec.clearColorF[1],
                                 rec.clearColorF[2], rec.clearColorF[3], colorClear);
    }
    if (w.kind != vkpkt::KIND_DRAW_ARRAYS && w.kind != vkpkt::KIND_DRAW_ELEMENTS) return false;

    // 采样源：第一个 unit 有效且能解析到 Vk 纹理的槽。
    uint32_t vkTex = 0;
    for (int i = 0; i < (int)vkpkt::kMaxTexSlots; i++) {
        if (rec.tex[i].unit == vkpkt::kTexSlotUnused) continue;
        uint32_t id = rec.tex[i].imageId;
        if (!id && rec.tex[i].texName) id = SnapshotTexVk((int)rec.tex[i].texName);
        if (id) { vkTex = id; break; }
    }
    if (!vkTex) {
        static int s_noTex = 0;
        if (s_noTex < 6 || s_noTex % 500 == 0) {
            fprintf(stderr, "[vkexec] err=no_tex prog=%u s=%u\n", (unsigned)w.program, (unsigned)w.seq);
        }
        s_noTex++;
        TraceSkip(w, rec, "skip_tex");
        return false;
    }

    // 槽位：loc0=Position，loc4=TexCoord（lime 的实际约定），loc5=逐顶点 alpha。
    int posIdx = -1;
    for (int i = 0; i < (int)vkpkt::kMaxAttrSlots; i++) {
        if ((rec.attrs[i].kind == vkpkt::ATTR_ARRAY || rec.attrs[i].kind == vkpkt::ATTR_CLIENT) &&
            rec.attrs[i].size >= 2) { posIdx = i; break; }
    }
    int uvIdx = -1;
    for (int i = 4; i >= 1; i -= 3) {
        if ((rec.attrs[i].kind == vkpkt::ATTR_ARRAY || rec.attrs[i].kind == vkpkt::ATTR_CLIENT) &&
            rec.attrs[i].size >= 2) { uvIdx = i; break; }
    }
    int alphaIdx = -1;
    if ((rec.attrs[5].kind == vkpkt::ATTR_ARRAY || rec.attrs[5].kind == vkpkt::ATTR_CLIENT) &&
        rec.attrs[5].size >= 1) alphaIdx = 5;
    if (posIdx < 0) {
        fprintf(stderr, "[vkexec] err=no_pos prog=%u s=%u\n", (unsigned)w.program, (unsigned)w.seq);
        TraceSkip(w, rec, "skip_no_pos");
        return false;
    }

    AttrRead pos, uv, alpha;
    if (!ResolveAttrRead(posIdx, rec.attrs[posIdx], &pos)) {
        fprintf(stderr, "[vkexec] err=pos_src idx=%d buf=%u s=%u\n",
                posIdx, (unsigned)rec.attrs[posIdx].bufferName, (unsigned)w.seq);
        TraceSkip(w, rec, "skip_pos_src");
        return false;
    }
    if (uvIdx >= 0 && !ResolveAttrRead(uvIdx, rec.attrs[uvIdx], &uv)) uvIdx = -1;
    if (alphaIdx >= 0 && !ResolveAttrRead(alphaIdx, rec.attrs[alphaIdx], &alpha)) alphaIdx = -1;

    // 索引：spec 约定 16 位（CopyIndexRange 同样按 2 字节取用）。
    const uint8_t* idx = nullptr;
    uint32_t idxBytes = 0;
    if (w.kind == vkpkt::KIND_DRAW_ELEMENTS) {
        if (!SnapshotBuffer((int)w.ibo, &idx, &idxBytes) || !idx) {
            fprintf(stderr, "[vkexec] err=idx_src ibo=%u s=%u\n", (unsigned)w.ibo, (unsigned)w.seq);
            TraceSkip(w, rec, "skip_idx_src");
            return false;
        }
    }

    std::vector<float> verts((size_t)w.count * 4, 0.0f);
    std::vector<float> alphas;
    if (alphaIdx >= 0) alphas.assign((size_t)w.count, 1.0f);
    float uMin = 0, uMax = 0, vMin = 0, vMax = 0;
    bool firstUv = true;
    // trace 用：几何与 alpha 的实际取值范围（只读累加，不参与任何计算）
    float xMin = 0, xMax = 0, yMin = 0, yMax = 0;
    bool firstPos = true;
    float aMin = 1.0f, aMax = 1.0f;

    for (int i = 0; i < w.count; i++) {
        int64_t vidx = 0;
        if (idx) {
            uint64_t at = ((uint64_t)w.first + (uint64_t)i) * 2u;
            if (at + 2u > idxBytes) return false;
            uint16_t v = 0;
            memcpy(&v, idx + at, 2);
            vidx = (int64_t)v;
        } else {
            vidx = (int64_t)w.first + i;
        }
        float x = 0.0f, y = 0.0f, u = 0.0f, vv = 0.0f;
        if (!ReadFloatComp(pos, vidx, 0, &x) || !ReadFloatComp(pos, vidx, 1, &y)) {
            static int s_bad = 0;
            if (s_bad < 6) {
                fprintf(stderr, "[vkexec] err=pos_range idx=%d vidx=%lld s=%u\n",
                        posIdx, (long long)vidx, (unsigned)w.seq);
            }
            s_bad++;
            TraceSkip(w, rec, "skip_pos_range");
            return false;
        }
        if (uvIdx >= 0) {
            ReadFloatComp(uv, vidx, 0, &u);
            ReadFloatComp(uv, vidx, 1, &vv);
        }
        if (alphaIdx >= 0) {
            float a = 1.0f;
            ReadFloatComp(alpha, vidx, 0, &a);
            alphas[(size_t)i] = a;
            if (a < aMin) aMin = a;
            if (a > aMax) aMax = a;
        }
        verts[(size_t)i * 4 + 0] = x;
        verts[(size_t)i * 4 + 1] = y;
        verts[(size_t)i * 4 + 2] = u;
        verts[(size_t)i * 4 + 3] = vv;
        if (firstPos) { xMin = xMax = x; yMin = yMax = y; firstPos = false; }
        if (x < xMin) xMin = x;
        if (x > xMax) xMax = x;
        if (y < yMin) yMin = y;
        if (y > yMax) yMax = y;
        if (firstUv) { uMin = uMax = u; vMin = vMax = vv; firstUv = false; }
        if (u < uMin) uMin = u;
        if (u > uMax) uMax = u;
        if (vv < vMin) vMin = vv;
        if (vv > vMax) vMax = vv;
    }

    int texName = 0;
    for (int i = 0; i < (int)vkpkt::kMaxTexSlots; i++) {
        if (rec.tex[i].unit == vkpkt::kTexSlotUnused) continue;
        texName = (int)rec.tex[i].texName;
        break;
    }

    // uv 口径与批处理完全一致：像素坐标要半像素内缩再按采样源尺寸归一化。
    int tw = 0, th = 0;
    if (vkTex >= 1000) r->GetRenderTargetSize(vkTex, &tw, &th);
    else if (texName > 0) gl_vk_texture_size(texName, &tw, &th);
    bool pixelUv = (uMax > 1.001f || uMin < -1.001f || vMax > 1.001f || vMin < -1.001f);

    // 常量属性：loc2=colorMultiplier，loc3=colorOffset；loc1/loc5 是 alpha 的两个乘数。
    float mult[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float off[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (rec.attrs[2].kind == vkpkt::ATTR_CONSTANT && rec.attrs[2].size >= 4) {
        for (int k = 0; k < 4; k++) mult[k] = rec.attrs[2].constValue[k];
    }
    if (rec.attrs[3].kind == vkpkt::ATTR_CONSTANT && rec.attrs[3].size >= 4) {
        for (int k = 0; k < 4; k++) off[k] = rec.attrs[3].constValue[k];
    }
    float alphaConst = 1.0f;
    if (rec.attrs[1].kind == vkpkt::ATTR_CONSTANT) alphaConst *= rec.attrs[1].constValue[0];
    if (rec.attrs[5].kind == vkpkt::ATTR_CONSTANT) alphaConst *= rec.attrs[5].constValue[0];
    if (alphaIdx < 0) { aMin = aMax = alphaConst; }

    std::vector<float> packed((size_t)w.count * 15);
    // trace 用：真正写进顶点的 uv 范围（只读累加，不参与任何计算）
    float nuMin = 0, nuMax = 0, nvMin = 0, nvMax = 0;
    bool firstNorm = true;
    for (int i = 0; i < w.count; i++) {
        float u = verts[(size_t)i * 4 + 2];
        float v = verts[(size_t)i * 4 + 3];
        if (pixelUv) {
            float eu = u, ev = v;
            if (u <= uMin + 1e-3f && u < uMax) eu = u + 0.5f;
            else if (u >= uMax - 1e-3f) eu = u - 0.5f;
            if (v <= vMin + 1e-3f && v < vMax) ev = v + 0.5f;
            else if (v >= vMax - 1e-3f) ev = v - 0.5f;
            u = tw > 0 ? eu / tw : 1.0f;
            v = th > 0 ? ev / th : 1.0f;
        } else {
            if (u > 1.001f || u < -1.001f) u = tw > 0 ? u / tw : 1.0f;
            if (v > 1.001f || v < -1.001f) v = th > 0 ? v / th : 1.0f;
        }
        float* o15 = packed.data() + (size_t)i * 15;
        o15[0] = verts[(size_t)i * 4 + 0];
        o15[1] = verts[(size_t)i * 4 + 1];
        o15[2] = 0.0f;
        o15[3] = 1.0f;
        o15[4] = alphas.empty() ? alphaConst : alphas[(size_t)i] * alphaConst;
        for (int k = 0; k < 4; k++) o15[5 + k] = mult[k];
        for (int k = 0; k < 4; k++) o15[9 + k] = off[k];
        o15[13] = u;
        o15[14] = v;
        if (firstNorm) { nuMin = nuMax = u; nvMin = nvMax = v; firstNorm = false; }
        if (u < nuMin) nuMin = u;
        if (u > nuMax) nuMax = u;
        if (v < nvMin) nvMin = v;
        if (v > nvMax) nvMax = v;
    }

    // 索引按展开后的顺序给 0..n-1：顶点已经按原索引顺序展开，视觉结果与原索引一致。
    // 优先 16 位；超过 65535 走 32 位；两种都装不下（不会发生）才退化为非索引。
    std::vector<uint16_t> seq;
    std::vector<uint32_t> seq32;
    bool indexed = (w.kind == vkpkt::KIND_DRAW_ELEMENTS);
    bool use32 = false;
    if (indexed) {
        if (w.count <= 65535) {
            seq.resize((size_t)w.count);
            for (int i = 0; i < w.count; i++) seq[(size_t)i] = (uint16_t)i;
        } else {
            seq32.resize((size_t)w.count);
            for (int i = 0; i < w.count; i++) seq32[(size_t)i] = (uint32_t)i;
            use32 = true;
        }
    }

    // 状态位按 record 的快照填；PktDraw 按这些位选管线变体（默认状态复用现有管线）。
    PktDrawDesc d{};
    d.texId = vkTex;
    d.verts = packed.data();
    d.vertexCount = w.count;
    d.indices = seq.empty() ? nullptr : seq.data();
    d.indices32 = seq32.empty() ? nullptr : seq32.data();
    d.indexCount = (int)(seq.size() ? seq.size() : seq32.size());
    d.index32 = use32 ? 1 : 0;
    d.state.blendEnabled = rec.blendEnabled;
    d.state.blendSrc = NormBlendFactor(rec.blendSrcRgb);
    d.state.blendDst = NormBlendFactor(rec.blendDstRgb);
    d.state.depthTest = rec.depthTestEnabled;
    d.state.depthWrite = rec.depthMask;
    d.state.stencilTest = 0;
    d.state.cullMode = rec.cullMode;
    d.state.frontFace = rec.frontFace;
    d.state.scissorTest = rec.scissorEnabled;
    d.state.colorMask = rec.colorMaskBits;

    // 采样器：记录侧的 draw_record 没有采样器字段，改从 GL 参数镜像按纹理名取
    // （与 texParameteri 同源，等于这一笔真正生效的过滤/wrap）。
    int fMin = 0, fMag = 0, wS = 0, wT = 0;
    if (texName > 0 && gl_vk_texture_sampler_state(texName, &fMin, &fMag, &wS, &wT)) {
        d.sampler.minFilter = NormMinFilter(fMin);
        d.sampler.magFilter = NormMagFilter(fMag);
        d.sampler.wrapU = NormWrap(wS);
        d.sampler.wrapV = NormWrap(wT);
    } else {
        d.sampler.minFilter = 0xFF;
        d.sampler.magFilter = 0xFF;
        d.sampler.wrapU = 0xFF;
        d.sampler.wrapV = 0xFF;
    }

    // viewport / scissor 逐笔：零尺寸或未启用时 valid=0，沿用 pass 打开时的下发值
    d.viewport.valid = (rec.vpW > 0 && rec.vpH > 0) ? 1 : 0;
    d.viewport.x = (int16_t)rec.vpX;
    d.viewport.y = (int16_t)rec.vpY;
    d.viewport.w = (int16_t)rec.vpW;
    d.viewport.h = (int16_t)rec.vpH;
    d.scissor.valid = (rec.scissorEnabled && rec.scissorW > 0 && rec.scissorH > 0) ? 1 : 0;
    d.scissor.x = (int16_t)rec.scissorX;
    d.scissor.y = (int16_t)rec.scissorY;
    d.scissor.w = (int16_t)rec.scissorW;
    d.scissor.h = (int16_t)rec.scissorH;
    // trace（只读）：把这一笔的实际取值原样打出来，字段与 GL 侧 [vkdraw] 对齐
    TraceInfo ti;
    ti.texName = texName;
    ti.tw = tw; ti.th = th;
    ti.pixelUv = pixelUv ? 1 : 0;
    ti.u0 = uMin; ti.u1 = uMax; ti.v0 = vMin; ti.v1 = vMax;
    ti.nu0 = nuMin; ti.nu1 = nuMax; ti.nv0 = nvMin; ti.nv1 = nvMax;
    ti.x0 = xMin; ti.x1 = xMax; ti.y0 = yMin; ti.y1 = yMax;
    ti.asrc = (alphaIdx >= 0) ? 1 : 0;
    ti.amin = aMin; ti.amax = aMax;

    // 兜底：正常流程里 pass 在 BindRenderTarget 时就开了；这里按 packet 里的目标
    // 再对齐一次（同目标幂等不重清，换目标会先把上一笔 pass 收尾再开新的）。
    if (!r->PktBeginTarget(rtId, rec.clearColorF[0], rec.clearColorF[1],
                           rec.clearColorF[2], rec.clearColorF[3])) return false;
    if (!r->PktDraw(d)) {
        TracePrint(w, rec, ti, d, "skip_pipeline");
        return false;
    }
    TracePrint(w, rec, ti, d, "drawn");
    // 对账行：这笔搬的是离屏画布（>=1000）时，直接给出"清没清 + 谁在用"。
    if (vkTex >= 1000 && TraceWantExtra()) {
        fprintf(stderr, "[trace] canvas3x target=%u cleared=%d used_by_s=%u\n",
                vkTex, r->PktTargetClearedThisFrame(vkTex) ? 1 : 0, (unsigned)w.seq);
    }

    g_rt.executed++;
    if (g_rt.executed <= 4 || g_rt.executed % 500 == 0) {
        fprintf(stderr, "[vkexec] exec=1 tex=%d vk=%u n=%d prog=%u s=%u idx=%d i%d\n",
                texName, (unsigned)vkTex, (int)w.count, (unsigned)w.program, (unsigned)w.seq,
                d.indexCount, use32 ? 32 : 16);
    }
    return true;
}

} // namespace pkt
} // namespace lime

// ---------------------------------------------------------------- CFFI 入口

namespace lime {

using namespace pkt;

int lime_vk_pkt_mode () {
    if (g_rt.enabled < 0) {
        const char* v = getenv("VK_PKT");
        g_rt.enabled = (v && v[0] == '1') ? 1 : 0;
        if (g_rt.enabled) EnsureArenas();
    }
    return g_rt.enabled;
}

int lime_vk_pkt_utag () {
    if (!lime_vk_pkt_mode()) return 0;
    int program = gl_shader_current_program();
    if (program <= 0) {
        g_rt.utag = 0;
        g_rt.utagValid = true;
        return 0;
    }
    const uint8_t* data = nullptr;
    uint32_t bytes = 0;
    if (!gl_shader_uniform_block(program, &data, &bytes) || !data || !bytes) {
        g_rt.utag = 0;
    } else {
        g_rt.utag = vkpkt::UniformTag(data, bytes);
    }
    g_rt.utagValid = true;
    return (int)g_rt.utag;
}

int lime_vk_pkt_frame (int frameGen, double animTime) {
    if (!lime_vk_pkt_mode()) return 0;
    (void)animTime; // dump 时机见 spec §6，此处先只维护帧代
    if (g_rt.frameGen != (uint32_t)frameGen) {
        g_rt.frameGen = (uint32_t)frameGen;
        ResetArenas();
        g_rt.nextSeq = 1;
    }
    return 0;
}

int lime_vk_pkt_emit (double pointer, int bytes) {
    if (!lime_vk_pkt_mode()) return 0;

    if (!pointer || bytes != (int)vkpkt::kWireBytes) {
        fprintf(stderr, "[vkexec] err=pkt_bytes bytes=%d\n", bytes);
        g_rt.rejected++;
        return -1;
    }

    // CFFI 约定：指针经 Float(double) 传递，先落整数再转指针（arm64）。
    const uint8_t* pkt = (const uint8_t*)(uintptr_t)(int64_t)pointer;

    vkpkt::WirePacket w;
    char err[64];
    if (!vkpkt::ParseWire(pkt, (uint32_t)bytes, &w, err, sizeof(err))) {
        fprintf(stderr, "[vkexec] err=pkt_reject reason=%s\n", err);
        g_rt.rejected++;
        return -1;
    }

    if (w.frameGen != g_rt.frameGen) {
        g_rt.frameGen = w.frameGen;
        ResetArenas();
        g_rt.nextSeq = (w.seq > 0) ? w.seq : 1;
    }

    if (w.seq != g_rt.nextSeq) {
        fprintf(stderr, "[vkexec] err=seq got=%u want=%u f=%u\n",
                (unsigned)w.seq, (unsigned)g_rt.nextSeq, (unsigned)w.frameGen);
        g_rt.nextSeq = w.seq;
    }
    g_rt.nextSeq = w.seq + 1;

    // ---- 抓取 + 物化
    vkpkt::MaterializeInput in;
    memset(&in, 0, sizeof(in));
    in.wire = w;
    in.stateVersion = (uint32_t)glstate::Version();
    // 记录侧固定写 0（Haxe 取不到 g_version，见 VKDrawSink.writePacket:139）。
    // 0 = "未提供"，不做对账；非 0 时才按旧规则校验低 8 位。
    if (w.stateVersion != 0 && w.stateVersion != (uint8_t)(in.stateVersion & 0xFF)) {
        fprintf(stderr, "[vkexec] err=state_version got=%u want=%u s=%u\n",
                (unsigned)w.stateVersion, (unsigned)(in.stateVersion & 0xFF), (unsigned)w.seq);
    }

    VkRenderer* r = gl_vk_get_renderer();
    in.rtImage = gl_vk_fbo_target((int)w.fbo);
    if (in.rtImage >= 1000) {
        int rw = 0, rh = 0;
        if (r && r->GetRenderTargetSize(in.rtImage, &rw, &rh)) {
            in.rtW = (uint16_t)(rw > 0 ? rw : 0);
            in.rtH = (uint16_t)(rh > 0 ? rh : 0);
        }
    } else if (r) {
        in.rtW = (uint16_t)(r->ScreenWidth() > 0 ? r->ScreenWidth() : 0);
        in.rtH = (uint16_t)(r->ScreenHeight() > 0 ? r->ScreenHeight() : 0);
    }

    GatherAttrs(in.attrs, &in.attrCount);
    vkpkt::TexInput tex[8];
    uint16_t slots = 0;
    GatherTex((int)w.program, tex, &slots);
    for (int i = 0; i < 8; i++) in.tex[i] = tex[i];
    GatherState(&in.state);

    vkpkt::DrawRecord rec;
    vkpkt::Materialize(in, &rec);

    // ---- 帧代 arena（立即消费：数据在这笔调用里就复制完成）
    if (g_rt.arenasReady) {
        const uint8_t* idxData = nullptr;
        uint32_t idxBytes = 0;
        if (w.kind == vkpkt::KIND_DRAW_ELEMENTS && w.ibo) {
            SnapshotBuffer((int)w.ibo, &idxData, &idxBytes);
        }
        CopyIndexRange(w, &rec);
        CopyUniform(w.program, w.seq, &rec, nullptr);
        if (!CopyVertexRange(w, in.attrs, idxData, idxBytes, &rec)) {
            if (w.kind != vkpkt::KIND_CLEAR && w.count > 0) {
                fprintf(stderr, "[vkexec] err=vb_missing first=%d count=%d prog=%u s=%u\n",
                        (int)w.first, (int)w.count, (unsigned)w.program, (unsigned)w.seq);
            }
        }
        g_rt.decoded++;
    }

    // ---- 执行：packet 独立路径，一笔一笔立即发 vkCmd（默认关，VK_PKT_EXEC=1 才执行）
    bool executed = false;
    if (ExecEnabled()) executed = ExecuteDraw(w, rec, in.rtImage);

    // ---- u：原样回填（lime_vk_pkt_utag 的缓存值）
    uint32_t u = 0;
    if (g_rt.utagValid) {
        u = g_rt.utag;
        g_rt.utagValid = false;
    } else if (w.program > 0) {
        const uint8_t* data = nullptr;
        uint32_t ub = 0;
        if (gl_shader_uniform_block((int)w.program, &data, &ub) && data && ub) {
            u = vkpkt::UniformTag(data, ub);
        }
    }
    char uHex[9];
    vkpkt::Hex8(u, uHex);

    // ---- [vkexec]
    // pending=0：这笔已经立即发进当前命令缓冲（或清屏行已在 pass 里落定）；
    // pending=1：只解码，没有发命令。
    if (GateOpen(w.frameGen)) {
        char line[1024];
        vkpkt::BuildLogLine("[vkexec]", rec, uHex, executed ? "pending=0" : "pending=1", line, sizeof(line));
        fputs(line, stderr);
        fputc('\n', stderr);
    }

    return 1;
}

} // namespace lime
