// =============================================================================
// Vulkan 翻译层：纹理参数 / 子区域更新 / 像素存储 / mipmap
//
// 映射思路：
//   纹理参数（过滤、wrap、mip 范围）保存在本模块里，作为每个 GL 纹理名的
//   sampler 状态；通过 GlVkRendererExt::setTextureSampler 真正作用到 VkRenderer
//   的 sampler。texSubImage2D 走 GlVkRendererExt::updateSubImage 做子区域上传，
//   texStorage2D 走 GlVkRendererExt::createEmptyTexture 分配空纹理。
//   这些 VkRenderer 扩展接口由主线补齐；未注册时本模块只记录状态，不谎报成功。
//
// 依边界要求，本文件不修改 gl_stubs.cpp / gl_core.cpp / vk_renderer.*。
// =============================================================================

#include "gl_vk_bridge.h"   // 内含 gl_compat.h
#include "gl_vk_contract.h"
#include "vk_renderer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lime {

namespace {

// ---------------------------------------------------------------- GL 常量
enum {
    K_TEXTURE_2D = 0x0DE1,
    K_TEXTURE_3D = 0x806F,
    K_TEXTURE_CUBE_MAP = 0x8513,
    K_TEXTURE_2D_ARRAY = 0x8C1A,

    K_TEXTURE_MAG_FILTER = 0x2800,
    K_TEXTURE_MIN_FILTER = 0x2801,
    K_TEXTURE_WRAP_S = 0x2802,
    K_TEXTURE_WRAP_T = 0x2803,
    K_TEXTURE_MIN_LOD = 0x813A,
    K_TEXTURE_MAX_LOD = 0x813B,
    K_TEXTURE_BASE_LEVEL = 0x813C,
    K_TEXTURE_MAX_LEVEL = 0x813D,
    K_TEXTURE_COMPARE_MODE = 0x884C,
    K_TEXTURE_COMPARE_FUNC = 0x884D,
    K_TEXTURE_SWIZZLE_R = 0x8E42,
    K_TEXTURE_SWIZZLE_G = 0x8E43,
    K_TEXTURE_SWIZZLE_B = 0x8E44,
    K_TEXTURE_SWIZZLE_A = 0x8E45,

    K_NEAREST = 0x2600,
    K_LINEAR = 0x2601,
    K_NEAREST_MIPMAP_NEAREST = 0x2700,
    K_LINEAR_MIPMAP_NEAREST = 0x2701,
    K_NEAREST_MIPMAP_LINEAR = 0x2702,
    K_LINEAR_MIPMAP_LINEAR = 0x2703,
    K_REPEAT = 0x2901,
    K_CLAMP_TO_EDGE = 0x812F,
    K_MIRRORED_REPEAT = 0x8370,

    K_UNPACK_ALIGNMENT = 0x0CF5,
    K_UNPACK_ROW_LENGTH = 0x0CF2,
    K_UNPACK_SKIP_ROWS = 0x0CF3,
    K_UNPACK_SKIP_PIXELS = 0x0CF4,
    K_UNPACK_IMAGE_HEIGHT = 0x806E,
    K_UNPACK_SKIP_IMAGES = 0x806D,

    K_PACK_ALIGNMENT = 0x0D05,
    K_PACK_ROW_LENGTH = 0x0D02,
    K_PACK_SKIP_ROWS = 0x0D03,
    K_PACK_SKIP_PIXELS = 0x0D04,

    K_UNSIGNED_BYTE = 0x1401,
    K_UNSIGNED_SHORT_5_6_5 = 0x8363,
    K_UNSIGNED_SHORT_4_4_4_4 = 0x8033,
    K_UNSIGNED_SHORT_5_5_5_1 = 0x8034,

    K_RED = 0x1903,
    K_RG = 0x8227,
    K_RGB = 0x1907,
    K_RGBA = 0x1908,
    K_ALPHA = 0x1906,
    K_LUMINANCE = 0x1909,
    K_LUMINANCE_ALPHA = 0x190A,

