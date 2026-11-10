#include "gl_compat.h"
#include "vk_renderer.h"
#include "gl_vk_bridge.h"
#include "gl_vk_contract.h"
#include "gl_state.h"
#include "gl_packet.h"
#include "gl_shader.h"

#include <vector>
#include <map>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace lime {

// 诊断用：当前 GL program（gl_shader.cpp 实现）
int gl_shader_current_program ();

static VkRenderer* g_renderer = nullptr;
static uint64_t g_draws = 0;
static uint64_t g_presents = 0;

// 硬失败：渲染器缺失 / 初始化失败时打一行可检索日志再退出，绝不静默黑屏。
static void vk_fatal (const char* what) {
    fprintf (stderr, "[vkerr] fatal %s\n", what);
    fflush (stderr);
    exit (1);
}

static float g_clearR = 0.0f, g_clearG = 0.0f, g_clearB = 0.0f, g_clearA = 1.0f;
static int g_vpX = 0, g_vpY = 0, g_vpW = 0, g_vpH = 0;

static bool g_blendEnabled = false;
static int g_blendSrc = 0, g_blendDst = 0;
static bool g_colorMask[4] = {true, true, true, true};
static int g_scissorX = 0, g_scissorY = 0, g_scissorW = 0, g_scissorH = 0;

// GL texture name -> VkRenderer texture id (0 = not uploaded yet)
static std::vector<uint32_t> g_texVk;
// 纹理尺寸：FBO 完整性判定 / 离屏目标尺寸都要用，必须在 data==nullptr 提前返回之前就记录
static std::vector<int> g_texW;
static std::vector<int> g_texH;
static int g_nextTexName = 1;
static std::vector<int> g_texBound; // per texture unit
static int g_activeUnit = 0;

// pending texImage2D parameters, applied on first real upload
struct PendingTex {
    int level = 0;
    int internalformat = 0;
    int w = 0, h = 0;
    int format = 0;
    int type = 0;
    std::vector<unsigned char> pixels;
};
static std::vector<PendingTex> g_pending;

static void EnsureSize(std::vector<uint32_t>& v, size_t n, uint32_t def) {
    if (v.size() < n) v.resize(n, def);
}
static void EnsureSizeI(std::vector<int>& v, size_t n, int def) {
    if (v.size() < n) v.resize(n, def);
}

// 纹理槽位被释放/替换时，把所有仍指向它的 GL 名字一并清 0。
// 槽位 id 会被 CreateTexture 复用；若不清理旧名字，后续绑定这些名字的绘制
// 会采样到“别人的图”（症状：整层贴图错乱、菜单背景变成另一张图）。
static void ClearTexNameMappings(uint32_t vkId) {
    if (!vkId) return;
    for (size_t i = 0; i < g_texVk.size(); i++) {
        if (g_texVk[i] == vkId) g_texVk[i] = 0;
    }
}

// ---------- 与 gl_fbo / gl_texture_params 的状态契约 ----------
static uint32_t S_STATE_getTextureId(int glName) {
    if (glName <= 0 || glName >= (int)g_texVk.size()) return 0;
    return g_texVk[(size_t)glName];
}
static void S_STATE_setTextureId(int glName, uint32_t vkId) {
    if (glName <= 0) return;
    EnsureSize(g_texVk, (size_t)glName + 1, 0);
    g_texVk[(size_t)glName] = vkId;
}
static int S_STATE_getBoundTexture() {
    if (g_activeUnit < 0 || g_activeUnit >= (int)g_texBound.size()) return 0;
    return g_texBound[(size_t)g_activeUnit];
}
static bool S_STATE_getTextureSize(int glName, int* w, int* h) {
    if (glName <= 0 || glName >= (int)g_texW.size()) return false;
    int tw = g_texW[(size_t)glName];
    int th = g_texH[(size_t)glName];
    if (tw <= 0 || th <= 0) return false;
    if (w) *w = tw;
    if (h) *h = th;
    return true;
}
static void S_STATE_getClearColor(float* rgba) {
    if (!rgba) return;
    rgba[0] = g_clearR;
    rgba[1] = g_clearG;
    rgba[2] = g_clearB;
    rgba[3] = g_clearA;
}
static const GlVkCoreState S_STATE = {
    S_STATE_getTextureId, S_STATE_setTextureId, S_STATE_getBoundTexture,
    S_STATE_getTextureSize, S_STATE_getClearColor
};

// ---------- 转发到 VkRenderer 的扩展能力（未实现时返回 false/0，不谎报成功） ----------
static bool EXT_setTextureSampler(uint32_t texId, int minFilter, int magFilter,
                                  int wrapS, int wrapT, int baseLevel, int maxLevel,
                                  float minLod, float maxLod) {
    return g_renderer ? g_renderer->SetTextureSampler(texId, minFilter, magFilter, wrapS,
                                                      wrapT, baseLevel, maxLevel, minLod, maxLod) : false;
}
static bool EXT_updateSubImage(uint32_t texId, int level, int x, int y, int w, int h,
                               const unsigned char* rgba) {
    return g_renderer ? g_renderer->UpdateSubImage(texId, level, x, y, w, h, rgba) : false;
}
static uint32_t EXT_createEmptyTexture(int w, int h) {
    return g_renderer ? g_renderer->CreateEmptyTexture(w, h) : 0;
}
static bool EXT_generateMipmaps(uint32_t texId, int maxLevel) {
    return g_renderer ? g_renderer->GenerateMipmaps(texId, maxLevel) : false;
}
static uint32_t EXT_createRenderTargetEx(int w, int h, bool withDepth) {
    return g_renderer ? g_renderer->CreateRenderTargetEx(w, h, withDepth) : 0;
}
static bool EXT_readPixels(uint32_t targetId, int x, int y, int w, int h, unsigned char* rgba) {
    return g_renderer ? g_renderer->ReadPixels(targetId, x, y, w, h, rgba) : false;
}
static const GlVkRendererExt EXT = {
    EXT_setTextureSampler, EXT_updateSubImage, EXT_createEmptyTexture,
    EXT_generateMipmaps, EXT_createRenderTargetEx, EXT_readPixels
};

void gl_vk_attach_renderer(VkRenderer* r) { g_renderer = r; }
VkRenderer* gl_vk_get_renderer() { return g_renderer; }

// 由 SDLWindow 在窗口创建后调用，激活 Vulkan 后端
void gl_vk_init(void* nativeWindow, int width, int height) {
    if (g_renderer) return;
    if (!nativeWindow || width <= 0 || height <= 0) {
        vk_fatal ("init_invalid_window");
    }
    VkRenderer* r = new VkRenderer();
    if (r->Init(nativeWindow, width, height)) {
        g_renderer = r;
        fprintf(stderr, "[glvk] renderer ready %dx%d window=%p\n", width, height, nativeWindow);
        // FBO / 纹理参数模块靠这两个契约拿到纹素与清屏色
        gl_vk_install_core_state(&S_STATE);
        gl_vk_install_renderer_ext(&EXT);
        // 状态机：视口/剪裁默认整窗；非法 blend 组合的警告走 stderr（构建日志里可见）
        glstate::ResetToDefaults(width, height);
        glstate::SetWarningCallback(nullptr);
    } else {
        fprintf(stderr, "[vkerr] init_failed reason=%s\n", r->LastError());
        vk_fatal ("init_failed");
    }
}

