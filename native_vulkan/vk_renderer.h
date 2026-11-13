#pragma once
#include <cstdint>
#include <vector>
#include <string>

#include "vk_device_caps.h"

// 平台边界：本头不得出现任何平台类型或平台 API 符号（含 win32 头名与窗口句柄类型）。
// 原生窗口句柄一律用 void* 传递；平台差异全部在 vk_platform_*.cpp 里处理。

// Minimal Vulkan renderer extracted from the verified demo.
// Capabilities proven: swapchain, DEVICE_LOCAL textures via staging,
// N-sprite batching, per-texture descriptor sets, UBO, GLSL->SPIR-V shaders.

// packet 执行路径（VK_PKT_EXEC=1）用的状态位。PktDraw 按这些位选管线变体，
// 缺的变体才新建；全部"默认"时直接复用 Init 建好的那条管线（兜底，不改画面）。
struct PktPipelineState {
    uint8_t blendEnabled;
    uint8_t blendSrc;      // 归一化混合因子，见 PktBlendFactor；0xFF = 未指定（默认 SRC_ALPHA）
    uint8_t blendDst;      // 同上；0xFF = 未指定（默认 ONE_MINUS_SRC_ALPHA）
    uint8_t depthTest;
    uint8_t depthWrite;
    uint8_t stencilTest;
    uint8_t cullMode;      // 0=none 1=back 2=front 3=both
    uint8_t frontFace;     // 0=CCW 1=CW
    uint8_t scissorTest;
    uint8_t colorMask;
};

// 归一化混合因子：把 GL 的几十种枚举收敛到这几种，管线的混合因子只有这几种会被用到。
enum PktBlendFactor {
    PKT_BLEND_UNSET = 0xFF,
    PKT_BLEND_ONE = 0,
    PKT_BLEND_ZERO = 1,
    PKT_BLEND_SRC_ALPHA = 2,
    PKT_BLEND_ONE_MINUS_SRC_ALPHA = 3,
    PKT_BLEND_DST_ALPHA = 4,
    PKT_BLEND_ONE_MINUS_DST_ALPHA = 5,
    PKT_BLEND_SRC_COLOR = 6,
    PKT_BLEND_ONE_MINUS_SRC_COLOR = 7,
    PKT_BLEND_DST_COLOR = 8,
    PKT_BLEND_ONE_MINUS_DST_COLOR = 9,
    PKT_BLEND_CONST_ALPHA = 10,
    PKT_BLEND_ONE_MINUS_CONST_ALPHA = 11,
    PKT_BLEND_COUNT = 12
};

// 归一化采样器参数：0xFF = 未指定（沿用该纹理现有的 descriptor set，即旧行为）
struct PktSamplerState {
    uint8_t minFilter;   // 0=nearest 1=linear 2=nearest_mip_nearest 3=linear_mip_nearest 4=nearest_mip_linear 5=linear_mip_linear
    uint8_t magFilter;   // 0=nearest 1=linear
    uint8_t wrapU;       // 0=clamp 1=repeat 2=mirror
    uint8_t wrapV;
};

// 一笔 draw 的 viewport / scissor（GL 口径：原点左下）。valid=0 时沿用 pass 打开时下发的值。
struct PktRect {
    int16_t x, y, w, h;
    uint8_t valid;
};

// 一笔 draw 的完整描述。顶点布局与批处理路径完全相同（15 floats/顶点：
// pos4, 逐顶点 alpha, colorMultiplier4, colorOffset4, uv2），所以能直接复用
// 同一条管线与同一份 UBO 投影。
struct PktDrawDesc {
    uint32_t texId;          // VkRenderer 纹理 id（>=1000 为离屏目标）
    const float* verts;      // vertexCount * 15 floats
    int vertexCount;
    const uint16_t* indices; // NULL = 非索引；32 位索引时用 indices32
    const uint32_t* indices32;
    int indexCount;
    uint8_t index32;         // 1 = 用 indices32 上传 32 位索引
    PktPipelineState state;
    PktSamplerState sampler;
    PktRect viewport;
    PktRect scissor;
    // 游戏 program + 该笔的完整常量块。两者都有时优先走 program 自己的 Vulkan 管线
    // （滤镜 NTSC/Bloom/TVStatic 只有这条路径会真正执行）；否则回退固定管线。
    int program;
    const uint8_t* uniform;
    uint32_t uniformSize;
};

class VkRenderer {
public:
    VkRenderer();
    ~VkRenderer();

    // nativeWindow：原生窗口句柄，各平台都按 void* 传入，本层不区分平台。
    bool Init(void* nativeWindow, int width, int height);
    void Shutdown();

