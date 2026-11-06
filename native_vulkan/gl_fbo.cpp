// =============================================================================
// Vulkan 翻译层：FBO / renderbuffer（GL 离屏渲染对象）实现
//
// 映射思路：
//   GL framebuffer object      -> VkRenderer::CreateRenderTarget / BindRenderTarget
//                                 / PresentTarget / DestroyRenderTarget
//   GL color attachment 纹理   -> 把该 GL 纹理名指向离屏 RenderTarget id（>=1000），
//                                 这样后续用这张纹理采样时命中的就是离屏内容
//                                 （VkRenderer::DrawRaw 已经支持 >=1000 的 id）
//   GL renderbuffer            -> 只记录尺寸/格式/采样数。VkRenderer 目前没有独立的
//                                 depth/stencil 通道，需要主线补接口（见下面 Ext）
//
// 依边界要求，本文件不修改 gl_stubs.cpp / gl_core.cpp / vk_renderer.*：
// 需要读 gl_core.cpp 内部状态的部分，通过 GlVkCoreState 函数指针向主线注册（下面的
// 契约块与 gl_texture_params.cpp 保持一致，内容必须完全相同）。
// =============================================================================

#include "gl_vk_bridge.h"   // 内含 gl_compat.h
#include "gl_vk_contract.h"
#include "vk_renderer.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace lime {

namespace {

// ---------------------------------------------------------------- GL 常量
enum {
    K_FRAMEBUFFER = 0x8D40,
    K_READ_FRAMEBUFFER = 0x8CA8,
    K_DRAW_FRAMEBUFFER = 0x8CA9,
    K_RENDERBUFFER = 0x8D41,

    K_COLOR_ATTACHMENT0 = 0x8CE0,
    K_COLOR_ATTACHMENT1 = 0x8CE1,
    K_COLOR_ATTACHMENT2 = 0x8CE2,
    K_COLOR_ATTACHMENT3 = 0x8CE3,
    K_DEPTH_ATTACHMENT = 0x8D00,
    K_STENCIL_ATTACHMENT = 0x8D20,
    K_DEPTH_STENCIL_ATTACHMENT = 0x821A,

    K_FB_COMPLETE = 0x8CD5,
    K_FB_INCOMPLETE_ATTACHMENT = 0x8CD6,
    K_FB_INCOMPLETE_MISSING_ATTACHMENT = 0x8CD7,
    K_FB_INCOMPLETE_DIMENSIONS = 0x8CD9,
    K_FB_UNSUPPORTED = 0x8CDD,
    K_FB_INCOMPLETE_MULTISAMPLE = 0x8D56,

    K_ATTACH_OBJECT_TYPE = 0x8CD0,
    K_ATTACH_OBJECT_NAME = 0x8CD1,
    K_ATTACH_TEXTURE_LEVEL = 0x8CD2,
    K_ATTACH_TEXTURE_CUBE_FACE = 0x8CD3,
    K_ATTACH_TEXTURE_LAYER = 0x8CD4,
    K_ATTACH_LAYERED = 0x8DA7,
    K_ATTACH_COLOR_ENCODING = 0x8210,
    K_ATTACH_COMPONENT_TYPE = 0x8211,
    K_LINEAR = 0x2601,
    K_UNSIGNED_NORMALIZED = 0x8C17,

    K_OBJECT_TEXTURE = 0x1702,
    K_OBJECT_RENDERBUFFER = 0x8D41,
    K_OBJECT_NONE = 0,