    K_NUM_SAMPLE_COUNTS = 0x9380,
    K_SAMPLES = 0x80A9
};

struct TexParams {
    int minFilter;
    int magFilter;
    int wrapS;
    int wrapT;
    int baseLevel;
    int maxLevel;
    float minLod;
    float maxLod;
    int compareMode;
    int compareFunc;
    int width;       // texStorage2D 记录的尺寸（texImage2D 的由 gl_core 记录）
    int height;
    int mipLevels;
    TexParams ()
        : minFilter (K_NEAREST_MIPMAP_LINEAR), magFilter (K_LINEAR),
          wrapS (K_REPEAT), wrapT (K_REPEAT),
          baseLevel (0), maxLevel (1000),
          minLod (-1000.0f), maxLod (1000.0f),
          compareMode (0), compareFunc (0x0203),
          width (0), height (0), mipLevels (1) {}
};

// 索引 = GL 纹理名；索引 0 对应默认纹理
std::vector<TexParams> g_texParams (1);

// pixelStore 状态（UNPACK 影响上传，PACK 供将来的回读使用）
int g_unpackAlignment = 4;
int g_unpackRowLength = 0;
int g_unpackSkipRows = 0;
int g_unpackSkipPixels = 0;
int g_packAlignment = 4;
int g_packRowLength = 0;
int g_packSkipRows = 0;
int g_packSkipPixels = 0;

// ---------------------------------------------------------------- 小工具
int BoundTextureName () {
    const GlVkCoreState* cs = gl_vk_core_state ();
    if (cs && cs->getBoundTexture) return cs->getBoundTexture ();
    return 0;
}

TexParams& ParamsFor (int glName) {
    int idx = (glName > 0) ? glName : 0;
    if ((size_t)idx >= g_texParams.size ()) {
        g_texParams.resize ((size_t)idx + 1);
    }
    return g_texParams[(size_t)idx];
}

bool IsTextureTarget (int target) {
    return target == K_TEXTURE_2D || target == K_TEXTURE_CUBE_MAP ||
           target == K_TEXTURE_3D || target == K_TEXTURE_2D_ARRAY;
}

int ComputeMipLevels (int w, int h) {
    int m = (w > h) ? w : h;
    int levels = 1;
    while (m > 1) { m >>= 1; levels++; }
    return levels;
}

// 把纹理参数推到 VkRenderer 的 sampler 上（离屏 RT 用统一 sampler，跳过）
void ApplySampler (int glName) {
    uint32_t id = gl_vk_texture_id (glName);
    if (id == 0 || id >= 1000) return;
    const GlVkRendererExt* ext = gl_vk_renderer_ext ();
    if (!ext || !ext->setTextureSampler) return;
    TexParams& p = ParamsFor (glName);
    ext->setTextureSampler (id, p.minFilter, p.magFilter,
                            p.wrapS, p.wrapT, p.baseLevel, p.maxLevel,
                            p.minLod, p.maxLod);
}

int ChannelsOfFormat (int format) {
    switch (format) {
        case K_RGBA: return 4;
        case K_RGB: return 3;
        case K_RG: return 2;
        case K_LUMINANCE_ALPHA: return 2;
        case K_ALPHA:
        case K_LUMINANCE:
        case K_RED: return 1;
        default: return 0;
    }
}

// 按 UNPACK 状态把源数据展开成紧凑 RGBA8
bool ConvertUnpacked (int format, int type, int width, int height,
                      const unsigned char* src, std::vector<unsigned char>& out) {
    if (type != K_UNSIGNED_BYTE) return false;   // 目前只处理 UNSIGNED_BYTE
    int ch = ChannelsOfFormat (format);
    if (ch <= 0) return false;

    int align = (g_unpackAlignment > 0) ? g_unpackAlignment : 4;
    size_t rowPixels = (size_t)((g_unpackRowLength > 0) ? g_unpackRowLength : width);
    size_t rowBytes = rowPixels * (size_t)ch;
    size_t stride = (rowBytes + (size_t)align - 1) / (size_t)align * (size_t)align;

    out.resize ((size_t)width * (size_t)height * 4);
    const unsigned char* base = src +
        (size_t)g_unpackSkipRows * stride +
        (size_t)g_unpackSkipPixels * (size_t)ch;

    for (int y = 0; y < height; y++) {
        const unsigned char* row = base + (size_t)y * stride;
        for (int x = 0; x < width; x++) {
            const unsigned char* px = row + (size_t)x * (size_t)ch;
            unsigned char* d = &out[((size_t)y * (size_t)width + (size_t)x) * 4];
            switch (ch) {
                case 1:
                    if (format == K_ALPHA) {
                        d[0] = 0; d[1] = 0; d[2] = 0; d[3] = px[0];
                    } else {
                        d[0] = px[0]; d[1] = px[0]; d[2] = px[0]; d[3] = 255;
                    }
                    break;
                case 2:
                    if (format == K_LUMINANCE_ALPHA) {
                        d[0] = px[0]; d[1] = px[0]; d[2] = px[0]; d[3] = px[1];
                    } else {   // GL_RG
                        d[0] = px[0]; d[1] = px[1]; d[2] = 0; d[3] = 255;
                    }
                    break;
                case 3:
                    d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = 255;
                    break;
                case 4:
                    memcpy (d, px, 4);
                    break;
            }
        }
    }
    return true;
}

void WriteIntParam (double params, int v) {
    double* out = (double*)(uintptr_t)params;
    if (out) out[0] = (double)v;
}

void WriteFloatParam (double params, float v) {
    double* out = (double*)(uintptr_t)params;
    if (out) out[0] = (double)v;
}

} // 匿名 namespace