    // 只有 Init 全成功后 ready 才为 true；Shutdown/失败后为 false。
    bool IsReady() const;
    // 设备能力快照；IsReady()==false 时内容不可用于能力声明。
    const VkDeviceCaps& Caps() const;

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
    // vertexMult / vertexOff 非 NULL 时按顶点逐点取 colorMultiplier / colorOffset
    // （各 4 float；lime 把这两组放在 loc6/loc7 顶点数组里），NULL 用常量值
    // uniformData/uniformSize：调用时刻该 program 的 uniform 快照（gl_shader_uniform_block），
    // 批处理提交时会按这份快照上传，避免同一个 program 一帧多次绘制时互相覆盖。
    void DrawRaw(uint32_t texId, const float* verts, int vertexCount,
                 const float* vertexAlpha = nullptr,
                 const float* vertexMult = nullptr,
                 const float* vertexOff = nullptr,
                 const uint8_t* uniformData = nullptr, uint32_t uniformSize = 0);

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
    // 批处理路径：游戏对本目标发过 glClear（颜色 rgba）。EndFrame 时改用 CLEAR pass 清屏。
    void RequestClear(uint32_t id, float r, float g, float b, float a);

    // ---- packet 执行路径（只给 VK_PKT_EXEC=1 用；与批处理路径互斥）----
    // 进目标：开 render pass、绑 framebuffer、按当前 viewport/scissor 下发。
    // 在 BindRenderTarget 的时机被调用（不是帧末）；同一目标重复调用只更新清屏色。
    // colorClear=true 表示这一笔是明确的 clear 包（且 clearMask 含颜色位）：
    // 离屏目标（>=1000）会真的把整幅清成 (r,g,b,a)；普通绑定保持 LOAD 语义。
    bool PktBeginTarget(uint32_t targetId, float r, float g, float b, float a,
                        bool colorClear = false, uint8_t clearMask = 0);
    // 取证：该离屏目标在"本帧"是否真的执行过 clear（配合 trace 的 canvas3x 行）。
    bool PktTargetClearedThisFrame(uint32_t targetId);
    // 一笔 draw：绑管线 -> 绑 descriptor（按纹理槽）-> 绑顶点/索引 -> vkCmdDraw*，
    // 命令立刻写进当前命令缓冲（不是每笔一次 vkQueueSubmit）。
    bool PktDraw(const PktDrawDesc& desc);
    // 出目标：关 pass、结束命令缓冲并提交一次；target=0 且请求了真实 flip 时 present。
    void PktEndTarget();
    // 当前是否有一个 packet pass 打开
    bool PktTargetOpen() const;
    // 用 packet 路径把离屏目标整幅合成到当前（后缓冲）pass
    bool PktComposite(uint32_t srcTargetId);

    const char* LastError() const;

private:
    struct Impl;
    Impl* impl;
    // 真正释放纹理槽位；只在 EndFrame 提交完成后调用（延迟销毁，避免槽位 id 复用串图）
    void DestroyTextureNow(uint32_t id);
    // 帧环：选当前槽位并等它的 fence（保证命令缓冲/顶点区/UBO 区可复用），
    // 顺带真正释放该槽延迟销毁的纹理
    void AcquireSlot();
    // 等当前槽位已提交的命令缓冲执行完（present/读回前必须调用）
    void WaitSlotFence();
    // 交换链过期（present/acquire 返回 OUT_OF_DATE/SUBOPTIMAL）后的重建
    bool RecreateSwapchain();
    // 当前窗口客户区尺寸是否等于交换链 extent（resize 动画期间用来决定要不要立即重建）
    bool WindowSizeMatchesExtent();
    // 每帧记录窗口尺寸变化（独立于 present 是否成功），返回 true 表示本帧尺寸变了
    bool ObserveWindowSize();
    // 是否处于窗口缩放动画中（最近 400ms 内尺寸变过）；动画期间禁止重建交换链
    bool InResizeAnimation();
    // 真正上传的实现；CreateTexture 负责大图降级后再转调它
    uint32_t CreateTextureOnce(int w, int h, const unsigned char* rgba);
    // packet 路径的动态顶点/索引缓冲（首次使用时懒创建）
    bool PktEnsureBuffers();
    // 按状态取管线变体：默认状态直接返回 Init 建的那条，缺的才新建并缓存
    // 返回/入参里的 Vulkan 句柄一律走 void*（本头不放 Vulkan 类型）
    void* PktPipelineFor(const PktPipelineState& st);
    void* PktSetFor(uint32_t texId, const PktSamplerState& ss);
    void* PktSamplerFor(const PktSamplerState& ss);
    // 纹理/目标销毁时丢掉指向它的变体 descriptor set（否则留下悬空引用）
    void PktDropSets(uint32_t texId);
    // ---- 取证转储（默认关）：VK_DUMP_TEX=<dir> + VK_DUMP_FRAME=<帧号> ----
    // 记下本帧被采样的纹理/目标 id（去重）；真正的读回在 MaybeDumpTextures 里做
    void NoteSampled(uint32_t texId);
    // 帧号命中（gl_packet 的 frameGen 或批处理帧号）才转储一次，返回是否真的写了文件
    bool MaybeDumpTextures(uint64_t frameHint);
};