    K_RB_WIDTH = 0x8D42,
    K_RB_HEIGHT = 0x8D43,
    K_RB_INTERNAL_FORMAT = 0x8D44,
    K_RB_SAMPLES = 0x8CAB
};

struct Attachment {
    int objectType;   // K_OBJECT_NONE / K_OBJECT_TEXTURE / K_OBJECT_RENDERBUFFER
    int name;         // GL 对象名
    int level;        // 纹理 level
    int face;         // cube face（0 表示 TEXTURE_2D）
    Attachment () : objectType (K_OBJECT_NONE), name (0), level (0), face (0) {}
};

struct Fbo {
    bool used;
    Attachment color;
    Attachment depth;
    Attachment stencil;
    uint32_t rtId;    // VkRenderer RenderTarget id（0 = 尚未创建）
    int width;
    int height;
    bool dirty;       // 附件变化，需要重建 RT
    Fbo () : used (false), rtId (0), width (0), height (0), dirty (true) {}
};

struct Rbo {
    bool used;
    int width;
    int height;
    int internalFormat;
    int samples;
    Rbo () : used (false), width (0), height (0), internalFormat (0), samples (0) {}
};

// 索引 0 保留给默认对象（GL 里名字 0 不对应对象）
std::vector<Fbo> g_fbos (1);
std::vector<Rbo> g_rbos (1);
int g_nextFboName = 1;
int g_nextRboName = 1;

int g_boundDrawFbo = 0;
int g_boundReadFbo = 0;
int g_boundRbo = 0;

// 当前真正打开帧的目标：0 = 后缓冲，>=1000 = 离屏 RT
uint32_t g_currentTarget = 0;
bool g_backbufferOpen = false;

// ---------------------------------------------------------------- 小工具
void EnsureFboSlot (int name) {
    if (name > 0 && (size_t)name >= g_fbos.size ()) {
        g_fbos.resize ((size_t)name + 1);
    }
}

void EnsureRboSlot (int name) {
    if (name > 0 && (size_t)name >= g_rbos.size ()) {
        g_rbos.resize ((size_t)name + 1);
    }
}

bool IsFboName (int name) {
    return name > 0 && (size_t)name < g_fbos.size () && g_fbos[name].used;
}

bool IsRboName (int name) {
    return name > 0 && (size_t)name < g_rbos.size () && g_rbos[name].used;
}

int BoundFboName (int target) {
    return (target == K_READ_FRAMEBUFFER) ? g_boundReadFbo : g_boundDrawFbo;
}

bool AttachmentSize (const Attachment& a, int* w, int* h) {
    if (a.objectType == K_OBJECT_TEXTURE) {
        return gl_vk_texture_size (a.name, w, h);
    }
    if (a.objectType == K_OBJECT_RENDERBUFFER) {
        if (!IsRboName (a.name)) return false;
        Rbo& b = g_rbos[(size_t)a.name];
        *w = b.width;
        *h = b.height;
        return b.width > 0 && b.height > 0;
    }
    return false;
}

void GetClearColor (float* c) {
    c[0] = 0.0f; c[1] = 0.0f; c[2] = 0.0f; c[3] = 1.0f;
    const GlVkCoreState* cs = gl_vk_core_state ();
    if (cs && cs->getClearColor) cs->getClearColor (c);
}

// 结束已经打开的帧（后缓冲会 present，离屏目标只提交）
void CloseOpenFrame () {
    VkRenderer* r = gl_vk_get_renderer ();
    if (g_currentTarget != 0 || g_backbufferOpen) {
        if (r) r->PresentTarget ();
    }
    g_currentTarget = 0;
    g_backbufferOpen = false;
}

// 销毁 FBO 持有的 RT；如果它正被绘制，先收尾当前帧
void DestroyFboTarget (Fbo& f) {
    if (f.rtId == 0) return;
    if (g_currentTarget == f.rtId) CloseOpenFrame ();
    VkRenderer* r = gl_vk_get_renderer ();
    if (r) r->DestroyRenderTarget (f.rtId);
    f.rtId = 0;
}

// 按当前附件创建 / 重建离屏 RT，并把颜色附件的纹理名指向它
bool EnsureFboTarget (int fboName) {
    Fbo& f = g_fbos[(size_t)fboName];
    int w = 0, h = 0;
    if (!AttachmentSize (f.color, &w, &h) || w <= 0 || h <= 0) return false;

    VkRenderer* r = gl_vk_get_renderer ();
    if (!r) return false;

    bool needDepth = (f.depth.objectType != K_OBJECT_NONE ||
                      f.stencil.objectType != K_OBJECT_NONE);

    DestroyFboTarget (f);

    uint32_t id = 0;
    uint32_t wId = (uint32_t)w, hId = (uint32_t)h;
    if (needDepth && gl_vk_renderer_ext() && gl_vk_renderer_ext()->createRenderTargetEx) {
        id = gl_vk_renderer_ext()->createRenderTargetEx (wId, hId, true);
    }
    if (id == 0) {
        // 当前 VkRenderer 只有颜色附件；深度/模板的落地等主线补 createRenderTargetEx
        id = r->CreateRenderTarget (wId, hId);
    }
    if (id == 0) return false;

    f.rtId = id;
    f.width = w;
    f.height = h;
    f.dirty = false;

    if (f.color.objectType == K_OBJECT_TEXTURE) {
        const GlVkCoreState* cs = gl_vk_core_state ();
        if (cs && cs->setTextureId) {
            uint32_t old = gl_vk_texture_id (f.color.name);
            if (old != 0 && old < 1000) {
                // 这张纹理原来的像素会被离屏内容取代，先释放显存
                r->DestroyTexture (old);
            }
            cs->setTextureId (f.color.name, id);
        }
    }
    return true;
}

bool FboTargetReady (int fboName) {
    Fbo& f = g_fbos[(size_t)fboName];
    int w = 0, h = 0;
    if (!AttachmentSize (f.color, &w, &h)) return false;
    if (f.rtId == 0 || f.dirty || f.width != w || f.height != h) {
        return EnsureFboTarget (fboName);
    }
    // 纹理被重新 texImage2D 后别名会被 gl_core 冲掉，这里检测到就重建
    if (f.color.objectType == K_OBJECT_TEXTURE &&
        gl_vk_texture_id (f.color.name) != f.rtId) {
        return EnsureFboTarget (fboName);
    }
    return true;
}

void BindDrawFbo (int framebuffer) {
    if (framebuffer != 0 && !IsFboName (framebuffer)) return;

    bool alreadyOpen = false;
    if (framebuffer == g_boundDrawFbo) {
        if (framebuffer == 0) {
            alreadyOpen = g_backbufferOpen;
        } else {
            alreadyOpen = (g_currentTarget == g_fbos[(size_t)framebuffer].rtId);
        }
    }
    if (alreadyOpen) return;

    CloseOpenFrame ();
    g_boundDrawFbo = framebuffer;

    VkRenderer* r = gl_vk_get_renderer ();
    if (!r) return;

    if (framebuffer == 0) {
        float c[4];
        GetClearColor (c);
        if (r->BindRenderTarget (0, c[0], c[1], c[2], c[3])) {
            g_backbufferOpen = true;
        }
        return;
    }

    if (!FboTargetReady (framebuffer)) return;
    float c[4];
    GetClearColor (c);
    Fbo& f = g_fbos[(size_t)framebuffer];
    if (r->BindRenderTarget (f.rtId, c[0], c[1], c[2], c[3])) {
        g_currentTarget = f.rtId;
    }
}

void MarkFbosReferencingRbo (int rboName) {
    for (size_t i = 1; i < g_fbos.size (); i++) {
        Fbo& f = g_fbos[i];
        if (!f.used) continue;
        if ((f.color.objectType == K_OBJECT_RENDERBUFFER && f.color.name == rboName) ||
            (f.depth.objectType == K_OBJECT_RENDERBUFFER && f.depth.name == rboName) ||
            (f.stencil.objectType == K_OBJECT_RENDERBUFFER && f.stencil.name == rboName)) {
            f.dirty = true;
        }
    }
}

const Attachment* FindAttachment (Fbo& f, int attachment) {
    switch (attachment) {
        case K_COLOR_ATTACHMENT0:
        case K_COLOR_ATTACHMENT1:
        case K_COLOR_ATTACHMENT2:
        case K_COLOR_ATTACHMENT3: return &f.color;
        case K_DEPTH_ATTACHMENT: return &f.depth;
        case K_STENCIL_ATTACHMENT: return &f.stencil;
        case K_DEPTH_STENCIL_ATTACHMENT: return &f.depth;   // 与 stencil 共用
        default: return nullptr;
    }
}

} // 匿名 namespace

