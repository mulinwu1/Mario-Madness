#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

// GLES 状态缓存 + Vulkan 管线键
//
// 定位：只做"数据建模"，不调用任何 Vulkan 函数，也不碰 lime_gl_* 符号。
//   - 所有入口按 GLES 规范记录状态、校验参数、按 GL 语义记录错误码；
//   - 影响管线创建的状态汇总成 PipelineKey，作为 pipeline cache 的键（生成或复用）；
//   - 每帧可变的状态（scissor / viewport / blend 常量 / stencil ref / depthBias 已入 key）
//     提供现成的 Vk 结构，由调用方自己 vkCmdSet* 应用。
//
// 线程模型：和 GL 一样按"单渲染线程"使用，未加锁。
// 约定：状态初值就是 GLES 默认值，即使调用方从未调用 Reset() 也是正确的。

namespace lime {
namespace glstate {

// 渲染目标的附件格式。Vulkan 管线必须与 render pass 的格式兼容，
// 所以格式也属于管线键的一部分。
struct Formats {
    int colorFormat = (int)VK_FORMAT_UNDEFINED;    // VkFormat
    int depthFormat = (int)VK_FORMAT_UNDEFINED;    // VkFormat，VK_FORMAT_UNDEFINED 表示无深度附件
    int stencilFormat = (int)VK_FORMAT_UNDEFINED;  // VkFormat，通常与 depthFormat 合成 D24S8
    int samples = 1;                              // 采样数，1 表示不开 MSAA

    bool operator==(const Formats& o) const {
        return colorFormat == o.colorFormat && depthFormat == o.depthFormat &&
               stencilFormat == o.stencilFormat && samples == o.samples;
    }
    bool operator!=(const Formats& o) const { return !(*this == o); }
};

// 单面模板状态（Vulkan 数值）。ref 不在这里：本阶段强制前后两面同值（见 GetStencilRef），
// 且 ref 在 Vulkan 里是动态状态，放进 key 会让每个引用值都新建一条管线。
struct StencilFace {
    int compareOp = (int)VK_COMPARE_OP_ALWAYS;      // VkCompareOp
    int failOp = (int)VK_STENCIL_OP_KEEP;           // VkStencilOp
    int passDepthFailOp = (int)VK_STENCIL_OP_KEEP;  // VkStencilOp
    int passOp = (int)VK_STENCIL_OP_KEEP;           // VkStencilOp
    uint32_t compareMask = 0xFFFFFFFFu;
    uint32_t writeMask = 0xFFFFFFFFu;

    bool operator==(const StencilFace& o) const {
        return compareOp == o.compareOp && failOp == o.failOp &&
               passDepthFailOp == o.passDepthFailOp && passOp == o.passOp &&
               compareMask == o.compareMask && writeMask == o.writeMask;
    }
};

// 一条图形管线所需的全部状态（Vulkan 数值，已做过 GL→VK 翻译）。
// 调用方用 Hash() 做 cache 键，命中则复用，未命中再建管线。
struct PipelineKey {
    // 输入装配
    int topology = (int)VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // VkPrimitiveTopology
    bool primitiveRestart = false;

    // 光栅化
    int cullMode = (int)VK_CULL_MODE_NONE;              // VkCullModeFlags
    int frontFace = (int)VK_FRONT_FACE_COUNTER_CLOCKWISE;  // VkFrontFace（已按 Y 翻转修正）
    bool depthBiasEnable = false;                        // GL_POLYGON_OFFSET_FILL
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    bool rasterizerDiscard = false;                      // GL_RASTERIZER_DISCARD
    float lineWidth = 1.0f;                              // 需要 wideLines 特性才生效

    // 深度
    bool depthTestEnable = false;
    bool depthWriteEnable = false;
    int depthCompareOp = (int)VK_COMPARE_OP_LESS;        // VkCompareOp

    // 模板
    bool stencilTestEnable = false;
    StencilFace frontStencil;
    StencilFace backStencil;

    // 混合（GL 的 RGB/alpha 通道各自独立的因子与方程）
    // 注意：只建模单个颜色附件（attachment0）；MRT 的逐附件混合/掩码不在本阶段范围内。
    bool blendEnable = false;
    int srcColorBlendFactor = (int)VK_BLEND_FACTOR_ONE;   // VkBlendFactor
    int dstColorBlendFactor = (int)VK_BLEND_FACTOR_ZERO;
    int colorBlendOp = (int)VK_BLEND_OP_ADD;              // VkBlendOp
    int srcAlphaBlendFactor = (int)VK_BLEND_FACTOR_ONE;
    int dstAlphaBlendFactor = (int)VK_BLEND_FACTOR_ZERO;
    int alphaBlendOp = (int)VK_BLEND_OP_ADD;
    uint32_t colorWriteMask = 0xF;                       // GL_COLOR_WRITEMASK（仅 attachment0）

