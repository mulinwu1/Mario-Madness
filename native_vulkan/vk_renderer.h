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

    // raw vertex submission (GL-style): verts are x,y,u,v float4 per vertex
    void DrawRaw(uint32_t texId, const float* verts, int vertexCount);

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
};
