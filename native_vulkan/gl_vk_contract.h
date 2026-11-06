#pragma once

#include <cstdint>
#include <vector>

namespace lime {

// ============================ GLVK_STATE_CONTRACT =============================
// gl_core / gl_fbo / gl_texture_params 三块之间的唯一对接契约。
// 之前每个文件各复制一份结构体并各自定义安装函数，编译进同一个 ndll 会重复符号，
// 所以统一收到这里，只由 gl_vk_contract.cpp 定义一次。
struct GlVkCoreState {
    // GL 纹理名 -> VkRenderer 纹理 id（gl_core 里的 g_texVk[name]），0 表示尚未上传
    uint32_t (*getTextureId) (int glName);
    // 把 GL 纹理名指向别的 Vk 资源；FBO 会把它指向离屏 RenderTarget id（>=1000）
    // 传 0 表示解除别名
    void (*setTextureId) (int glName, uint32_t vkId);
    // 当前 active texture unit 上绑定的纹理名（g_texBound[g_activeUnit]）
    int (*getBoundTexture) ();
    // texImage2D 时记录的纹理尺寸；未分配返回 false
    bool (*getTextureSize) (int glName, int* width, int* height);
    // 最近一次 glClearColor 的颜色
    void (*getClearColor) (float* rgba);
};

// VkRenderer 现有能力缺口。主线补齐 VkRenderer 接口后在 gl_vk_init 里注册即可生效；
// 任一函数为空指针时，各模块只记录 GL 状态，不崩溃、不谎报成功。
struct GlVkRendererExt {
    bool (*setTextureSampler) (uint32_t texId, int minFilter, int magFilter,
                               int wrapS, int wrapT, int baseLevel, int maxLevel,
                               float minLod, float maxLod);
    bool (*updateSubImage) (uint32_t texId, int level, int x, int y, int w, int h,
                            const unsigned char* rgba);
    uint32_t (*createEmptyTexture) (int w, int h);
    bool (*generateMipmaps) (uint32_t texId, int maxLevel);
    uint32_t (*createRenderTargetEx) (int w, int h, bool withDepth);
    bool (*readPixels) (uint32_t targetId, int x, int y, int w, int h,
                        unsigned char* rgba);
};

void gl_vk_install_core_state (const GlVkCoreState* state);
const GlVkCoreState* gl_vk_core_state ();
void gl_vk_install_renderer_ext (const GlVkRendererExt* ext);
const GlVkRendererExt* gl_vk_renderer_ext ();
// ============================ GLVK_STATE_CONTRACT END =========================

} // namespace lime