// ---------------------------------------------------------------- FBO 入口

int lime_gl_create_framebuffer () {
    int name = g_nextFboName++;
    EnsureFboSlot (name);
    g_fbos[(size_t)name] = Fbo ();
    g_fbos[(size_t)name].used = true;
    return name;
}

void lime_gl_delete_framebuffer (int framebuffer) {
    if (!IsFboName (framebuffer)) return;
    if (g_boundDrawFbo == framebuffer || g_boundReadFbo == framebuffer) {
        CloseOpenFrame ();
        if (g_boundDrawFbo == framebuffer) g_boundDrawFbo = 0;
        if (g_boundReadFbo == framebuffer) g_boundReadFbo = 0;
    }
    Fbo& f = g_fbos[(size_t)framebuffer];
    DestroyFboTarget (f);
    if (f.color.objectType == K_OBJECT_TEXTURE) {
        const GlVkCoreState* cs = gl_vk_core_state ();
        if (cs && cs->setTextureId) cs->setTextureId (f.color.name, 0);
    }
    f = Fbo ();
}

bool lime_gl_is_framebuffer (int handle) {
    return IsFboName (handle);
}

void lime_gl_bind_framebuffer (int target, int framebuffer) {
    if (target != K_FRAMEBUFFER && target != K_READ_FRAMEBUFFER &&
        target != K_DRAW_FRAMEBUFFER) {
        return;
    }
    if (framebuffer < 0) return;
    if (framebuffer != 0 && !IsFboName (framebuffer)) return;

    if (target == K_READ_FRAMEBUFFER) {
        g_boundReadFbo = framebuffer;
        return;
    }
    BindDrawFbo (framebuffer);
    if (target == K_FRAMEBUFFER) g_boundReadFbo = framebuffer;
}

