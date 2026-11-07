#pragma once
#include <cstdint>
#include <vector>
#include <string>

// 平台边界：本头不得出现任何平台类型或平台 API 符号（含 win32 头名与窗口句柄类型）。
// 原生窗口句柄一律用 void* 传递；平台差异全部在 vk_platform_*.cpp 里处理。

// Minimal Vulkan renderer extracted from the verified demo.
// Capabilities proven: swapchain, DEVICE_LOCAL textures via staging,
// N-sprite batching, per-texture descriptor sets, UBO, GLSL->SPIR-V shaders.

class VkRenderer {
public:
    VkRenderer();
    ~VkRenderer();

    // nativeWindow：原生窗口句柄，各平台都按 void* 传入，本层不区分平台。
    bool Init(void* nativeWindow, int width, int height);
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
    // 最后一次真正收到顶点（vertexCount>0）并已提交的离屏目标（0 = 没有）
    uint32_t LastGeometryTarget() const;
    // present 收尾时复位，避免跨帧取到上一帧的目标
    void ResetLastGeometryTarget();
    // 当前提交帧号，用于把截图与日志行对齐
    uint64_t FrameIndex() const;
    // 本帧后缓冲最后一次采样的纹理 id（0 = 没采样过）
    uint32_t BackbufferLastSample() const;

    // raw vertex submission (GL-style): verts are x,y,u,v float4 per vertex
    // vertexAlpha 非 NULL 时按顶点逐点取 alpha（lime 把 per-vertex alpha 放在 loc5
    // 顶点数组里，黑幕的 0.05 就在这条数组里）；NULL 用 SetDrawColor 的常量值
    void DrawRaw(uint32_t texId, const float* verts, int vertexCount, const float* vertexAlpha = nullptr);

    // 设置本次绘制的常量顶点属性（GL glVertexAttrib1f/4f 的语义）：
    // alpha 对应 loc1 openfl_Alpha，mult 对应 loc2 colorMultiplier，offset 对应 loc3 colorOffset。
    // 传 NULL 恢复默认（mult=1,1,1,1，offset=0）。
    bool SetDrawColor(float alpha, const float* mult, const float* offset);

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
    // 标记"下一个后缓冲提交是真实 flip"：只在真正的 swap 路径调用
    void RequestPresent();

    const char* LastError() const;

private:
    struct Impl;
    Impl* impl;
    // 交换链过期（present/acquire 返回 OUT_OF_DATE/SUBOPTIMAL）后的重建
    bool RecreateSwapchain();
    // 真正上传的实现；CreateTexture 负责大图降级后再转调它
    uint32_t CreateTextureOnce(int w, int h, const unsigned char* rgba);
};
