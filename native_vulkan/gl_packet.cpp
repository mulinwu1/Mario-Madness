#include "gl_packet.h"
#include "vk_packet.h"

#include "gl_state.h"
#include "gl_shader.h"
#include "vk_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// gl_vk_bridge.h 会拉入 gl_compat.h（hxcpp/CFFI），这里只需要两个函数，单decl。
namespace lime {
VkRenderer* gl_vk_get_renderer ();
uint32_t gl_vk_fbo_target (int fboName);
}

namespace lime {
namespace pkt {

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
        d.glType = (uint8_t)(snap.glType & 0xFF);
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
    uint32_t typeCode = 255;
    switch (a.glType) {
        case 0x1406: typeCode = vkpkt::ATYPE_FLOAT; break;
        case 0x1401: typeCode = vkpkt::ATYPE_UBYTE; break;
        case 0x1400: typeCode = vkpkt::ATYPE_BYTE; break;
        case 0x1403: typeCode = vkpkt::ATYPE_USHORT; break;
        case 0x1402: typeCode = vkpkt::ATYPE_SHORT; break;
        default: typeCode = vkpkt::ATYPE_UNKNOWN; break;
    }
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
    if (w.stateVersion != (uint8_t)(in.stateVersion & 0xFF)) {
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
    // 说明：本版只做"解码 + 物化"，尚未把 vkCmdDraw* 编进命令缓冲（见 STATUS）。
    // pending=1 是对账工具忽略的附加字段，明确表示这条不是已提交的 GPU 命令。
    if (GateOpen(w.frameGen)) {
        char line[1024];
        vkpkt::BuildLogLine("[vkexec]", rec, uHex, "pending=1", line, sizeof(line));
        fputs(line, stderr);
        fputc('\n', stderr);
    }

    return 1;
}

} // namespace lime