void lime_gl_framebuffer_texture2D (int target, int attachment, int textarget,
                                    int texture, int level) {
    int fboName = BoundFboName (target);
    if (!IsFboName (fboName)) return;

    Fbo& f = g_fbos[(size_t)fboName];
    int face = 0;
    // 0x8515..0x851A = cube map 各面
    if (textarget >= 0x8515 && textarget <= 0x851A) face = textarget;

    Attachment a;
    if (texture != 0) {
        a.objectType = K_OBJECT_TEXTURE;
        a.name = texture;
        a.level = level;
        a.face = face;
    }

    switch (attachment) {
        case K_COLOR_ATTACHMENT0:
        case K_COLOR_ATTACHMENT1:
        case K_COLOR_ATTACHMENT2:
        case K_COLOR_ATTACHMENT3:
            // 只支持单颜色附件（VkRenderer 的 RT 只有一个颜色附件）
            if (attachment != K_COLOR_ATTACHMENT0) return;
            f.color = a;
            break;
        case K_DEPTH_ATTACHMENT: f.depth = a; break;
        case K_STENCIL_ATTACHMENT: f.stencil = a; break;
        case K_DEPTH_STENCIL_ATTACHMENT: f.depth = a; f.stencil = a; break;
        default: return;
    }
    f.dirty = true;
}

void lime_gl_framebuffer_renderbuffer (int target, int attachment,
                                       int renderbuffertarget, int renderbuffer) {
    if (renderbuffertarget != K_RENDERBUFFER) return;
    int fboName = BoundFboName (target);
    if (!IsFboName (fboName)) return;
    if (renderbuffer != 0 && !IsRboName (renderbuffer)) return;

    Fbo& f = g_fbos[(size_t)fboName];
    Attachment a;
    if (renderbuffer != 0) {
        a.objectType = K_OBJECT_RENDERBUFFER;
        a.name = renderbuffer;
    }

    switch (attachment) {
        case K_COLOR_ATTACHMENT0: f.color = a; break;
        case K_DEPTH_ATTACHMENT: f.depth = a; break;
        case K_STENCIL_ATTACHMENT: f.stencil = a; break;
        case K_DEPTH_STENCIL_ATTACHMENT: f.depth = a; f.stencil = a; break;
        default: return;
    }
    f.dirty = true;
}