// ---------------------------------------------------------------- 查询辅助
uint32_t gl_vk_texture_id (int glName) {
    if (glName <= 0) return 0;
    const GlVkCoreState* cs = gl_vk_core_state ();
    if (cs && cs->getTextureId) return cs->getTextureId (glName);
    return 0;
}

bool gl_vk_texture_size (int glName, int* width, int* height) {
    if (glName <= 0) return false;
    const GlVkCoreState* cs = gl_vk_core_state ();
    if (cs && cs->getTextureSize &&
        cs->getTextureSize (glName, width, height) &&
        *width > 0 && *height > 0) {
        return true;
    }
    // texStorage2D 分配的空纹理由本模块记录尺寸
    if ((size_t)glName < g_texParams.size ()) {
        TexParams& p = g_texParams[(size_t)glName];
        if (p.width > 0 && p.height > 0) {
            *width = p.width;
            *height = p.height;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- texParameter
void lime_gl_tex_parameteri (int target, int pname, int param) {
    if (!IsTextureTarget (target)) return;
    int name = BoundTextureName ();
    TexParams& p = ParamsFor (name);
    switch (pname) {
        case K_TEXTURE_MIN_FILTER: p.minFilter = param; break;
        case K_TEXTURE_MAG_FILTER: p.magFilter = param; break;
        case K_TEXTURE_WRAP_S: p.wrapS = param; break;
        case K_TEXTURE_WRAP_T: p.wrapT = param; break;
        case K_TEXTURE_BASE_LEVEL: p.baseLevel = param; break;
        case K_TEXTURE_MAX_LEVEL: p.maxLevel = param; break;
        case K_TEXTURE_MIN_LOD: p.minLod = (float)param; break;
        case K_TEXTURE_MAX_LOD: p.maxLod = (float)param; break;
        case K_TEXTURE_COMPARE_MODE: p.compareMode = param; break;
        case K_TEXTURE_COMPARE_FUNC: p.compareFunc = param; break;
        default: return;
    }
    ApplySampler (name);
}

void lime_gl_tex_parameterf (int target, int pname, float param) {
    if (!IsTextureTarget (target)) return;
    int name = BoundTextureName ();
    TexParams& p = ParamsFor (name);
    switch (pname) {
        case K_TEXTURE_MIN_FILTER: p.minFilter = (int)param; break;
        case K_TEXTURE_MAG_FILTER: p.magFilter = (int)param; break;
        case K_TEXTURE_WRAP_S: p.wrapS = (int)param; break;
        case K_TEXTURE_WRAP_T: p.wrapT = (int)param; break;
        case K_TEXTURE_BASE_LEVEL: p.baseLevel = (int)param; break;
        case K_TEXTURE_MAX_LEVEL: p.maxLevel = (int)param; break;
        case K_TEXTURE_MIN_LOD: p.minLod = param; break;
        case K_TEXTURE_MAX_LOD: p.maxLod = param; break;
        case K_TEXTURE_COMPARE_MODE: p.compareMode = (int)param; break;
        case K_TEXTURE_COMPARE_FUNC: p.compareFunc = (int)param; break;
        default: return;
    }
    ApplySampler (name);
}

int lime_gl_get_tex_parameteri (int target, int pname) {
    if (!IsTextureTarget (target)) return 0;
    TexParams& p = ParamsFor (BoundTextureName ());
    switch (pname) {
        case K_TEXTURE_MIN_FILTER: return p.minFilter;
        case K_TEXTURE_MAG_FILTER: return p.magFilter;
        case K_TEXTURE_WRAP_S: return p.wrapS;
        case K_TEXTURE_WRAP_T: return p.wrapT;
        case K_TEXTURE_BASE_LEVEL: return p.baseLevel;
        case K_TEXTURE_MAX_LEVEL: return p.maxLevel;
        case K_TEXTURE_MIN_LOD: return (int)p.minLod;
        case K_TEXTURE_MAX_LOD: return (int)p.maxLod;
        case K_TEXTURE_COMPARE_MODE: return p.compareMode;
        case K_TEXTURE_COMPARE_FUNC: return p.compareFunc;
        default: return 0;
    }
}

float lime_gl_get_tex_parameterf (int target, int pname) {
    if (!IsTextureTarget (target)) return 0.0f;
    TexParams& p = ParamsFor (BoundTextureName ());
    switch (pname) {
        case K_TEXTURE_MIN_LOD: return p.minLod;
        case K_TEXTURE_MAX_LOD: return p.maxLod;
        default: return (float)lime_gl_get_tex_parameteri (target, pname);
    }
}

void lime_gl_get_tex_parameterfv (int target, int pname, double params) {
    WriteFloatParam (params, lime_gl_get_tex_parameterf (target, pname));
}

void lime_gl_get_tex_parameteriv (int target, int pname, double params) {
    WriteIntParam (params, lime_gl_get_tex_parameteri (target, pname));
}

// ---------------------------------------------------------------- pixelStore
void lime_gl_pixel_storei (int pname, int param) {
    switch (pname) {
        case K_UNPACK_ALIGNMENT:
            if (param == 1 || param == 2 || param == 4 || param == 8) g_unpackAlignment = param;
            break;
        case K_UNPACK_ROW_LENGTH: g_unpackRowLength = (param > 0) ? param : 0; break;
        case K_UNPACK_SKIP_ROWS: g_unpackSkipRows = (param > 0) ? param : 0; break;
        case K_UNPACK_SKIP_PIXELS: g_unpackSkipPixels = (param > 0) ? param : 0; break;
        case K_PACK_ALIGNMENT:
            if (param == 1 || param == 2 || param == 4 || param == 8) g_packAlignment = param;
            break;
        case K_PACK_ROW_LENGTH: g_packRowLength = (param > 0) ? param : 0; break;
        case K_PACK_SKIP_ROWS: g_packSkipRows = (param > 0) ? param : 0; break;
        case K_PACK_SKIP_PIXELS: g_packSkipPixels = (param > 0) ? param : 0; break;
        default: break;   // UNPACK_IMAGE_HEIGHT / SKIP_IMAGES 只对 3D 有效，忽略
    }
}

// ---------------------------------------------------------------- 子区域上传
void lime_gl_tex_sub_image_2d (int target, int level, int xoffset, int yoffset,
                               int width, int height, int format, int type,
                               double data) {
    if (!IsTextureTarget (target) || width <= 0 || height <= 0 || level < 0) return;
    const unsigned char* src = (const unsigned char*)(uintptr_t)data;
    if (!src) return;

    int name = BoundTextureName ();
    if (name <= 0) return;
    uint32_t id = gl_vk_texture_id (name);
    if (id == 0) return;   // 首次分配由 gl_core 的 texImage2D 负责
    if (id >= 1000) return;   // 这是 FBO 别名过来的离屏目标，不接受子区域上传

    std::vector<unsigned char> rgba;
    if (!ConvertUnpacked (format, type, width, height, src, rgba)) return;

    const GlVkRendererExt* ext = gl_vk_renderer_ext ();
    if (!ext || !ext->updateSubImage) return;   // 待主线补接口
    ext->updateSubImage (id, level, xoffset, yoffset, width, height, rgba.data ());
}

void lime_gl_tex_sub_image_3d (int target, int level, int xoffset, int yoffset,
                               int zoffset, int width, int height, int depth,
                               int format, int type, double data) {
    // 3D 纹理未支持
    (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset;
    (void)width; (void)height; (void)depth; (void)format; (void)type; (void)data;
}

// ---------------------------------------------------------------- 不可变存储
void lime_gl_tex_storage_2d (int target, int levels, int internalformat,
                             int width, int height) {
    if (!IsTextureTarget (target) || width <= 0 || height <= 0) return;
    int name = BoundTextureName ();
    if (name <= 0) return;

    TexParams& p = ParamsFor (name);
    p.width = width;
    p.height = height;
    p.mipLevels = (levels > 0) ? levels : ComputeMipLevels (width, height);
    p.maxLevel = p.mipLevels - 1;

    if (gl_vk_texture_id (name) != 0) return;
    const GlVkRendererExt* ext = gl_vk_renderer_ext ();
    if (!ext || !ext->createEmptyTexture) return;   // 待主线补接口
    uint32_t id = ext->createEmptyTexture (width, height);
    if (id == 0) return;
    const GlVkCoreState* cs = gl_vk_core_state ();
    if (cs && cs->setTextureId) {
        cs->setTextureId (name, id);
        ApplySampler (name);
    }
}

void lime_gl_tex_storage_3d (int target, int levels, int internalformat,
                             int width, int height, int depth) {
    // 3D 纹理未支持
    (void)target; (void)levels; (void)internalformat;
    (void)width; (void)height; (void)depth;
}

void lime_gl_generate_mipmap (int target) {
    if (!IsTextureTarget (target)) return;
    int name = BoundTextureName ();
    if (name <= 0) return;
    uint32_t id = gl_vk_texture_id (name);
    if (id == 0 || id >= 1000) return;

    TexParams& p = ParamsFor (name);
    int w = p.width, h = p.height;
    if (w <= 0 || h <= 0) gl_vk_texture_size (name, &w, &h);
    int levels = ComputeMipLevels (w, h);
    if (levels <= 1) {
        // 尺寸未知时（gl_core 还没记录 texImage2D 尺寸）无从计算，保持单层
        return;
    }
    const GlVkRendererExt* ext = gl_vk_renderer_ext ();
    if (!ext || !ext->generateMipmaps) return;   // 待主线补接口
    if (ext->generateMipmaps (id, levels)) {
        p.mipLevels = levels;
        p.maxLevel = levels - 1;
        ApplySampler (name);
    }
}

// ---------------------------------------------------------------- 回读类入口
// copyTexImage2D / copyTexSubImage2D / compressed* 需要 VkRenderer 提供离屏回读
// （GlVkRendererExt::readPixels）后才可落地；当前保持“不改变真实渲染”的语义。

void lime_gl_copy_tex_image_2d (int target, int level, int internalformat,
                                int x, int y, int width, int height, int border) {
    // 取证：这两个入口当前是空实现。若游戏真的走 copyTex*（把帧缓冲内容拷进纹理），
    // 那这条路就是"纹理永远为空"的直接原因，必须计数确认调用量。
    static int s_n = 0;
    if (s_n < 20) {
        fprintf(stderr, "[glcopy] image2d n=%d target=0x%x level=%d fmt=0x%x rect=(%d,%d %dx%d)\n",
                s_n + 1, target, level, internalformat, x, y, width, height);
    }
    s_n++;
}

void lime_gl_copy_tex_sub_image_2d (int target, int level, int xoffset, int yoffset,
                                    int x, int y, int width, int height) {
    static int s_n = 0;
    if (s_n < 20) {
        fprintf(stderr, "[glcopy] sub2d n=%d target=0x%x level=%d off=(%d,%d) rect=(%d,%d %dx%d)\n",
                s_n + 1, target, level, xoffset, yoffset, x, y, width, height);
    }
    s_n++;
}

void lime_gl_copy_tex_sub_image_3d (int target, int level, int xoffset, int yoffset,
                                    int zoffset, int x, int y, int width, int height) {
    (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset;
    (void)x; (void)y; (void)width; (void)height;
}

void lime_gl_compressed_tex_image_2d (int target, int level, int internalformat,
                                      int width, int height, int border,
                                      int imageSize, double data) {
    (void)target; (void)level; (void)internalformat;
    (void)width; (void)height; (void)border; (void)imageSize; (void)data;
}

void lime_gl_compressed_tex_image_3d (int target, int level, int internalformat,
                                      int width, int height, int depth, int border,
                                      int imageSize, double data) {
    (void)target; (void)level; (void)internalformat;
    (void)width; (void)height; (void)depth; (void)border; (void)imageSize; (void)data;
}

void lime_gl_compressed_tex_sub_image_2d (int target, int level, int xoffset, int yoffset,
                                          int width, int height, int format,
                                          int imageSize, double data) {
    (void)target; (void)level; (void)xoffset; (void)yoffset;
    (void)width; (void)height; (void)format; (void)imageSize; (void)data;
}

void lime_gl_compressed_tex_sub_image_3d (int target, int level, int xoffset, int yoffset,
                                          int zoffset, int width, int height, int depth,
                                          int format, int imageSize, double data) {
    (void)target; (void)level; (void)xoffset; (void)yoffset; (void)zoffset;
    (void)width; (void)height; (void)depth; (void)format; (void)imageSize; (void)data;
}

void lime_gl_get_internalformativ (int target, int internalformat, int pname,
                                   int bufSize, double params) {
    (void)target; (void)internalformat;
    double* out = (double*)(uintptr_t)params;
    if (!out || bufSize <= 0) return;
    if (pname == K_NUM_SAMPLE_COUNTS || pname == K_SAMPLES) {
        out[0] = 1.0;   // 单采样
    }
}

} // namespace lime
