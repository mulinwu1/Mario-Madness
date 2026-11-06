#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

// Minimal Vulkan renderer extracted from the verified demo.
// Capabilities proven: swapchain, DEVICE_LOCAL textures via staging,
// N-sprite batching, per-texture descriptor sets, UBO, GLSL->SPIR-V shaders.

class VkRenderer {
public:
    VkRenderer();
    ~VkRenderer();

    bool Init(HWND hwnd, int width, int height);
    void Shutdown();

    // texture: returns id, 0 on failure. Pixels are RGBA8, row-major.
    uint32_t CreateTexture(int w, int h, const unsigned char* rgba);
    void DestroyTexture(uint32_t id);
    uint32_t TextureCount() const;

    // frame
    void BeginFrame(float r, float g, float b, float a);
    void SetUniform(int index, float value);
    void DrawSprite(uint32_t texId, float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1);
    void EndFrame();

    // 交换链尺寸 / 离屏目标尺寸（present 时把最后一张离屏图铺满窗口要用）
    int ScreenWidth() const;
    int ScreenHeight() const;
    bool GetRenderTargetSize(uint32_t id, int* w, int* h) const;
    // 当前已打开帧里是否已经有待提交的几何（present 时判断要不要补合成）
    bool FrameHasGeometry() const;

    // raw vertex submission (GL-style): verts are x,y,u,v float4 per vertex
    void DrawRaw(uint32_t texId, const float* verts, int vertexCount);

    // ---- GL 翻译层需要的能力（gl_vk_contract.h 里的 GlVkRendererExt）----
    // per-texture sampler：texParameteri 真正生效的必要条件
    bool SetTextureSampler(uint32_t texId, int minFilter, int magFilter,
                           int wrapS, int wrapT, int baseLevel, int maxLevel,
                           float minLod, float maxLod);
    bool UpdateSubImage(uint32_t texId, int level, int x, int y, int w, int h,
                        const unsigned char* rgba);
    uint32_t CreateEmptyTexture(int w, int h);
    bool GenerateMipmaps(uint32_t texId, int maxLevel);
    uint32_t CreateRenderTargetEx(int w, int h, bool withDepth);
    bool ReadPixels(uint32_t targetId, int x, int y, int w, int h, unsigned char* rgba);

    // offscreen render target (FBO equivalent)
    // returns id usable with BindTarget, or 0 on failure. id can also be used as a texture.
    uint32_t CreateRenderTarget(int w, int h);
    void DestroyRenderTarget(uint32_t id);
    // bind a render target (0 = backbuffer). starts a new frame for that target.
    bool BindRenderTarget(uint32_t id, float r, float g, float b, float a);
    // finish drawing into the current target and present it if it is the backbuffer
    void PresentTarget();

    const char* LastError() const;

private:
    struct Impl;
    Impl* impl;
    // 真正上传的实现；CreateTexture 负责大图降级后再转调它
    uint32_t CreateTextureOnce(int w, int h, const unsigned char* rgba);
};