void lime_gl_framebuffer_texture_layer (int target, int attachment, int texture,
                                        int level, int layer) {
    // 2D array / 3D 纹理的分层附件：VkRenderer 暂不支持，按未支持处理
    (void)target; (void)attachment; (void)texture; (void)level; (void)layer;
}

int lime_gl_check_framebuffer_status (int target) {
    int fboName = (target == K_READ_FRAMEBUFFER) ? g_boundReadFbo : g_boundDrawFbo;
    // 默认帧缓冲：GLES 规定返回 FRAMEBUFFER_COMPLETE
    if (fboName == 0) return K_FB_COMPLETE;
    if (!IsFboName (fboName)) return K_FB_INCOMPLETE_ATTACHMENT;

    Fbo& f = g_fbos[(size_t)fboName];

    if (f.color.objectType == K_OBJECT_NONE &&
        f.depth.objectType == K_OBJECT_NONE &&
        f.stencil.objectType == K_OBJECT_NONE) {
        return K_FB_INCOMPLETE_MISSING_ATTACHMENT;
    }
    // 只有 depth/stencil：VkRenderer 没有该通道，明确报不支持而不是假成功
    if (f.color.objectType == K_OBJECT_NONE) return K_FB_UNSUPPORTED;

    int cw = 0, ch = 0;
    if (!AttachmentSize (f.color, &cw, &ch) || cw <= 0 || ch <= 0) {
        return K_FB_INCOMPLETE_ATTACHMENT;
    }

    if (f.depth.objectType != K_OBJECT_NONE) {
        int dw = 0, dh = 0;
        if (!AttachmentSize (f.depth, &dw, &dh) || dw <= 0 || dh <= 0) {
            return K_FB_INCOMPLETE_ATTACHMENT;
        }
        if (dw != cw || dh != ch) return K_FB_INCOMPLETE_ATTACHMENT;
        if (f.depth.objectType == K_OBJECT_RENDERBUFFER &&
            g_rbos[(size_t)f.depth.name].samples > 1) {
            return K_FB_INCOMPLETE_MULTISAMPLE;
        }
    }
    if (f.stencil.objectType != K_OBJECT_NONE) {
        int sw = 0, sh = 0;
        if (!AttachmentSize (f.stencil, &sw, &sh) || sw <= 0 || sh <= 0) {
            return K_FB_INCOMPLETE_ATTACHMENT;
        }
        if (sw != cw || sh != ch) return K_FB_INCOMPLETE_ATTACHMENT;
    }

    // 真正去分配显存，分配失败按 UNSUPPORTED 上报
    if (!FboTargetReady (fboName)) return K_FB_UNSUPPORTED;
    return K_FB_COMPLETE;
}

int lime_gl_get_framebuffer_attachment_parameteri (int target, int attachment,
                                                   int pname) {
    int fboName = BoundFboName (target);
    if (!IsFboName (fboName)) return 0;

    Fbo& f = g_fbos[(size_t)fboName];
    const Attachment* a = FindAttachment (f, attachment);
    if (!a || a->objectType == K_OBJECT_NONE) return 0;

    switch (pname) {
        case K_ATTACH_OBJECT_TYPE: return a->objectType;
        case K_ATTACH_OBJECT_NAME: return a->name;
        case K_ATTACH_TEXTURE_LEVEL: return a->level;
        case K_ATTACH_TEXTURE_CUBE_FACE: return a->face;
        case K_ATTACH_TEXTURE_LAYER: return 0;
        case K_ATTACH_LAYERED: return 0;
        case K_ATTACH_COLOR_ENCODING: return K_LINEAR;
        case K_ATTACH_COMPONENT_TYPE: return K_UNSIGNED_NORMALIZED;
        default: return 0;   // 组件位宽查询暂不提供（GLES3 少用）
    }
}

void lime_gl_get_framebuffer_attachment_parameteriv (int target, int attachment,
                                                     int pname, double params) {
    double* out = (double*)(uintptr_t)params;
    if (!out) return;
    out[0] = (double)lime_gl_get_framebuffer_attachment_parameteri (target, attachment, pname);
}

// ---------------------------------------------------------------- RBO 入口

