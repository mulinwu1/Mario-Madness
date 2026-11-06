#include "gl_compat.h"
#include "vk_renderer.h"
#include "gl_vk_bridge.h"
#include "gl_vk_contract.h"
#include "gl_state.h"

#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdio>

namespace lime {

static VkRenderer* g_renderer = nullptr;
static uint64_t g_draws = 0;
static uint64_t g_presents = 0;

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
void gl_vk_init(void* hwnd, int width, int height) {
    if (g_renderer) return;
    if (!hwnd || width <= 0 || height <= 0) return;
    VkRenderer* r = new VkRenderer();
    if (r->Init((HWND)hwnd, width, height)) {
        g_renderer = r;
        fprintf(stderr, "[glvk] renderer ready %dx%d hwnd=%p\n", width, height, hwnd);
        // FBO / 纹理参数模块靠这两个契约拿到纹素与清屏色
        gl_vk_install_core_state(&S_STATE);
        gl_vk_install_renderer_ext(&EXT);
        // 状态机：视口/剪裁默认整窗；非法 blend 组合的警告走 stderr（构建日志里可见）
        glstate::ResetToDefaults(width, height);
        glstate::SetWarningCallback(nullptr);
    } else {
        fprintf(stderr, "[glvk] init failed: %s\n", r->LastError());
        delete r;
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
    if (!g_renderer) return;
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

    std::vector<unsigned char> rgba((size_t)width * height * 4);
    // GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_UNSIGNED_BYTE = 0x1401
    if (format == 0x1908) {
        memcpy(rgba.data(), src, rgba.size());
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

    if (!g_renderer) return;
    if (g_texVk[texName]) g_renderer->DestroyTexture(g_texVk[texName]);
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
    bool enabled = false;
};
static AttribState g_attrs[16];

int lime_gl_create_buffer() {
    int name = g_nextBufferName++;
    if (g_buffers.size() < (size_t)name + 1) g_buffers.resize((size_t)name + 1);
    return name;
}

void lime_gl_bind_buffer(int target, int buffer) {
    if (target == 0x8892) g_boundArrayBuffer = buffer;
    else if (target == 0x8893) g_boundElementBuffer = buffer;
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
}

void lime_gl_enable_vertex_attrib_array(int index) {
    if (index >= 0 && index < 16) g_attrs[index].enabled = true;
}

void lime_gl_disable_vertex_attrib_array(int index) {
    if (index >= 0 && index < 16) g_attrs[index].enabled = false;
}

void lime_gl_draw_arrays(int mode, int first, int count) {
    if (!g_renderer || count <= 0) return;
    if (g_boundArrayBuffer <= 0 || g_boundArrayBuffer >= (int)g_buffers.size()) return;
    const GLBufferData& b = g_buffers[(size_t)g_boundArrayBuffer];
    if (b.data.empty()) return;

    int stride = g_attrs[0].stride;
    if (stride <= 0) stride = 16;

    std::vector<float> out((size_t)count * 4);
    const unsigned char* p = b.data.data();
    for (int i = 0; i < count; i++) {
        size_t base = (size_t)first + (size_t)i;
        size_t posOff = (size_t)g_attrs[0].offset + base * (size_t)stride;
        size_t uvOff = (size_t)g_attrs[1].offset + base * (size_t)stride;
        if (posOff + 8 > b.data.size() || uvOff + 8 > b.data.size()) return;
        out[(size_t)i * 4 + 0] = *(const float*)(p + posOff);
        out[(size_t)i * 4 + 1] = *(const float*)(p + posOff + 4);
        out[(size_t)i * 4 + 2] = *(const float*)(p + uvOff);
        out[(size_t)i * 4 + 3] = *(const float*)(p + uvOff + 4);
    }

    int texName = 0;
    if (g_activeUnit < (int)g_texBound.size()) texName = g_texBound[(size_t)g_activeUnit];
    if (texName <= 0 || texName >= (int)g_texVk.size()) return;
    uint32_t vkTex = g_texVk[(size_t)texName];
    if (!vkTex) return;
    g_draws++;
    g_renderer->DrawRaw(vkTex, out.data(), count);
}

void lime_gl_draw_elements(int mode, int count, int type, double offset) {
    if (!g_renderer || count <= 0) return;
    if (g_boundArrayBuffer <= 0 || g_boundArrayBuffer >= (int)g_buffers.size()) return;
    if (g_boundElementBuffer <= 0 || g_boundElementBuffer >= (int)g_buffers.size()) return;
    const GLBufferData& vb = g_buffers[(size_t)g_boundArrayBuffer];
    const GLBufferData& ib = g_buffers[(size_t)g_boundElementBuffer];
    if (vb.data.empty() || ib.data.empty()) return;

    int stride = g_attrs[0].stride;
    if (stride <= 0) stride = 16;
    int idxSize = (type == 0x1403) ? 2 : 4; // GL_UNSIGNED_SHORT = 0x1403

    std::vector<float> out((size_t)count * 4);
    const unsigned char* ip = ib.data.data();
    const unsigned char* p = vb.data.data();
    size_t byteOff = (size_t)offset;
    for (int i = 0; i < count; i++) {
        size_t idxOff = byteOff + (size_t)i * (size_t)idxSize;
        if (idxOff + (size_t)idxSize > ib.data.size()) return;
        unsigned int idx = (idxSize == 2)
            ? *(const unsigned short*)(ip + idxOff)
            : *(const unsigned int*)(ip + idxOff);
        size_t posOff = (size_t)g_attrs[0].offset + (size_t)idx * (size_t)stride;
        size_t uvOff = (size_t)g_attrs[1].offset + (size_t)idx * (size_t)stride;
        if (posOff + 8 > vb.data.size() || uvOff + 8 > vb.data.size()) return;
        out[(size_t)i * 4 + 0] = *(const float*)(p + posOff);
        out[(size_t)i * 4 + 1] = *(const float*)(p + posOff + 4);
        out[(size_t)i * 4 + 2] = *(const float*)(p + uvOff);
        out[(size_t)i * 4 + 3] = *(const float*)(p + uvOff + 4);
    }

    int texName = 0;
    if (g_activeUnit < (int)g_texBound.size()) texName = g_texBound[(size_t)g_activeUnit];
    if (texName <= 0 || texName >= (int)g_texVk.size()) return;
    uint32_t vkTex = g_texVk[(size_t)texName];
    if (!vkTex) return;
    g_draws++;
    g_renderer->DrawRaw(vkTex, out.data(), count);
}

int lime_gl_get_error() {
    return glstate::TakeError();
}

} // namespace lime