    // 附件格式（管线兼容性）
    Formats formats;

    bool operator==(const PipelineKey& o) const;
    bool operator!=(const PipelineKey& o) const { return !(*this == o); }
    uint64_t Hash() const;
};

// ---------------- 初始化 / 尺寸 ----------------

// 恢复所有 GLES 默认值；viewport/scissor 归零，面尺寸记 0。
void Reset();

// 恢复默认值，同时把 viewport/scissor 设为整窗（surfaceWidth/surfaceHeight）。
// 上下文创建后调用一次最省事。
void ResetToDefaults(int surfaceWidth, int surfaceHeight);

// 交换链重建/窗口缩放时调用。只有 viewport/scissor 仍等于"上一次的整窗值"时才跟随更新，
// 不会覆盖游戏自己设过的视图矩形。
void ResizeSurface(int surfaceWidth, int surfaceHeight);

// ---------------- 能力开关（GL_ENABLE_BIT） ----------------

// 未知 cap 按规范记 GL_INVALID_ENUM 且不改状态，返回 false。
bool SetCapability(int cap, bool enabled);
// glIsEnabled 语义：未知 cap 记 GL_INVALID_ENUM 并返回 false。
bool IsEnabled(int cap);
// 是否为已知的（可 enable/disable 的）cap。
bool IsCapability(int cap);

// ---------------- 混合 ----------------

bool BlendFunc(int sfactor, int dfactor);
bool BlendFuncSeparate(int srcRGB, int dstRGB, int srcAlpha, int dstAlpha);
bool BlendEquation(int mode);
bool BlendEquationSeparate(int rgb, int alpha);
void BlendColor(float r, float g, float b, float a);

// OpenFL/Flixel 走预乘 alpha，正确的混合是 ONE / ONE_MINUS_SRC_ALPHA。
// 等价于 enable(BLEND) + BlendFuncSeparate(ONE, ONE_MINUS_SRC_ALPHA, ONE, ONE_MINUS_SRC_ALPHA)。
// 供主线在"游戏没有显式设置混合"的时候兜底。
void EnablePremultipliedAlphaBlend();

// ---------------- 深度 ----------------

bool DepthFunc(int func);
void DepthMask(bool flag);
// zNear/zFar 按规范 clamp 到 [0,1]；zNear > zFar 记 GL_INVALID_VALUE 且不改状态。
bool DepthRangef(float zNear, float zFar);

// ---------------- 模板 ----------------

// face 取 GL_FRONT / GL_BACK / GL_FRONT_AND_BACK。
// 注意：本阶段前后两面的 ref 强制同值——传进来的 ref 会同时写到两面，
// 这样提交时一次 vkCmdSetStencilReference(VK_STENCIL_FACE_FRONT_AND_BACK, ...) 就够，
// 不需要为不同的前后 ref 建管线变体。func 与 mask 仍按面独立。
bool StencilFunc(int func, int ref, int mask);
bool StencilFuncSeparate(int face, int func, int ref, int mask);
void StencilMask(int mask);
void StencilMaskSeparate(int face, int mask);
bool StencilOp(int sfail, int dpfail, int dppass);
bool StencilOpSeparate(int face, int sfail, int dpfail, int dppass);

// ---------------- 光栅化 ----------------

bool CullFace(int mode);
bool FrontFace(int mode);
void PolygonOffset(float factor, float units);
// width <= 0 记 GL_INVALID_VALUE。
bool LineWidth(float width);
void ColorMask(bool red, bool green, bool blue, bool alpha);
// value clamp 到 [0,1]。
void SampleCoverage(float value, bool invert);

// ---------------- 视口 / 剪裁 / 清除值 ----------------

bool Viewport(int x, int y, int width, int height);
bool Scissor(int x, int y, int width, int height);
void ClearColor(float red, float green, float blue, float alpha);
void ClearDepthf(float depth);
void ClearStencil(int stencil);

// ---------------- 绘制拓扑（进入管线键） ----------------

// GL 没有"持久拓扑"这个概念，但管线里 topology 是创建期状态。
// 主线在 draw 前调一次本函数即可；复位默认 GL_TRIANGLES。
bool SetDrawMode(int glMode);
// GL_TRIANGLE_FAN / GL_LINE_LOOP 在 Vulkan 里没有对应拓扑，需要调用方自行拆成列表。
bool DrawModeNeedsEmulation(int glMode);

// ---------------- 查询（glGet*） ----------------

// mainline 只应把本模块认识的 pname 路由进来；先用 HandlesPname 判断，
// 其它 pname（MAX_TEXTURE_SIZE 之类）继续走原有路径。
bool HandlesPname(int pname);
// 多分量 pname 的标量版本按 GL 语义只回第一个分量。
bool QueryBoolean(int pname, bool* out);
// 返回写入的 int 个数，0 表示不支持（同时记 GL_INVALID_ENUM）。
int QueryInteger(int pname, int* out, int maxCount);
// 返回写入的 float 个数，0 表示不支持（同时记 GL_INVALID_ENUM）。
int QueryFloat(int pname, float* out, int maxCount);

void GetClearColor(float out[4]);
float GetClearDepth();
int GetClearStencil();
void GetBlendColor(float out[4]);
void GetScissorBox(int out[4]);
void GetViewportBox(int out[4]);
float GetLineWidth();
// 前后强制同值，face 参数保留只为接口对齐；提交时用 FRONT_AND_BACK。
int GetStencilRef(int face);
bool GetColorMask(int* out4);
bool GetDepthRangef(float out2[2]);

// ---------------- Vulkan 动态状态 ----------------

// GL 的 y 轴朝上、Vulkan 朝下：返回的 scissor 已翻到左上原点，并 clamp 到目标尺寸。
// scissor test 关闭时返回整目标矩形（Vulkan 没有"关闭 scissor"这个管线状态）。
VkRect2D GetVulkanScissor(int targetWidth, int targetHeight);
// 同样做 y 翻转；depth 固定 [0,1]。
VkViewport GetVulkanViewport(int targetHeight);

// vk_renderer 当前在 DrawRaw 里做了 y 取反（py -> 1 - py*2/h），
// 这会让三角形绕序整体反转，所以默认要把 GL 的 frontFace 取反后再写进管线。
// 若主线改成负高度 viewport 而不在顶点上翻转，把这里设成 false。（默认 true，保持取反）
void SetClipSpaceYFlip(bool flip);
bool GetClipSpaceYFlip();

// ---------------- Vulkan 合法性钳制 / 警告 ----------------

// Vulkan 不接受的 GL 混合因子组合会被钳制成等价或最接近的合法值，
// 并按"同一组合只警告一次"回调；不会改动 GL 查询状态，也不记 GL 错误。
typedef void (*WarningCallback)(const char* message);
void SetWarningCallback(WarningCallback callback);  // 传 nullptr 恢复默认（stderr）

// dual-source blending（GL_SRC1_* 因子）默认关闭（本阶段按 ES3.0 对待）。
// 关闭时 SRC1_* 会被钳制成对应的 SRC_* 并警告一次；打开则需要管线/着色器侧支持。
void SetDualSourceBlendEnabled(bool enabled);

// GL 绘制模式 → VkPrimitiveTopology。needsEmulation 可为 nullptr，
// 返回 true 表示没有等价拓扑（fan / line loop），拓扑值给的是可用的近似。
int VkTopology(int glMode, bool* needsEmulation);

// ---------------- 管线键 ----------------

// 按当前状态生成键（不做缓存）。
PipelineKey CurrentPipelineKey(const Formats& formats);
// 带缓存版本：状态版本号与 formats 都没变时直接返回上次算好的键。
// 适合每帧/每批次调用，返回引用，生命周期直到下次状态变化。
const PipelineKey& CachedPipelineKey(const Formats& formats);
// 任一影响管线的状态变化都会 +1；不随 scissor/viewport/blendColor/stencilRef 变化。
uint64_t Version();

// 把键填进 Vulkan 创建结构体；调用方只需再补 stage / layout / renderPass。
void FillInputAssembly(const PipelineKey& key, VkPipelineInputAssemblyStateCreateInfo* out);
void FillRasterization(const PipelineKey& key, VkPipelineRasterizationStateCreateInfo* out);
void FillDepthStencil(const PipelineKey& key, VkPipelineDepthStencilStateCreateInfo* out);
void FillColorBlendAttachment(const PipelineKey& key, VkPipelineColorBlendAttachmentState* out);
void FillMultisample(const PipelineKey& key, VkPipelineMultisampleStateCreateInfo* out);

// ---------------- 错误码（供 glGetError 使用） ----------------

// 与 GL 一致：已有错误时新错误被丢弃，直到被 TakeError 取走。
void SetError(int glError);
int PeekError();
int TakeError();

} // namespace glstate
} // namespace lime