int lime_gl_create_renderbuffer () {
    int name = g_nextRboName++;
    EnsureRboSlot (name);
    g_rbos[(size_t)name] = Rbo ();
    g_rbos[(size_t)name].used = true;
    return name;
}

void lime_gl_delete_renderbuffer (int renderbuffer) {
    if (!IsRboName (renderbuffer)) return;
    for (size_t i = 1; i < g_fbos.size (); i++) {
        Fbo& f = g_fbos[i];
        if (!f.used) continue;
        if (f.color.objectType == K_OBJECT_RENDERBUFFER && f.color.name == renderbuffer)
            f.color = Attachment ();
        if (f.depth.objectType == K_OBJECT_RENDERBUFFER && f.depth.name == renderbuffer)
            f.depth = Attachment ();
        if (f.stencil.objectType == K_OBJECT_RENDERBUFFER && f.stencil.name == renderbuffer)
            f.stencil = Attachment ();
        f.dirty = true;
    }
    if (g_boundRbo == renderbuffer) g_boundRbo = 0;
    g_rbos[(size_t)renderbuffer] = Rbo ();
}

bool lime_gl_is_renderbuffer (int handle) {
    return IsRboName (handle);
}

void lime_gl_bind_renderbuffer (int target, int renderbuffer) {
    if (target != K_RENDERBUFFER) return;
    if (renderbuffer < 0) return;
    if (renderbuffer != 0 && !IsRboName (renderbuffer)) return;
    g_boundRbo = renderbuffer;
}

void lime_gl_renderbuffer_storage (int target, int internalformat, int width, int height) {
    if (target != K_RENDERBUFFER || !IsRboName (g_boundRbo)) return;
    Rbo& b = g_rbos[(size_t)g_boundRbo];
    b.width = (width > 0) ? width : 0;
    b.height = (height > 0) ? height : 0;
    b.internalFormat = internalformat;
    b.samples = 0;
    MarkFbosReferencingRbo (g_boundRbo);
}

void lime_gl_renderbuffer_storage_multisample (int target, int samples,
                                               int internalformat, int width, int height) {
    if (target != K_RENDERBUFFER || !IsRboName (g_boundRbo)) return;
    Rbo& b = g_rbos[(size_t)g_boundRbo];
    b.width = (width > 0) ? width : 0;
    b.height = (height > 0) ? height : 0;
    b.internalFormat = internalformat;
    b.samples = (samples > 1) ? samples : 0;
    MarkFbosReferencingRbo (g_boundRbo);
}

int lime_gl_get_renderbuffer_parameteri (int target, int pname) {
    if (target != K_RENDERBUFFER || !IsRboName (g_boundRbo)) return 0;
    Rbo& b = g_rbos[(size_t)g_boundRbo];
    switch (pname) {
        case K_RB_WIDTH: return b.width;
        case K_RB_HEIGHT: return b.height;
        case K_RB_INTERNAL_FORMAT: return b.internalFormat;
        case K_RB_SAMPLES: return b.samples;
        default: return 0;
    }
}

void lime_gl_get_renderbuffer_parameteriv (int target, int pname, double params) {
    double* out = (double*)(uintptr_t)params;
    if (!out) return;
    out[0] = (double)lime_gl_get_renderbuffer_parameteri (target, pname);
}

// ---------------------------------------------------------------- 其余 FBO 入口
// 这些入口在原 GL 里要么只是提示/优化，要么需要 VkRenderer 提供额外能力；
// 这里保持“不改变真实渲染结果”的语义，不假装实现。

void lime_gl_draw_buffers (value buffers) {
    // 单颜色附件：MRT 的其余 draw buffer 直接忽略
    (void)buffers;
}

void lime_gl_invalidate_framebuffer (int target, value attachments) {
    (void)target; (void)attachments;
}

void lime_gl_invalidate_sub_framebuffer (int target, value attachments,
                                         int x, int y, int width, int height) {
    (void)target; (void)attachments; (void)x; (void)y; (void)width; (void)height;
}