bool gl_vk_active() { return g_renderer != nullptr; }

// 由 SDLWindow::ContextFlip 每帧末调用，提交并 present
void gl_vk_end_frame() {
    // 统一走 gl_fbo 的帧收尾：它负责离屏目标与后缓冲的切换，再调 VkRenderer::EndFrame
    gl_vk_present_frame();
    g_presents++;
    if (g_presents % 60 == 0) {
        fprintf(stderr, "[glvk] presents=%llu draws=%llu\n",
                (unsigned long long)g_presents, (unsigned long long)g_draws);
    }
}

void gl_vk_shutdown() {
    if (g_renderer) {
        g_renderer->Shutdown();
        delete g_renderer;
        g_renderer = nullptr;
    }
}

void lime_gl_clear_color(float red, float green, float blue, float alpha) {
    g_clearR = red;
    g_clearG = green;
    g_clearB = blue;
    g_clearA = alpha;
    glstate::ClearColor(red, green, blue, alpha);
}

void lime_gl_clear(int mask) {
    if (!g_renderer) vk_fatal ("renderer_null fn=lime_gl_clear");
    // 离屏目标由 gl_fbo 负责清除；返回 false 说明当前画在后缓冲上，需要自己开帧
    if (gl_vk_handle_clear()) return;
    // 只开启一帧，真正的提交在 ContextFlip 里做
    g_renderer->BeginFrame(g_clearR, g_clearG, g_clearB, g_clearA);
}

void lime_gl_viewport(int x, int y, int width, int height) {
    g_vpX = x;
    g_vpY = y;
    g_vpW = width;
    g_vpH = height;
    glstate::Viewport(x, y, width, height);
}

// GL_BLEND = 0x0BE2
void lime_gl_enable(int cap) {
    glstate::SetCapability(cap, true);
    if (cap == 0x0BE2) g_blendEnabled = true;
}

void lime_gl_disable(int cap) {
    glstate::SetCapability(cap, false);
    if (cap == 0x0BE2) g_blendEnabled = false;
}

void lime_gl_blend_func(int sfactor, int dfactor) {
    g_blendSrc = sfactor;
    g_blendDst = dfactor;
    glstate::BlendFunc(sfactor, dfactor);
}

// 诊断用：读当前 GL 混合状态（只给日志，不参与渲染）。
extern "C" void lime_vk_get_blend_state(int* en, int* src, int* dst) {
    if (en) *en = g_blendEnabled ? 1 : 0;
    if (src) *src = g_blendSrc;
    if (dst) *dst = g_blendDst;
}

void lime_gl_color_mask(bool red, bool green, bool blue, bool alpha) {
    g_colorMask[0] = red;
    g_colorMask[1] = green;
    g_colorMask[2] = blue;
    g_colorMask[3] = alpha;
    glstate::ColorMask(red, green, blue, alpha);
}

void lime_gl_scissor(int x, int y, int width, int height) {
    g_scissorX = x;
    g_scissorY = y;
    g_scissorW = width;
    g_scissorH = height;
    glstate::Scissor(x, y, width, height);
}

int lime_gl_create_texture() {
    int name = g_nextTexName++;
    EnsureSize(g_texVk, (size_t)name + 1, 0);
    EnsureSizeI(g_texW, (size_t)name + 1, 0);
    EnsureSizeI(g_texH, (size_t)name + 1, 0);
    if (g_texBound.size() < (size_t)name + 1) g_texBound.resize((size_t)name + 1, 0);
    g_texVk[name] = 0;
    return name;
}

void lime_gl_bind_texture(int target, int texture) {
    if (g_texBound.size() < (size_t)texture + 1) g_texBound.resize((size_t)texture + 1, 0);
    g_texBound[(size_t)g_activeUnit] = texture;
}

void lime_gl_delete_texture(int texture) {
    if (texture <= 0) return;
    EnsureSize(g_texVk, (size_t)texture + 1, 0);
    uint32_t vk = g_texVk[texture];
    if (vk && g_renderer) g_renderer->DestroyTexture(vk);
    ClearTexNameMappings(vk);
    g_texVk[texture] = 0;
}

void lime_gl_active_texture(int texture) {
    // GL_TEXTURE0 = 0x84C0
    if (texture >= 0x84C0) g_activeUnit = texture - 0x84C0;
}

