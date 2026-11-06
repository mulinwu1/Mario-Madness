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

    const char* LastError() const;

private:
    struct Impl;
    Impl* impl;
};