void lime_gl_blit_framebuffer (int srcX0, int srcY0, int srcX1, int srcY1,
                               int dstX0, int dstY0, int dstX1, int dstY1,
                               int mask, int filter) {
    (void)filter;
    if (!(mask & 0x4000)) return;   // GL_COLOR_BUFFER_BIT

    VkRenderer* r = gl_vk_get_renderer ();
    if (!r) return;

    // 源：READ_FRAMEBUFFER 的颜色附件（纹理名已被 alias 成 RT id，>=1000）
    uint32_t srcTex = 0;
    int sw = 0, sh = 0;
    int readFbo = g_boundReadFbo;
    if (readFbo > 0 && IsFboName (readFbo)) {
        Fbo& f = g_fbos[(size_t)readFbo];
        if (f.color.objectType == K_OBJECT_TEXTURE) {
            srcTex = gl_vk_texture_id (f.color.name);
            gl_vk_texture_size (f.color.name, &sw, &sh);
        }
    }
    if (!srcTex) return;

    // 用一次带贴图的四边形代替 vkCmdBlitImage：目标当前绑定的 pass 已经开着
    float quad[24] = {
        (float)dstX0, (float)dstY0, (float)srcX0, (float)srcY0,
        (float)dstX1, (float)dstY0, (float)srcX1, (float)srcY0,
        (float)dstX0, (float)dstY1, (float)srcX0, (float)srcY1,
        (float)dstX0, (float)dstY1, (float)srcX0, (float)srcY1,
        (float)dstX1, (float)dstY0, (float)srcX1, (float)srcY0,
        (float)dstX1, (float)dstY1, (float)srcX1, (float)srcY1,
    };
    r->DrawRaw (srcTex, quad, 6);
}

// ---------------------------------------------------------------- 帧目标对接
uint32_t gl_vk_current_target () {
    return g_currentTarget;
}

bool gl_vk_handle_clear () {
    VkRenderer* r = gl_vk_get_renderer ();
    if (!r) return false;

    float c[4];
    GetClearColor (c);

    if (g_currentTarget != 0) {
        // 离屏目标：帧已打开，这里只更新清屏色（不会重新 Acquire 交换链）
        r->BindRenderTarget (g_currentTarget, c[0], c[1], c[2], c[3]);
        return true;
    }
    if (!g_backbufferOpen) {
        r->BindRenderTarget (0, c[0], c[1], c[2], c[3]);
        g_backbufferOpen = true;
    }
    // 后缓冲帧已经打开：清屏色在打开时已取，EndFrame 时才真正使用
    return true;
}

void gl_vk_present_frame () {
    VkRenderer* r = gl_vk_get_renderer ();
    if (r) {
        // 关键：OpenFL 的整帧常常停在最后一个离屏目标上，如果直接 EndFrame，
        // 渲染结果只留在 FBO 里，后缓冲从来没被画过 → 屏幕只有清屏色（黑屏）。
        // 所以提交前先把最后一张离屏图铺满后缓冲。
        if (g_currentTarget >= 1000) {
            uint32_t src = g_currentTarget;
            float c[4];
            GetClearColor (c);
            int tw = 0, th = 0;
            bool bound = r->BindRenderTarget (0, c[0], c[1], c[2], c[3]);
            if (bound && r->GetRenderTargetSize (src, &tw, &th) && tw > 0 && th > 0) {
                float W = (float)r->ScreenWidth ();
                float H = (float)r->ScreenHeight ();
                float quad[24] = {
                    0.0f, 0.0f, 0.0f, 0.0f,
                    W,    0.0f, (float)tw, 0.0f,
                    0.0f, H,    0.0f, (float)th,
                    0.0f, H,    0.0f, (float)th,
                    W,    0.0f, (float)tw, 0.0f,
                    W,    H,    (float)tw, (float)th,
                };
                r->DrawRaw (src, quad, 6);
                g_backbufferOpen = true;
            }
        }
        r->EndFrame ();
    }
    g_currentTarget = 0;
    g_backbufferOpen = false;
    // 帧结束后绘制目标回到后缓冲，避免跨帧残留指向已提交的离屏目标
    g_boundDrawFbo = 0;
}

} // namespace lime