void lime_gl_tex_image_2d(int target, int level, int internalformat, int width, int height,
                          int border, int format, int type, double data) {
    if (width <= 0 || height <= 0) return;
    if (g_activeUnit >= (int)g_texBound.size()) return;
    int texName = g_texBound[(size_t)g_activeUnit];
    if (texName <= 0) return;
    EnsureSize(g_texVk, (size_t)texName + 1, 0);

    // 尺寸必须在 data==nullptr 提前返回之前记录：FBO 挂附件时常传空数据占位，
    // 完整性判定要靠这两个尺寸
    EnsureSizeI(g_texW, (size_t)texName + 1, 0);
    EnsureSizeI(g_texH, (size_t)texName + 1, 0);
    g_texW[(size_t)texName] = width;
    g_texH[(size_t)texName] = height;

    // Only handle level 0 RGBA/RGB 8-bit for now
    if (level != 0) return;

    const unsigned char* src = (const unsigned char*)(uintptr_t)data;
    if (!src) return;

    // 诊断：确认上传的像素不是全 0（全 0 会让采样全透明，整条 filter 链输出空）
    {
        static int s_texLog = 0;
        if (s_texLog < 20 || s_texLog % 200 == 0) {
            int bpp = (format == 0x1908) ? 4 : 3;
            fprintf(stderr, "[gltex] name=%d %dx%d fmt=0x%x px=%02x%02x%02x%02x\n",
                    texName, width, height, format,
                    src[0], src[1], src[2], bpp == 3 ? 0xff : src[3]);
        }
        s_texLog++;
    }

    std::vector<unsigned char> rgba((size_t)width * height * 4);
    // GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_BGRA_EXT = 0x80E1（lime 在桌面上传贴图用的就是 BGRA）
    if (format == 0x1908 || format == 0x80E1) {
        if (format == 0x1908) {
            memcpy(rgba.data(), src, rgba.size());
        } else {
            // BGRA -> RGBA：之前这里直接 return，导致游戏所有贴图都没上传，采样全黑
            for (int i = 0; i < width * height; i++) {
                rgba[i * 4 + 0] = src[i * 4 + 2];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 0];
                rgba[i * 4 + 3] = src[i * 4 + 3];
            }
        }
    } else if (format == 0x1907) {
        for (int i = 0; i < width * height; i++) {
            rgba[i * 4 + 0] = src[i * 3 + 0];
            rgba[i * 4 + 1] = src[i * 3 + 1];
            rgba[i * 4 + 2] = src[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
    } else {
        return;
    }

    if (!g_renderer) vk_fatal ("renderer_null fn=tex_image_2d");
    if (g_texVk[texName]) {
        // 取证：同名纹理被再次 texImage2D（等于销毁重建）。若大纹理每帧走这条路，
        // 说明内容来自 CPU 上传，且可以据此判断它是不是黑帧的来源。
        static int s_n = 0;
        if (s_n < 8) {
            fprintf(stderr, "[gltex] re-create n=%d name=%d %dx%d px=%02x%02x%02x%02x\n",
                    s_n + 1, texName, width, height,
                    rgba.empty() ? 0 : rgba[0], rgba.empty() ? 0 : rgba[1],
                    rgba.empty() ? 0 : rgba[2], rgba.size() < 4 ? 0 : rgba[3]);
        }
        s_n++;
        ClearTexNameMappings(g_texVk[texName]);
        g_renderer->DestroyTexture(g_texVk[texName]);
    }
    g_texVk[texName] = g_renderer->CreateTexture(width, height, rgba.data());
}

// ---------- buffers / vertex arrays ----------
struct GLBufferData {
    std::vector<unsigned char> data;
};
static std::vector<GLBufferData> g_buffers;
static int g_nextBufferName = 1;
static int g_boundArrayBuffer = 0; // GL_ARRAY_BUFFER = 0x8892
static int g_boundElementBuffer = 0; // GL_ELEMENT_ARRAY_BUFFER = 0x8893

struct AttribState {
    int size = 0;
    int type = 0;
    int stride = 0;
    double offset = 0;
    int buffer = 0;   // glVertexAttribPointer 当时绑定的 GL_ARRAY_BUFFER
    bool client = false;        // buffer==0 时 offset 是 client 内存指针
    uintptr_t clientPtr = 0;
    bool enabled = false;
};
static AttribState g_attrs[16];

// 常量顶点属性：OpenFL/Flixel 把每个 sprite 的 alpha / colorMultiplier / colorOffset
// 通过 glVertexAttrib1f/4f 写进 loc5/6/7（顶点缓冲里只有 loc0/4 的数组数据）。
// 之前这里是空实现，per-draw alpha 全部丢失，导致本该 alpha=0.05 的黑幕被画成不透明全屏。
static float g_attrConst[16][4] = {{0}};
static int g_attrConstN[16] = {0};   // 已写入的分量数
static bool g_attrConstSet[16] = {false};

// [vkfix] 常量乘数 (0,0,0,0) 折叠成 identity 的计数（vk_renderer EndFrame 每帧打印并清零）
int g_vkfixDrawTotal = 0;
int g_vkfixDrawFolded = 0;

// 常量乘数是否等于 (0,0,0,0)：Flixel 没设 colorTransform 时 loc6 保持默认零，
// 真 shader 的 hasColorTransform 分支会把它乘成黑，所以这一批必须当 identity。
static bool ConstMultAllZero() {
    if (!g_attrConstSet[6]) return true;
    for (int k = 0; k < 4; k++) {
        if (g_attrConst[6][k] != 0.0f) return false;
    }
    return true;
}

static bool ResolveAttrSource(const AttribState& a, const unsigned char** p,
                              size_t* base, size_t* bytes, int* stride);

// 关键层逐笔取证：tex 5/6/7 是 B 链（HUD/标题）三条，黑帧时它们是否被画、
// 画的时候 alpha 是多少、alpha 数组来自哪条 buffer —— 用日志区分
// “alpha=0 变透明”与“根本没发绘制”。
static void LogAlpha567(int texName, int count, const std::vector<float>& alphas, bool fromArray,
                        int baseVertex, const unsigned int* indices) {
    if (texName != 5 && texName != 6 && texName != 7) return;
    static int s_log = 0;
    if (s_log >= 2400) return;
    s_log++;
    float aMin = alphas[0], aMax = alphas[0];
    for (int i = 1; i < count; i++) {
        if (alphas[(size_t)i] < aMin) aMin = alphas[(size_t)i];
        if (alphas[(size_t)i] > aMax) aMax = alphas[(size_t)i];
    }
    const AttribState& a5 = g_attrs[5];
    int a5buf = a5.buffer > 0 ? a5.buffer : g_boundArrayBuffer;
    const unsigned char* ab = nullptr;
    size_t abytes = 0;
    if (a5.enabled && a5.size >= 1 && a5buf > 0 && a5buf < (int)g_buffers.size()
        && !g_buffers[(size_t)a5buf].data.empty()) {
        ab = g_buffers[(size_t)a5buf].data.data();
        abytes = g_buffers[(size_t)a5buf].data.size();
    }
    int astr = a5.stride > 0 ? a5.stride : 4;
    float raw[6] = {0, 0, 0, 0, 0, 0};
    int rn = count < 6 ? count : 6;
    for (int k = 0; k < rn; k++) {
        size_t o = (size_t)a5.offset + (size_t)k * (size_t)astr;
        if (ab && (abytes == 0 || o + 4 <= abytes)) raw[k] = *(const float*)(ab + o);
    }
    // 按“本次绘制的真实顶点索引”重读 alpha：raw[] 是按 0,1,2... 读的，
    // 若本笔 first/baseVertex != 0，raw[4]/raw[5]=0 就是读错了位置，不是真 alpha。
    float rawIx[6] = {0, 0, 0, 0, 0, 0};
    unsigned ix[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        unsigned vi = indices ? indices[k] : (unsigned)(baseVertex + k);
        ix[k] = vi;
        size_t o = (size_t)a5.offset + (size_t)vi * (size_t)astr;
        if (ab && (abytes == 0 || o + 4 <= abytes)) rawIx[k] = *(const float*)(ab + o);
    }
    // 第一顶点的 colorMultiplier（loc6 数组），这是“灰带是否来自顶点色”的直接证据
    float col[4] = {1, 1, 1, 1};
    {
        const unsigned char* cb = nullptr; size_t cbytes = 0, cbase = 0; int cstr = 0;
        if (ResolveAttrSource(g_attrs[6], &cb, &cbase, &cbytes, &cstr)) {
            unsigned vi = indices ? indices[0] : (unsigned)baseVertex;
            size_t o = cbase + (size_t)vi * (size_t)cstr;
            if (cbytes == 0 || o + 16 <= cbytes) memcpy(col, cb + o, 16);
        } else if (g_attrConstSet[6]) {
            memcpy(col, g_attrConst[6], 16);
        }
    }
    fprintf(stderr, "[alpha567] #%d tex=%d n=%d arr=%d en5=%d buf5=%d off5=%.0f str5=%d amin=%.3f amax=%.3f prog=%d blend=%d,%d,%d bv=%d ix=[%u %u %u] raw=[%.3f %.3f %.3f %.3f %.3f %.3f] rawIx=[%.3f %.3f %.3f] col=[%.3f %.3f %.3f %.3f]\n",
            s_log, texName, count, fromArray ? 1 : 0, a5.enabled ? 1 : 0, a5buf, a5.offset, astr,
            aMin, aMax, gl_shader_current_program (), g_blendEnabled ? 1 : 0, g_blendSrc, g_blendDst,
            baseVertex, ix[0], ix[1], ix[2],
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
            rawIx[0], rawIx[1], rawIx[2],
            col[0], col[1], col[2], col[3]);
}


// per-vertex alpha：lime 为 openfl 的 alpha 单独开了 loc5 顶点数组
// （实测 idx=5 size=1 stride=4 off=8，独立 VBO），黑幕的 0.05 就在这条数组里。
// 数组未启用/越界时退回常量属性（glVertexAttrib1f 的值），再退回 1。
static std::vector<float> BuildVertexAlpha(int count, int baseVertex, const unsigned int* indices, bool* usedArray) {
    std::vector<float> alphas((size_t)count, 1.0f);
    *usedArray = false;
    float a1 = g_attrConstSet[1] ? g_attrConst[1][0] : 1.0f;
    float a5c = g_attrConstSet[5] ? g_attrConst[5][0] : 1.0f;
    const AttribState& a = g_attrs[5];
    const unsigned char* p = nullptr;
    size_t bytes = 0, base = 0;
    int stride = 0;
    if (a.enabled && a.size >= 1) {
        if (a.client && a.clientPtr) {
            p = (const unsigned char*)a.clientPtr;
            stride = a.stride > 0 ? a.stride : 4;
        } else {
            int buf = a.buffer > 0 ? a.buffer : g_boundArrayBuffer;
            if (buf > 0 && buf < (int)g_buffers.size() && !g_buffers[(size_t)buf].data.empty()) {
                p = g_buffers[(size_t)buf].data.data();
                bytes = g_buffers[(size_t)buf].data.size();
                base = (size_t)a.offset;
                stride = a.stride > 0 ? a.stride : 4;
            }
        }
    }
    for (int i = 0; i < count; i++) {
        unsigned vi = indices ? indices[i] : (unsigned)(baseVertex + i);
        float v = a5c;
        if (p) {
            size_t o = base + (size_t)vi * (size_t)stride;
            if (bytes == 0 || o + 4 <= bytes) v = *(const float*)(p + o);
        }
        alphas[(size_t)i] = v * a1;
    }
    *usedArray = (p != nullptr);
    return alphas;
}

// 解析一个顶点属性的数据源（与 BuildVertexAlpha 同一套 GL 语义）：
// 设置指针时的 VBO（不是绘制时当前绑定的）、client 内存、stride 默认值。
static bool ResolveAttrSource(const AttribState& a, const unsigned char** p,
                              size_t* base, size_t* bytes, int* stride) {
    *p = nullptr; *base = 0; *bytes = 0; *stride = 0;
    if (!a.enabled || a.size < 1) return false;
    int st = a.stride > 0 ? a.stride : 4;
    if (a.client && a.clientPtr) {
        *p = (const unsigned char*)a.clientPtr;
        *stride = st;
        return true;
    }
    int buf = a.buffer > 0 ? a.buffer : g_boundArrayBuffer;
    if (buf > 0 && buf < (int)g_buffers.size() && !g_buffers[(size_t)buf].data.empty()) {
        *p = g_buffers[(size_t)buf].data.data();
        *bytes = g_buffers[(size_t)buf].data.size();
        *base = (size_t)a.offset;
        *stride = st;
        return true;
    }
    return false;
}

// 逐顶点颜色调制：lime 把 colorMultiplier/colorOffset 放在 loc6/loc7 的顶点数组
// （实测 idx=6 stride=16 off=24 buf=6，idx=7 stride=16 off=88 buf=6）。批处理路径
// 之前完全不读这两条数组，VK 顶点色恒为 (1,1,1,1)/(0,0,0,0)，红色调制因此丢失。
static void BuildVertexColor(int count, int baseVertex, const unsigned int* indices,
                             std::vector<float>& mult, std::vector<float>& off,
                             bool* usedArray) {
    mult.assign((size_t)count * 4, 1.0f);
    off.assign((size_t)count * 4, 0.0f);
    // 常量属性回退值（glVertexAttrib4f 写入）
    for (int k = 0; k < 4; k++) {
        float m = g_attrConstSet[6] ? g_attrConst[6][k] : 1.0f;
        float f = g_attrConstSet[7] ? g_attrConst[7][k] : 0.0f;
        for (int i = 0; i < count; i++) {
            mult[(size_t)i * 4 + k] = m;
            off[(size_t)i * 4 + k] = f;
        }
    }
    const unsigned char *p6, *p7;
    size_t b6, b7, base6, base7;
    int s6, s7;
    bool ok6 = ResolveAttrSource(g_attrs[6], &p6, &base6, &b6, &s6);
    bool ok7 = ResolveAttrSource(g_attrs[7], &p7, &base7, &b7, &s7);
    *usedArray = (ok6 || ok7);
    // 没走数组时，GL 的常量属性默认态是 (0,0,0,1)/(0,0,0,0)——它表示"这个 program 不读这条属性"。
    // VK 侧现在统一走 colorTransform 分支，必须把这种默认态当 identity，否则那批 const 笔会被乘成全黑。
    if (!ok6) {
        bool zero = (!g_attrConstSet[6]) ||
                    (g_attrConst[6][0] == 0.0f && g_attrConst[6][1] == 0.0f && g_attrConst[6][2] == 0.0f);
        if (zero) for (size_t i = 0; i < mult.size(); i++) mult[i] = 1.0f;
    }
    if (!ok7) {
        bool zero = (!g_attrConstSet[7]) ||
                    (g_attrConst[7][0] == 0.0f && g_attrConst[7][1] == 0.0f && g_attrConst[7][2] == 0.0f);
        if (zero) for (size_t i = 0; i < off.size(); i++) off[i] = 0.0f;
    }
    for (int i = 0; i < count; i++) {
        unsigned vi = indices ? indices[i] : (unsigned)(baseVertex + i);
        if (ok6) {
            size_t o = base6 + (size_t)vi * (size_t)s6;
            if (b6 == 0 || o + 16 <= b6) memcpy(&mult[(size_t)i * 4], p6 + o, 16);
        }
        if (ok7) {
            size_t o = base7 + (size_t)vi * (size_t)s7;
            if (b7 == 0 || o + 16 <= b7) memcpy(&off[(size_t)i * 4], p7 + o, 16);
        }
    }
}

int lime_gl_create_buffer() {
    int name = g_nextBufferName++;
    if (g_buffers.size() < (size_t)name + 1) g_buffers.resize((size_t)name + 1);
    return name;
}

void lime_gl_bind_buffer(int target, int buffer) {
    if (target == 0x8892) g_boundArrayBuffer = buffer;
    else if (target == 0x8893) g_boundElementBuffer = buffer;
    static int s_bindLog = 0;
    if (s_bindLog < 60) {
        fprintf(stderr, "[bindbuf] target=0x%x buf=%d\n", target, buffer);
        s_bindLog++;
    }
}

void lime_gl_buffer_data(int target, int size, double data, int usage) {
    int name = 0;
    if (target == 0x8892) name = g_boundArrayBuffer;
    else if (target == 0x8893) name = g_boundElementBuffer;
    if (name <= 0 || size < 0) return;
    if (g_buffers.size() < (size_t)name + 1) g_buffers.resize((size_t)name + 1);
    GLBufferData& b = g_buffers[(size_t)name];
    b.data.assign((size_t)size, 0);
    const unsigned char* src = (const unsigned char*)(uintptr_t)data;
    if (src && size > 0) memcpy(b.data.data(), src, (size_t)size);
    static int s_bdLog = 0;
    if (s_bdLog < 40) {
        const float* fp = (const float*)(uintptr_t)data;
        bool canRead = src && size >= 16;
        fprintf(stderr, "[bufdata] name=%d size=%d src=%p f0=%.2f f1=%.2f f2=%.2f f3=%.2f\n",
                name, size, (void*)src, canRead ? fp[0] : 0.0f, canRead ? fp[1] : 0.0f,
                canRead ? fp[2] : 0.0f, canRead ? fp[3] : 0.0f);
        s_bdLog++;
    }
}

void lime_gl_buffer_sub_data(int target, int offset, int size, double data) {
    int name = 0;
    if (target == 0x8892) name = g_boundArrayBuffer;
    else if (target == 0x8893) name = g_boundElementBuffer;
    if (name <= 0 || name >= (int)g_buffers.size()) return;
    GLBufferData& b = g_buffers[(size_t)name];
    const unsigned char* src = (const unsigned char*)(uintptr_t)data;
    if (!src || size <= 0) return;
    if ((size_t)(offset + size) > b.data.size()) return;
    memcpy(b.data.data() + offset, src, (size_t)size);
    static int s_bsLog = 0;
    if (s_bsLog < 40) {
        const float* fp = (const float*)src;
        bool canRead = size >= 16;
        fprintf(stderr, "[bufsub] name=%d off=%d size=%d f0=%.2f f1=%.2f f2=%.2f f3=%.2f\n",
                name, offset, size, canRead ? fp[0] : 0.0f, canRead ? fp[1] : 0.0f,
                canRead ? fp[2] : 0.0f, canRead ? fp[3] : 0.0f);
        s_bsLog++;
    }
}

void lime_gl_delete_buffer(int buffer) {
    if (buffer <= 0 || buffer >= (int)g_buffers.size()) return;
    g_buffers[(size_t)buffer].data.clear();
}

void lime_gl_vertex_attrib_pointer(int index, int size, int type, bool normalized, int stride, double offset) {
    if (index < 0 || index >= 16) return;
    g_attrs[index].size = size;
    g_attrs[index].type = type;
    g_attrs[index].stride = stride;
    g_attrs[index].offset = offset;
    // GL 语义：属性数据源是调用 glVertexAttribPointer 时绑定的 VBO，
    // 不是绘制时当前绑定的那个；uv 与 position 分属不同 VBO 时必须分开读
    g_attrs[index].buffer = g_boundArrayBuffer;
    // 没绑 VBO 时，offset 本身就是 client-side 数组的内存地址；不能拿它当字节偏移
    g_attrs[index].client = (g_boundArrayBuffer == 0);
    g_attrs[index].clientPtr = (uintptr_t)offset;
    static int s_apLog = 0;
    if (s_apLog < 60) {
        fprintf(stderr, "[attrib] idx=%d size=%d type=0x%x stride=%d off=%.0f buf=%d en=%d\n",
                index, size, type, stride, offset, g_boundArrayBuffer, g_attrs[index].enabled ? 1 : 0);
        s_apLog++;
    }
}

void lime_gl_enable_vertex_attrib_array(int index) {
    if (index >= 0 && index < 16) g_attrs[index].enabled = true;
}

void lime_gl_disable_vertex_attrib_array(int index) {
    if (index >= 0 && index < 16) g_attrs[index].enabled = false;
}

// ---------- 常量顶点属性（GL glVertexAttrib*f 的语义） ----------
static void SetConstAttr(int index, int n, float v0, float v1, float v2, float v3) {
    if (index < 0 || index >= 16) return;
    g_attrConst[index][0] = v0;
    g_attrConst[index][1] = v1;
    g_attrConst[index][2] = v2;
    g_attrConst[index][3] = v3;
    g_attrConstN[index] = n;
    g_attrConstSet[index] = true;
}

void lime_gl_vertex_attrib1f(int index, float v0) {
    SetConstAttr(index, 1, v0, 0.0f, 0.0f, 1.0f);
    if (index == 5 || index == 1) {
        static int s_log = 0;
        if (s_log < 40 || s_log % 1000 == 0) {
            fprintf(stderr, "[attrc] idx=%d v=%.3f\n", index, v0);
        }
        s_log++;
    }
}

void lime_gl_vertex_attrib1fv(int index, double v) {
    const float* p = (const float*)(uintptr_t)v;
    SetConstAttr(index, 1, p ? p[0] : 0.0f, 0.0f, 0.0f, 1.0f);
}

void lime_gl_vertex_attrib2f(int index, float v0, float v1) {
    SetConstAttr(index, 2, v0, v1, 0.0f, 1.0f);
}

void lime_gl_vertex_attrib2fv(int index, double v) {
    const float* p = (const float*)(uintptr_t)v;
    SetConstAttr(index, 2, p ? p[0] : 0.0f, p ? p[1] : 0.0f, 0.0f, 1.0f);
}

void lime_gl_vertex_attrib3f(int index, float v0, float v1, float v2) {
    SetConstAttr(index, 3, v0, v1, v2, 1.0f);
}

void lime_gl_vertex_attrib3fv(int index, double v) {
    const float* p = (const float*)(uintptr_t)v;
    SetConstAttr(index, 3, p ? p[0] : 0.0f, p ? p[1] : 0.0f, p ? p[2] : 0.0f, 1.0f);
}

void lime_gl_vertex_attrib4f(int index, float v0, float v1, float v2, float v3) {
    SetConstAttr(index, 4, v0, v1, v2, v3);
    if (index == 6 || index == 7) {
        static int s_log = 0;
        if (s_log < 30 || s_log % 1000 == 0) {
            fprintf(stderr, "[attrc] idx=%d v=%.3f,%.3f,%.3f,%.3f\n", index, v0, v1, v2, v3);
        }
        s_log++;
    }
}

void lime_gl_vertex_attrib4fv(int index, double v) {
    const float* p = (const float*)(uintptr_t)v;
    SetConstAttr(index, 4, p ? p[0] : 0.0f, p ? p[1] : 0.0f, p ? p[2] : 0.0f, p ? p[3] : 0.0f);
}

void lime_gl_draw_arrays(int mode, int first, int count) {
    // VK_PKT_EXEC=1：packet 路径已经在 emit 时立即发出 vkCmd，这里跳过，避免画两遍
    if (pkt::ExecActive()) return;
    if (count <= 0) return;
    if (!g_renderer) vk_fatal ("renderer_null fn=draw_arrays");
    // lime/OpenFL 的属性槽位：loc0=Position，loc4=TexCoord（不是 loc1）。
    // 旧实现按 loc1 读 uv，但游戏从不设置 loc1，于是回退读位置数据，
    // 表现为整张图集被铺进单个精灵的四边形。uv 优先取 loc4。
    int uvIdx = (g_attrs[4].size >= 2) ? 4 : 1;
    // 每个属性的数据源是设置指针时的 VBO（GL 语义），不一定是绘制时当前绑定的 VBO。
    // buffer==0 表示 client-side 数组：此时 offset 是内存地址，必须直接读；
    // 旧实现回退到位置 VBO，会把位置数据当 uv 读，表现为整张图集铺进精灵四边形
    bool posClient = g_attrs[0].buffer == 0 && g_attrs[0].clientPtr > 0x10000;
    bool uvClient = g_attrs[uvIdx].buffer == 0 && g_attrs[uvIdx].clientPtr > 0x10000;
    int posBuf = posClient ? 0 : (g_attrs[0].buffer > 0 ? g_attrs[0].buffer : g_boundArrayBuffer);
    int uvBuf = uvClient ? 0 : (g_attrs[uvIdx].buffer > 0 ? g_attrs[uvIdx].buffer : posBuf);
    if (!posClient) {
        if (posBuf <= 0 || posBuf >= (int)g_buffers.size()) return;
        if (g_buffers[(size_t)posBuf].data.empty()) return;
    }
    if (!uvClient) {
        if (uvBuf <= 0 || uvBuf >= (int)g_buffers.size()) uvBuf = posBuf;
        if (uvBuf <= 0 || uvBuf >= (int)g_buffers.size()) return;
        if (g_buffers[(size_t)uvBuf].data.empty()) return;
    }

    int strideP = g_attrs[0].stride;
    if (strideP <= 0) strideP = 16;
    int strideU = g_attrs[uvIdx].stride;
    if (strideU <= 0) strideU = strideP;

    std::vector<float> out((size_t)count * 4);
    const unsigned char* pp = posClient
        ? (const unsigned char*)g_attrs[0].clientPtr : g_buffers[(size_t)posBuf].data.data();
    const unsigned char* pu = uvClient
        ? (const unsigned char*)g_attrs[uvIdx].clientPtr : g_buffers[(size_t)uvBuf].data.data();
    size_t pBytes = posClient ? 0 : g_buffers[(size_t)posBuf].data.size();
    size_t uBytes = uvClient ? 0 : g_buffers[(size_t)uvBuf].data.size();
    size_t pBase0 = posClient ? 0 : (size_t)g_attrs[0].offset;
    size_t uBase0 = uvClient ? 0 : (size_t)g_attrs[uvIdx].offset;
    for (int i = 0; i < count; i++) {
        size_t base = (size_t)first + (size_t)i;
        size_t posOff = pBase0 + base * (size_t)strideP;
        size_t uvOff = uBase0 + base * (size_t)strideU;
        if ((!posClient && posOff + 8 > pBytes) || (!uvClient && uvOff + 8 > uBytes)) return;
        out[(size_t)i * 4 + 0] = *(const float*)(pp + posOff);
        out[(size_t)i * 4 + 1] = *(const float*)(pp + posOff + 4);
        out[(size_t)i * 4 + 2] = *(const float*)(pu + uvOff);
        out[(size_t)i * 4 + 3] = *(const float*)(pu + uvOff + 4);
    }

    int texName = 0;
    if (g_activeUnit < (int)g_texBound.size()) texName = g_texBound[(size_t)g_activeUnit];
    if (texName <= 0 || texName >= (int)g_texVk.size()) return;
    uint32_t vkTex = g_texVk[(size_t)texName];
    // FBO 颜色附件的纹理名会被别名到离屏 RenderTarget（>=1000），g_texVk 里没有，
    // 必须回退查询；不回退就会把整条 filter 链的场景绘制全部丢掉
    if (!vkTex) vkTex = gl_vk_texture_id(texName);
    if (!vkTex) {
        static int s_skip = 0;
        if (s_skip < 8 || s_skip % 200 == 0) {
            fprintf(stderr, "[gldraw] skip tex=%d count=%d\n", texName, count);
        }
        s_skip++;
        return;
    }
    // 诊断（默认常开、限 60 条）：每个 (GL 纹理名 -> Vk 纹理 id/尺寸) 组合只打一次。
    // 用于确认某一笔是不是采样到了错误的纹理/离屏画布别名（菜单偏亮问题的定位线）。
    {
        static int s_n = 0;
        static std::map<long long, int> s_seen;
        long long key = ((long long)texName << 32) | (long long)vkTex;
        if (s_n < 60 && s_seen.find(key) == s_seen.end()) {
            s_seen[key] = 1;
            s_n++;
            int tw = 0, th = 0;
            S_STATE_getTextureSize(texName, &tw, &th);
            fprintf(stderr, "[texmap] glName=%d vk=%u size=%dx%d alias=%d\n",
                    texName, vkTex, tw, th, vkTex >= 1000 ? 1 : 0);
        }
    }
    g_draws++;
    // 逐顶点 alpha（loc5 数组）；这是黑幕"几乎不可见"的唯一通道
    bool alphaFromArray = false;
    std::vector<float> alphas = BuildVertexAlpha(count, first, nullptr, &alphaFromArray);
    // 逐顶点颜色调制（loc6/loc7 数组）；这是顶部"红调制变灰"的唯一通道
    std::vector<float> mults, offs;
    bool colorFromArray = false;
    BuildVertexColor(count, first, nullptr, mults, offs, &colorFromArray);
    LogAlpha567(texName, count, alphas, alphaFromArray, first, nullptr);
    // [vkfix] 常量乘数全零 ⇒ 交给 SetDrawColor 的 nullptr（identity），否则真 frag 会乘成黑
    const bool foldZeroMult = !colorFromArray && ConstMultAllZero();
    g_vkfixDrawTotal++;
    if (foldZeroMult) g_vkfixDrawFolded++;
    g_renderer->SetDrawColor ((g_attrConstSet[1] ? g_attrConst[1][0] : 1.0f) *
                              ((g_attrConstSet[5] && g_attrConst[5][0] != 0.0f) ? g_attrConst[5][0] : 1.0f),
                              foldZeroMult ? nullptr : (g_attrConstSet[6] ? g_attrConst[6] : nullptr),
                              g_attrConstSet[7] ? g_attrConst[7] : nullptr);
    // 诊断：确认 lime 传进来的 uv 是“帧子矩形归一化”还是“整图 0..1”/像素值
    {
        static int s_uvdbg = 0;
        if (s_uvdbg < 25 || s_uvdbg % 300 == 0) {
            float minU = out[2], maxU = out[2], minV = out[3], maxV = out[3];
            float minX = out[0], maxX = out[0], minY = out[1], maxY = out[1];
            for (int i = 1; i < count; i++) {
                float u = out[(size_t)i * 4 + 2], v = out[(size_t)i * 4 + 3];
                if (u < minU) minU = u; if (u > maxU) maxU = u;
                if (v < minV) minV = v; if (v > maxV) maxV = v;
                float x = out[(size_t)i * 4 + 0], y = out[(size_t)i * 4 + 1];
                if (x < minX) minX = x; if (x > maxX) maxX = x;
                if (y < minY) minY = y; if (y > maxY) maxY = y;
            }
            float aMin = 1e9f, aMax = -1e9f;
            for (int i = 0; i < count; i++) {
                if (alphas[(size_t)i] < aMin) aMin = alphas[(size_t)i];
                if (alphas[(size_t)i] > aMax) aMax = alphas[(size_t)i];
            }
            int tw = 0, th = 0; gl_vk_texture_size (texName, &tw, &th);
            fprintf (stderr, "[gldraw] cur=%d tex=%d size=%dx%d n=%d uv=[%.3f..%.3f, %.3f..%.3f] pos=[%.1f..%.1f, %.1f..%.1f] pStride=%d pOff=%.0f uvBuf=%d uvStride=%d uvOff=%.0f pBufRaw=%d uBufRaw=%d posCli=%d uvCli=%d amin=%.3f amax=%.3f col=[%.3f %.3f %.3f %.3f] coff=[%.3f %.3f %.3f %.3f]\n",
                     gl_vk_current_target(), texName, tw, th, count, minU, maxU, minV, maxV,
                     minX, maxX, minY, maxY, strideP, g_attrs[0].offset,
                     uvBuf, strideU, g_attrs[uvIdx].offset,
                     g_attrs[0].buffer, g_attrs[uvIdx].buffer, posClient ? 1 : 0, uvClient ? 1 : 0,
                     aMin, aMax,
                     mults[0], mults[1], mults[2], mults[3],
                     offs[0], offs[1], offs[2], offs[3]);
        }
        s_uvdbg++;
    }
    // uniform 快照：批处理把提交推迟到帧尾，同一个 program 一帧内可能有多笔绘制
    // 且 uniform 不同（典型：hasColorTransform 0/1 交替）。这里在 draw 时刻拷贝一份。
    const unsigned char* uniData = nullptr;
    uint32_t uniSize = 0;
    lime::gl_shader_uniform_block (lime::gl_shader_current_program (), &uniData, &uniSize);
    g_renderer->DrawRaw(vkTex, out.data(), count, alphaFromArray ? alphas.data() : nullptr,
                        colorFromArray ? mults.data() : nullptr,
                        colorFromArray ? offs.data() : nullptr,
                        uniData, uniSize);
}

void lime_gl_draw_elements(int mode, int count, int type, double offset) {
    // VK_PKT_EXEC=1：packet 路径已经在 emit 时立即发出 vkCmd，这里跳过，避免画两遍
    if (pkt::ExecActive()) return;
    if (count <= 0) return;
    if (!g_renderer) vk_fatal ("renderer_null fn=draw_elements");
    // lime/OpenFL 的 texcoord 在 loc4（loc1 从未被设置）：读 loc1 会拿到位置数据
    int uvIdx = (g_attrs[4].size >= 2) ? 4 : 1;
    // 同上：pos / uv 各自读设置指针时的数据源；client-side 数组（buffer==0）
    // 的 offset 是内存地址，必须直接读，不能回退到位置 VBO
    bool posClient = g_attrs[0].buffer == 0 && g_attrs[0].clientPtr > 0x10000;
    bool uvClient = g_attrs[uvIdx].buffer == 0 && g_attrs[uvIdx].clientPtr > 0x10000;
    int posBuf = posClient ? 0 : (g_attrs[0].buffer > 0 ? g_attrs[0].buffer : g_boundArrayBuffer);
    int uvBuf = uvClient ? 0 : (g_attrs[uvIdx].buffer > 0 ? g_attrs[uvIdx].buffer : posBuf);
    if (!posClient) {
        if (posBuf <= 0 || posBuf >= (int)g_buffers.size()) return;
        if (g_buffers[(size_t)posBuf].data.empty()) return;
    }
    if (!uvClient) {
        if (uvBuf <= 0 || uvBuf >= (int)g_buffers.size()) uvBuf = posBuf;
        if (uvBuf <= 0 || uvBuf >= (int)g_buffers.size()) return;
        if (g_buffers[(size_t)uvBuf].data.empty()) return;
    }
    if (g_boundElementBuffer <= 0 || g_boundElementBuffer >= (int)g_buffers.size()) return;
    const GLBufferData& ib = g_buffers[(size_t)g_boundElementBuffer];
    if (ib.data.empty()) return;

    int strideP = g_attrs[0].stride;
    if (strideP <= 0) strideP = 16;
    int strideU = g_attrs[uvIdx].stride;
    if (strideU <= 0) strideU = strideP;
    int idxSize = (type == 0x1403) ? 2 : 4; // GL_UNSIGNED_SHORT = 0x1403

    std::vector<float> out((size_t)count * 4);
    std::vector<unsigned int> idxs((size_t)count);
    const unsigned char* ip = ib.data.data();
    const unsigned char* pp = posClient
        ? (const unsigned char*)g_attrs[0].clientPtr : g_buffers[(size_t)posBuf].data.data();
    const unsigned char* pu = uvClient
        ? (const unsigned char*)g_attrs[uvIdx].clientPtr : g_buffers[(size_t)uvBuf].data.data();
    size_t pBytes = posClient ? 0 : g_buffers[(size_t)posBuf].data.size();
    size_t uBytes = uvClient ? 0 : g_buffers[(size_t)uvBuf].data.size();
    size_t pBase0 = posClient ? 0 : (size_t)g_attrs[0].offset;
    size_t uBase0 = uvClient ? 0 : (size_t)g_attrs[uvIdx].offset;
    size_t byteOff = (size_t)offset;
    for (int i = 0; i < count; i++) {
        size_t idxOff = byteOff + (size_t)i * (size_t)idxSize;
        if (idxOff + (size_t)idxSize > ib.data.size()) return;
        unsigned int idx = (idxSize == 2)
            ? *(const unsigned short*)(ip + idxOff)
            : *(const unsigned int*)(ip + idxOff);
        idxs[(size_t)i] = idx;
        size_t posOff = pBase0 + (size_t)idx * (size_t)strideP;
        size_t uvOff = uBase0 + (size_t)idx * (size_t)strideU;
        if ((!posClient && posOff + 8 > pBytes) || (!uvClient && uvOff + 8 > uBytes)) return;
        out[(size_t)i * 4 + 0] = *(const float*)(pp + posOff);
        out[(size_t)i * 4 + 1] = *(const float*)(pp + posOff + 4);
        out[(size_t)i * 4 + 2] = *(const float*)(pu + uvOff);
        out[(size_t)i * 4 + 3] = *(const float*)(pu + uvOff + 4);
    }

    int texName = 0;
    if (g_activeUnit < (int)g_texBound.size()) texName = g_texBound[(size_t)g_activeUnit];
    if (texName <= 0 || texName >= (int)g_texVk.size()) return;
    uint32_t vkTex = g_texVk[(size_t)texName];
    if (!vkTex) vkTex = gl_vk_texture_id(texName);
    if (!vkTex) {
        static int s_skip2 = 0;
        if (s_skip2 < 8 || s_skip2 % 200 == 0) {
            fprintf(stderr, "[gldraw] skip(ie) tex=%d count=%d\n", texName, count);
        }
        s_skip2++;
        return;
    }
    // 诊断（indexed 路径，限 120 条）：每个 (GL 纹理名 -> Vk id/尺寸/是否别名) 只打一次。
    {
        static int s_n2 = 0;
        static std::map<long long, int> s_seen2;
        long long key = ((long long)texName << 32) | (long long)vkTex;
        if (s_n2 < 120 && s_seen2.find(key) == s_seen2.end()) {
            s_seen2[key] = 1;
            s_n2++;
            int tw = 0, th = 0;
            gl_vk_texture_size(texName, &tw, &th);
            fprintf(stderr, "[texmap] glName=%d vk=%u size=%dx%d alias=%d ie=1\n",
                    texName, vkTex, tw, th, vkTex >= 1000 ? 1 : 0);
        }
    }
    g_draws++;
    bool alphaFromArray = false;
    std::vector<float> alphas = BuildVertexAlpha(count, 0, idxs.data(), &alphaFromArray);
    std::vector<float> mults, offs;
    bool colorFromArray = false;
    BuildVertexColor(count, 0, idxs.data(), mults, offs, &colorFromArray);
    LogAlpha567(texName, count, alphas, alphaFromArray, 0, idxs.data());
    // [vkfix] indexed 路径同款折叠
    const bool foldZeroMult = !colorFromArray && ConstMultAllZero();
    g_vkfixDrawTotal++;
    if (foldZeroMult) g_vkfixDrawFolded++;
    g_renderer->SetDrawColor ((g_attrConstSet[1] ? g_attrConst[1][0] : 1.0f) *
                              ((g_attrConstSet[5] && g_attrConst[5][0] != 0.0f) ? g_attrConst[5][0] : 1.0f),
                              foldZeroMult ? nullptr : (g_attrConstSet[6] ? g_attrConst[6] : nullptr),
                              g_attrConstSet[7] ? g_attrConst[7] : nullptr);
    // 诊断：与 draw_arrays 同款，确认 indexed 路径的 uv 语义
    {
        static int s_uvdbg2 = 0;
        if (s_uvdbg2 < 25 || s_uvdbg2 % 300 == 0) {
            float minU = out[2], maxU = out[2], minV = out[3], maxV = out[3];
            float minX = out[0], maxX = out[0], minY = out[1], maxY = out[1];
            for (int i = 1; i < count; i++) {
                float u = out[(size_t)i * 4 + 2], v = out[(size_t)i * 4 + 3];
                if (u < minU) minU = u; if (u > maxU) maxU = u;
                if (v < minV) minV = v; if (v > maxV) maxV = v;
                float x = out[(size_t)i * 4 + 0], y = out[(size_t)i * 4 + 1];
                if (x < minX) minX = x; if (x > maxX) maxX = x;
                if (y < minY) minY = y; if (y > maxY) maxY = y;
            }
            float aMin = 1e9f, aMax = -1e9f;
            for (int i = 0; i < count; i++) {
                if (alphas[(size_t)i] < aMin) aMin = alphas[(size_t)i];
                if (alphas[(size_t)i] > aMax) aMax = alphas[(size_t)i];
            }
            int tw = 0, th = 0; gl_vk_texture_size (texName, &tw, &th);
            fprintf (stderr, "[gldraw/ie] cur=%d tex=%d size=%dx%d n=%d uv=[%.3f..%.3f, %.3f..%.3f] pos=[%.1f..%.1f, %.1f..%.1f] pStride=%d pOff=%.0f uvBuf=%d uvStride=%d uvOff=%.0f pBufRaw=%d uBufRaw=%d posCli=%d uvCli=%d prog=%d blend=%d,%d,%d amin=%.3f amax=%.3f col=[%.3f %.3f %.3f %.3f] coff=[%.3f %.3f %.3f %.3f]\n",
                     gl_vk_current_target(), texName, tw, th, count, minU, maxU, minV, maxV,
                     minX, maxX, minY, maxY, strideP, g_attrs[0].offset,
                     uvBuf, strideU, g_attrs[uvIdx].offset,
                     g_attrs[0].buffer, g_attrs[uvIdx].buffer, posClient ? 1 : 0, uvClient ? 1 : 0,
                     gl_shader_current_program (), g_blendEnabled ? 1 : 0, g_blendSrc, g_blendDst,
                     aMin, aMax,
                     mults[0], mults[1], mults[2], mults[3],
                     offs[0], offs[1], offs[2], offs[3]);
            static int s_raw2 = 0;
            if (s_raw2 < 6 && !posClient && pBytes >= pBase0 + 32) {
                const float* f = (const float*)(pp + pBase0);
                fprintf(stderr, "[rawpos] ie tex=%d off=%zu f=[%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f]\n",
                        texName, pBase0, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
                s_raw2++;
            }
            if (tw >= 2000 && th >= 2000) {
                static int s_big = 0;
                if (s_big < 60) {
                    const AttribState& a5 = g_attrs[5];
                    int a5buf = a5.buffer > 0 ? a5.buffer : g_boundArrayBuffer;
                    const unsigned char* ab = nullptr;
                    size_t abytes = 0;
                    if (a5.enabled && a5.size >= 1 && a5buf > 0 && a5buf < (int)g_buffers.size()
                        && !g_buffers[(size_t)a5buf].data.empty()) {
                        ab = g_buffers[(size_t)a5buf].data.data();
                        abytes = g_buffers[(size_t)a5buf].data.size();
                    }
                    size_t abase = (size_t)a5.offset;
                    int astr = a5.stride > 0 ? a5.stride : 4;
                    float raw[6] = {0, 0, 0, 0, 0, 0};
                    int rn = count < 6 ? count : 6;
                    for (int k = 0; k < rn; k++) {
                        unsigned vi = idxs[(size_t)k];
                        size_t o = abase + (size_t)vi * (size_t)astr;
                        if (ab && (abytes == 0 || o + 4 <= abytes)) raw[k] = *(const float*)(ab + o);
                    }
                    fprintf(stderr, "[bigalpha] tex=%d en5=%d en6=%d en7=%d buf5=%d off5=%.0f str5=%d a5c=%.3f raw=[%.3f %.3f %.3f %.3f %.3f %.3f]\n",
                            texName, a5.enabled ? 1 : 0, g_attrs[6].enabled ? 1 : 0, g_attrs[7].enabled ? 1 : 0,
                            a5buf, a5.offset, astr,
                            g_attrConstSet[5] ? g_attrConst[5][0] : 1.0f,
                            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
                    s_big++;
                }
            }
        }
        s_uvdbg2++;
    }
    const unsigned char* uniData = nullptr;
    uint32_t uniSize = 0;
    lime::gl_shader_uniform_block (lime::gl_shader_current_program (), &uniData, &uniSize);
    g_renderer->DrawRaw(vkTex, out.data(), count, alphaFromArray ? alphas.data() : nullptr,
                        colorFromArray ? mults.data() : nullptr,
                        colorFromArray ? offs.data() : nullptr,
                        uniData, uniSize);
}

int lime_gl_get_error() {
    return glstate::TakeError();
}

// ---------------------------------------------------------------- packet 解码桥
// 只读快照：emit 调用内抓取，随后解码器只看 DrawRecord。实现在本文件是为了
// 不把 g_attrs/g_buffers/g_tex* 暴露到新文件里（避免解码侧耦合渲染器内部）。

namespace pkt {

bool SnapshotAttr (int index, AttrSnap* out) {
    if (!out || index < 0 || index >= 16) return false;
    const AttribState& a = g_attrs[index];
    memset(out, 0, sizeof(*out));
    out->bufferName = (a.buffer > 0) ? (uint32_t)a.buffer : 0u;
    out->byteOffset = (a.client || a.offset < 0.0) ? 0u : (uint32_t)a.offset;
    out->stride = a.stride;
    out->size = a.size;
    out->glType = a.type;
    out->kind = 0;
    out->enabled = a.enabled ? 1 : 0;
    out->client = a.client ? 1 : 0;
    out->constSet = g_attrConstSet[index] ? 1 : 0;
    out->constN = (uint8_t)g_attrConstN[index];
    for (int k = 0; k < 4; k++) out->constValue[k] = g_attrConst[index][k];
    out->clientPtr = (a.client && a.clientPtr) ? (const void*)a.clientPtr : nullptr;
    return true;
}

int SnapshotTexBound (int unit) {
    if (unit < 0 || unit >= (int)g_texBound.size()) return 0;
    return g_texBound[(size_t)unit];
}

uint32_t SnapshotTexVk (int glName) {
    return S_STATE_getTextureId(glName);
}

bool SnapshotBuffer (int name, const uint8_t** data, uint32_t* bytes) {
    if (data) *data = nullptr;
    if (bytes) *bytes = 0;
    if (name <= 0 || name >= (int)g_buffers.size()) return false;
    const std::vector<unsigned char>& d = g_buffers[(size_t)name].data;
    if (d.empty()) return false;
    if (data) *data = d.data();
    if (bytes) *bytes = (uint32_t)d.size();
    return true;
}

} // namespace pkt

} // namespace lime
