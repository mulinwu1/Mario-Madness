#include "gl_state.h"

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace lime {
namespace glstate {
namespace {

// ---------------- GL 常量 ----------------
// 这里故意不加 GL_ 前缀：lime 自己的 GL 头是宏定义，混用会撞名。
enum : int {
    // 能力开关
    kBlend = 0x0BE2,
    kCullFace = 0x0B44,
    kDepthTest = 0x0B71,
    kDither = 0x0BD0,
    kPolygonOffsetFill = 0x8037,
    kPrimitiveRestartFixedIndex = 0x8D69,
    kRasterizerDiscard = 0x8C89,
    kSampleAlphaToCoverage = 0x809E,
    kSampleCoverageCap = 0x80A0,
    kScissorTest = 0x0C11,
    kStencilTest = 0x0B90,
    kTexture2D = 0x0DE1,
    kFramebufferSrgb = 0x8DB9,
    kSampleShading = 0x8C76,
    kSampleMask = 0x8E51,
    kDebugOutput = 0x92E0,
    kDebugOutputSynchronous = 0x8242,

    // 混合因子
    kZero = 0x0000,
    kOne = 0x0001,
    kSrcColor = 0x0300,
    kOneMinusSrcColor = 0x0301,
    kSrcAlpha = 0x0302,
    kOneMinusSrcAlpha = 0x0303,
    kDstAlpha = 0x0304,
    kOneMinusDstAlpha = 0x0305,
    kDstColor = 0x0306,
    kOneMinusDstColor = 0x0307,
    kSrcAlphaSaturate = 0x0308,
    kConstantColor = 0x8001,
    kOneMinusConstantColor = 0x8002,
    kConstantAlpha = 0x8003,
    kOneMinusConstantAlpha = 0x8004,
    kSrc1Color = 0x88F9,
    kOneMinusSrc1Color = 0x88FA,
    kSrc1Alpha = 0x8589,
    kOneMinusSrc1Alpha = 0x88FB,

    // 混合方程
    kFuncAdd = 0x8006,
    kFuncSubtract = 0x800A,
    kFuncReverseSubtract = 0x800B,
    kMin = 0x8007,
    kMax = 0x8008,

    // 比较函数（深度、模板共用）
    kNever = 0x0200,
    kLess = 0x0201,
    kEqual = 0x0202,
    kLequal = 0x0203,
    kGreater = 0x0204,
    kNotEqual = 0x0205,
    kGequal = 0x0206,
    kAlways = 0x0207,

    // 模板操作
    kKeep = 0x1E00,
    kReplace = 0x1E01,
    kIncr = 0x1E02,
    kDecr = 0x1E03,
    kInvert = 0x150A,
    kIncrWrap = 0x8507,
    kDecrWrap = 0x8508,

    // 面 / 光栅化
    kFront = 0x0404,
    kBack = 0x0405,
    kFrontAndBack = 0x0408,
    kCw = 0x0900,
    kCcw = 0x0901,

    // 绘制模式
    kPoints = 0x0000,
    kLines = 0x0001,
    kLineLoop = 0x0002,
    kLineStrip = 0x0003,
    kTriangles = 0x0004,
    kTriangleStrip = 0x0005,
    kTriangleFan = 0x0006,

    // 查询 pname
    kPnameBlendColor = 0x8005,
    kPnameBlendEquationRgb = 0x8009,
    kPnameBlendEquationAlpha = 0x883D,
    kPnameBlendSrcRgb = 0x80C9,
    kPnameBlendDstRgb = 0x80C8,
    kPnameBlendSrcAlpha = 0x80CB,
    kPnameBlendDstAlpha = 0x80CA,
    kPnameColorClearValue = 0x0C22,
    kPnameColorWritemask = 0x0C23,
    kPnameCullFaceMode = 0x0B45,
    kPnameFrontFace = 0x0B46,
    kPnameDepthClearValue = 0x0B73,
    kPnameDepthFunc = 0x0B74,
    kPnameDepthRange = 0x0B70,
    kPnameDepthWritemask = 0x0B72,
    kPnameLineWidth = 0x0B21,
    kPnamePolygonOffsetFactor = 0x8038,
    kPnamePolygonOffsetUnits = 0x2A00,
    kPnameSampleCoverageValue = 0x80AA,
    kPnameSampleCoverageInvert = 0x80AB,
    kPnameScissorBox = 0x0C10,
    kPnameStencilClearValue = 0x0B91,
    kPnameStencilFail = 0x0B94,
    kPnameStencilFunc = 0x0B92,
    kPnameStencilPassDepthFail = 0x0B95,
    kPnameStencilPassDepthPass = 0x0B96,
    kPnameStencilRef = 0x0B97,
    kPnameStencilValueMask = 0x0B93,
    kPnameStencilWritemask = 0x0B98,
    kPnameStencilBackFail = 0x8801,
    kPnameStencilBackFunc = 0x8800,
    kPnameStencilBackPassDepthFail = 0x8802,
    kPnameStencilBackPassDepthPass = 0x8803,
    kPnameStencilBackRef = 0x8CA3,
    kPnameStencilBackValueMask = 0x8CA4,
    kPnameStencilBackWritemask = 0x8CA5,
    kPnameViewport = 0x0BA2,

    // 错误
    kInvalidEnum = 0x0500,
    kInvalidValue = 0x0501,
    kInvalidOperation = 0x0502,
};

// 能力开关的位序号（不是 GL 数值）
enum CapSlot {
    CS_Blend = 0,
    CS_CullFace,
    CS_DepthTest,
    CS_Dither,
    CS_PolygonOffsetFill,
    CS_PrimitiveRestartFixedIndex,
    CS_RasterizerDiscard,
    CS_SampleAlphaToCoverage,
    CS_SampleCoverage,
    CS_ScissorTest,
    CS_StencilTest,
    CS_Texture2D,
    CS_FramebufferSrgb,
    CS_SampleShading,
    CS_SampleMask,
    CS_DebugOutput,
    CS_DebugOutputSynchronous,
    CS_Count
};

struct FaceState {
    int func = kAlways;
    unsigned valueMask = 0xFFFFFFFFu;
    unsigned writeMask = 0xFFFFFFFFu;
    int sfail = kKeep;
    int dpfail = kKeep;
    int dppass = kKeep;
};

// 全部默认值按 GLES 规范；成员初始化器保证"从未调用 Reset 也是对的"
struct State {
    uint32_t caps = 1u << CS_Dither;  // GL_DITHER 默认开启，其余默认关闭

    int blendSrcRgb = kOne;
    int blendDstRgb = kZero;
    int blendSrcAlpha = kOne;
    int blendDstAlpha = kZero;
    int blendEqRgb = kFuncAdd;
    int blendEqAlpha = kFuncAdd;
    float blendColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    bool colorMask[4] = {true, true, true, true};

    int cullMode = kBack;
    int frontFace = kCcw;

    int depthFunc = kLess;
    bool depthMask = true;
    float depthRange[2] = {0.0f, 1.0f};

    // 前后两面的 func/mask/op 各自独立，但 ref 按本阶段约定强制同值
    FaceState frontStencil;
    FaceState backStencil;
    int stencilRef = 0;

    int scissorBox[4] = {0, 0, 0, 0};
    int viewportBox[4] = {0, 0, 0, 0};

    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float clearDepth = 1.0f;
    int clearStencil = 0;

    float lineWidth = 1.0f;
    float polygonOffsetFactor = 0.0f;
    float polygonOffsetUnits = 0.0f;

    float sampleCoverageValue = 1.0f;
    bool sampleCoverageInvert = false;

    int drawMode = kTriangles;

    int surfaceWidth = 0;
    int surfaceHeight = 0;
};

State g_state;
uint64_t g_version = 0;
int g_error = 0;
bool g_yFlip = true;  // 与 vk_renderer DrawRaw 的 y 取反一致

WarningCallback g_warnCallback = nullptr;  // nullptr 时写 stderr
bool g_dualSource = false;                 // 本阶段按 ES3.0 对待，默认无 dual-source
std::unordered_set<uint64_t> g_warnedBlendCombos;

PipelineKey g_cachedKey;
Formats g_cachedFormats;
uint64_t g_cachedVersion = (uint64_t)-1;

int CapSlotOf(int cap) {
    switch (cap) {
        case kBlend: return CS_Blend;
        case kCullFace: return CS_CullFace;
        case kDepthTest: return CS_DepthTest;
        case kDither: return CS_Dither;
        case kPolygonOffsetFill: return CS_PolygonOffsetFill;
        case kPrimitiveRestartFixedIndex: return CS_PrimitiveRestartFixedIndex;
        case kRasterizerDiscard: return CS_RasterizerDiscard;
        case kSampleAlphaToCoverage: return CS_SampleAlphaToCoverage;
        case kSampleCoverageCap: return CS_SampleCoverage;
        case kScissorTest: return CS_ScissorTest;
        case kStencilTest: return CS_StencilTest;
        case kTexture2D: return CS_Texture2D;
        case kFramebufferSrgb: return CS_FramebufferSrgb;
        case kSampleShading: return CS_SampleShading;
        case kSampleMask: return CS_SampleMask;
        case kDebugOutput: return CS_DebugOutput;
        case kDebugOutputSynchronous: return CS_DebugOutputSynchronous;
        default: return -1;
    }
}

bool IsEnabledSlot(int slot) {
    if (slot < 0 || slot >= CS_Count) return false;
    return (g_state.caps & (1u << slot)) != 0;
}

bool SetEnabledSlot(int slot, bool enabled) {
    uint32_t bit = 1u << slot;
    uint32_t old = g_state.caps;
    if (enabled) g_state.caps |= bit;
    else g_state.caps &= ~bit;
    return g_state.caps != old;
}

bool IsBlendEquation(int e) {
    switch (e) {
        case kFuncAdd:
        case kFuncSubtract:
        case kFuncReverseSubtract:
        case kMin:
        case kMax:
            return true;
        default:
            return false;
    }
}

bool IsCompareFunc(int f) {
    return f >= kNever && f <= kAlways;
}

// forSrc 区分源/目标：GL_SRC_ALPHA_SATURATE 只能给源用
bool IsBlendFactor(int f, bool forSource) {
    switch (f) {
        case kZero:
        case kOne:
        case kSrcColor:
        case kOneMinusSrcColor:
        case kSrcAlpha:
        case kOneMinusSrcAlpha:
        case kDstAlpha:
        case kOneMinusDstAlpha:
        case kDstColor:
        case kOneMinusDstColor:
        case kConstantColor:
        case kOneMinusConstantColor:
        case kConstantAlpha:
        case kOneMinusConstantAlpha:
        case kSrc1Color:
        case kOneMinusSrc1Color:
        case kSrc1Alpha:
        case kOneMinusSrc1Alpha:
            return true;
        case kSrcAlphaSaturate:
            return forSource;
        default:
            return false;
    }
}

bool IsStencilOp(int op) {
    switch (op) {
        case kKeep:
        case kZero:
        case kReplace:
        case kIncr:
        case kDecr:
        case kInvert:
        case kIncrWrap:
        case kDecrWrap:
            return true;
        default:
            return false;
    }
}

bool IsFace(int face) {
    return face == kFront || face == kBack || face == kFrontAndBack;
}

// 时序安全的浮点比较：只有真正变了才递增版本号，避免管线缓存被无意义地打穿
bool FloatChanged(float a, float b) {
    return memcmp(&a, &b, sizeof(float)) != 0;
}

// ---------------- GL → Vulkan 翻译 ----------------

VkBlendFactor ToVkBlendFactor(int gl) {
    switch (gl) {
        case kZero: return VK_BLEND_FACTOR_ZERO;
        case kOne: return VK_BLEND_FACTOR_ONE;
        case kSrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
        case kOneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case kSrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case kOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case kDstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case kOneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case kDstColor: return VK_BLEND_FACTOR_DST_COLOR;
        case kOneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case kSrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case kConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case kOneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case kConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case kOneMinusConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case kSrc1Color: return VK_BLEND_FACTOR_SRC1_COLOR;
        case kOneMinusSrc1Color: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case kSrc1Alpha: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case kOneMinusSrc1Alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp ToVkBlendOp(int gl) {
    switch (gl) {
        case kFuncAdd: return VK_BLEND_OP_ADD;
        case kFuncSubtract: return VK_BLEND_OP_SUBTRACT;
        case kFuncReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case kMin: return VK_BLEND_OP_MIN;
        case kMax: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp ToVkCompareOp(int gl) {
    switch (gl) {
        case kNever: return VK_COMPARE_OP_NEVER;
        case kLess: return VK_COMPARE_OP_LESS;
        case kEqual: return VK_COMPARE_OP_EQUAL;
        case kLequal: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case kGreater: return VK_COMPARE_OP_GREATER;
        case kNotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case kGequal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case kAlways: return VK_COMPARE_OP_ALWAYS;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp ToVkStencilOp(int gl) {
    switch (gl) {
        case kZero: return VK_STENCIL_OP_ZERO;
        case kKeep: return VK_STENCIL_OP_KEEP;
        case kReplace: return VK_STENCIL_OP_REPLACE;
        case kIncr: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case kDecr: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case kInvert: return VK_STENCIL_OP_INVERT;
        case kIncrWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case kDecrWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkCullModeFlags ToVkCullMode(int gl) {
    switch (gl) {
        case kFront: return VK_CULL_MODE_FRONT_BIT;
        case kBack: return VK_CULL_MODE_BACK_BIT;
        case kFrontAndBack: return VK_CULL_MODE_FRONT_AND_BACK;
        default: return VK_CULL_MODE_NONE;
    }
}

// 顶点 y 取反会让绕序整体翻转，所以 frontFace 也要跟着翻
VkFrontFace ToVkFrontFace(int gl) {
    bool ccw = (gl == kCcw);
    if (g_yFlip) ccw = !ccw;
    return ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
}

VkPrimitiveTopology ToVkTopology(int gl, bool* needsEmulation) {
    bool emu = false;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (gl) {
        case kPoints: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case kLines: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case kLineLoop: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; emu = true; break;
        case kLineStrip: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case kTriangles: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case kTriangleStrip: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case kTriangleFan: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; emu = true; break;
        default: emu = true; break;
    }
    if (needsEmulation) *needsEmulation = emu;
    return topo;
}

VkSampleCountFlagBits ToVkSamples(int samples) {
    switch (samples) {
        case 1: return VK_SAMPLE_COUNT_1_BIT;
        case 2: return VK_SAMPLE_COUNT_2_BIT;
        case 4: return VK_SAMPLE_COUNT_4_BIT;
        case 8: return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        case 32: return VK_SAMPLE_COUNT_32_BIT;
        case 64: return VK_SAMPLE_COUNT_64_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

// 把 GL 面的比较函数/写掩码/操作翻译成键里的单面结构
StencilFace MakeFace(const FaceState& f) {
    StencilFace out;
    out.compareOp = (int)ToVkCompareOp(f.func);
    out.failOp = (int)ToVkStencilOp(f.sfail);
    out.passDepthFailOp = (int)ToVkStencilOp(f.dpfail);
    out.passOp = (int)ToVkStencilOp(f.dppass);
    out.compareMask = f.valueMask;
    out.writeMask = f.writeMask;
    return out;
}

// GL 忽略 MIN/MAX 的混合因子，但 Vulkan 要求两者必须是 ONE
void NormalizeBlendForOp(VkBlendOp op, int& src, int& dst) {
    if (op == VK_BLEND_OP_MIN || op == VK_BLEND_OP_MAX) {
        src = (int)VK_BLEND_FACTOR_ONE;
        dst = (int)VK_BLEND_FACTOR_ONE;
    }
}

// ---------------- Vulkan 非法混合组合的钳制 ----------------

// alpha 通道的因子不能引用颜色，映射到对应的 alpha 因子（语义上最接近）
int ClampAlphaFactor(int f) {
    switch (f) {
        case kSrcColor: return kSrcAlpha;
        case kOneMinusSrcColor: return kOneMinusSrcAlpha;
        case kDstColor: return kDstAlpha;
        case kOneMinusDstColor: return kOneMinusDstAlpha;
        case kSrc1Color: return kSrc1Alpha;
        case kOneMinusSrc1Color: return kOneMinusSrc1Alpha;
        default: return f;
    }
}

// SRC1_*（dual-source）降级到对应的 SRC_*
int ClampDualSourceFactor(int f) {
    switch (f) {
        case kSrc1Color: return kSrcColor;
        case kOneMinusSrc1Color: return kOneMinusSrcColor;
        case kSrc1Alpha: return kSrcAlpha;
        case kOneMinusSrc1Alpha: return kOneMinusSrcAlpha;
        default: return f;
    }
}

uint64_t BlendComboSignature(int sc, int dc, int sa, int da) {
    return ((uint64_t)(sc & 0xFFFF) << 48) | ((uint64_t)(dc & 0xFFFF) << 32) |
           ((uint64_t)(sa & 0xFFFF) << 16) | (uint64_t)(da & 0xFFFF);
}

void WarnBlendClampedOnce(int sc, int dc, int sa, int da) {
    if (!g_warnedBlendCombos.insert(BlendComboSignature(sc, dc, sa, da)).second) return;
    char msg[224];
    snprintf(msg, sizeof(msg),
             "[gl_state] GL blend 组合 srcRGB=0x%04X dstRGB=0x%04X srcAlpha=0x%04X dstAlpha=0x%04X "
             "在 Vulkan 中不合法，已钳制为合法值",
             (unsigned)sc, (unsigned)dc, (unsigned)sa, (unsigned)da);
    if (g_warnCallback) g_warnCallback(msg);
    else fprintf(stderr, "%s\n", msg);
}

// 按 Vulkan 的硬限制修正因子；GL 查询状态保持原值，只影响送进管线的键
void SanitizeBlendFactors(int rawSc, int rawDc, int rawSa, int rawDa,
                          int& sc, int& dc, int& sa, int& da) {
    sc = rawSc;
    dc = rawDc;
    sa = ClampAlphaFactor(rawSa);
    da = ClampAlphaFactor(rawDa);
    // dstColor 不能用第二源因子（Vulkan 只允许源侧引用 SRC1）
    dc = ClampDualSourceFactor(dc);
    if (!g_dualSource) {
        sc = ClampDualSourceFactor(sc);
        sa = ClampDualSourceFactor(sa);
        da = ClampDualSourceFactor(da);
    }
    if (sc != rawSc || dc != rawDc || sa != rawSa || da != rawDa) {
        WarnBlendClampedOnce(rawSc, rawDc, rawSa, rawDa);
    }
}

// ---------------- 哈希 ----------------

uint64_t Fnv(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

uint64_t FloatBits(float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof(u));
    return (uint64_t)u;
}

uint64_t HashFace(uint64_t h, const StencilFace& f) {
    h = Fnv(h, (uint64_t)f.compareOp);
    h = Fnv(h, (uint64_t)f.failOp);
    h = Fnv(h, (uint64_t)f.passDepthFailOp);
    h = Fnv(h, (uint64_t)f.passOp);
    h = Fnv(h, (uint64_t)f.compareMask);
    h = Fnv(h, (uint64_t)f.writeMask);
    return h;
}

// 键里所有 pname 的清单（供 HandlesPname 判定）
const int kKnownPnames[] = {
    // 能力开关
    kBlend, kCullFace, kDepthTest, kDither, kPolygonOffsetFill,
    kPrimitiveRestartFixedIndex, kRasterizerDiscard, kSampleAlphaToCoverage,
    kSampleCoverageCap, kScissorTest, kStencilTest, kTexture2D,
    kFramebufferSrgb, kSampleShading, kSampleMask, kDebugOutput,
    kDebugOutputSynchronous,
    // 整型状态
    kPnameBlendEquationRgb, kPnameBlendEquationAlpha, kPnameBlendSrcRgb,
    kPnameBlendDstRgb, kPnameBlendSrcAlpha, kPnameBlendDstAlpha,
    kPnameCullFaceMode, kPnameFrontFace, kPnameDepthFunc,
    kPnameDepthWritemask, kPnameColorWritemask, kPnameScissorBox,
    kPnameViewport, kPnameStencilClearValue, kPnameStencilFunc,
    kPnameStencilRef, kPnameStencilValueMask, kPnameStencilWritemask,
    kPnameStencilFail, kPnameStencilPassDepthFail, kPnameStencilPassDepthPass,
    kPnameStencilBackFunc, kPnameStencilBackRef, kPnameStencilBackValueMask,
    kPnameStencilBackWritemask, kPnameStencilBackFail,
    kPnameStencilBackPassDepthFail, kPnameStencilBackPassDepthPass,
    kPnameSampleCoverageInvert,
    // 浮点状态
    kPnameBlendColor, kPnameColorClearValue, kPnameDepthClearValue,
    kPnameDepthRange, kPnameLineWidth, kPnamePolygonOffsetFactor,
    kPnamePolygonOffsetUnits, kPnameSampleCoverageValue,
};

} // namespace

// ---------------- 初始化 ----------------

void Reset() {
    g_state = State();
    g_version++;
}

void ResetToDefaults(int surfaceWidth, int surfaceHeight) {
    Reset();
    g_state.surfaceWidth = surfaceWidth;
    g_state.surfaceHeight = surfaceHeight;
    g_state.viewportBox[0] = 0;
    g_state.viewportBox[1] = 0;
    g_state.viewportBox[2] = surfaceWidth;
    g_state.viewportBox[3] = surfaceHeight;
    g_state.scissorBox[0] = 0;
    g_state.scissorBox[1] = 0;
    g_state.scissorBox[2] = surfaceWidth;
    g_state.scissorBox[3] = surfaceHeight;
}

void ResizeSurface(int surfaceWidth, int surfaceHeight) {
    int oldW = g_state.surfaceWidth;
    int oldH = g_state.surfaceHeight;

    bool viewportIsFull = g_state.viewportBox[0] == 0 && g_state.viewportBox[1] == 0 &&
                          g_state.viewportBox[2] == oldW && g_state.viewportBox[3] == oldH;
    bool scissorIsFull = g_state.scissorBox[0] == 0 && g_state.scissorBox[1] == 0 &&
                         g_state.scissorBox[2] == oldW && g_state.scissorBox[3] == oldH;

    if (viewportIsFull) {
        g_state.viewportBox[2] = surfaceWidth;
        g_state.viewportBox[3] = surfaceHeight;
    }
    if (scissorIsFull) {
        g_state.scissorBox[2] = surfaceWidth;
        g_state.scissorBox[3] = surfaceHeight;
    }
    g_state.surfaceWidth = surfaceWidth;
    g_state.surfaceHeight = surfaceHeight;
    // viewport/scissor 不进管线键，无需递增版本
}

// ---------------- 能力开关 ----------------

bool SetCapability(int cap, bool enabled) {
    int slot = CapSlotOf(cap);
    if (slot < 0) {
        SetError(kInvalidEnum);
        return false;
    }
    if (SetEnabledSlot(slot, enabled)) g_version++;
    return true;
}

bool IsEnabled(int cap) {
    int slot = CapSlotOf(cap);
    if (slot < 0) {
        SetError(kInvalidEnum);
        return false;
    }
    return IsEnabledSlot(slot);
}

bool IsCapability(int cap) { return CapSlotOf(cap) >= 0; }

// ---------------- 混合 ----------------

bool BlendFunc(int sfactor, int dfactor) {
    return BlendFuncSeparate(sfactor, dfactor, sfactor, dfactor);
}

bool BlendFuncSeparate(int srcRGB, int dstRGB, int srcAlpha, int dstAlpha) {
    if (!IsBlendFactor(srcRGB, true) || !IsBlendFactor(dstRGB, false) ||
        !IsBlendFactor(srcAlpha, true) || !IsBlendFactor(dstAlpha, false)) {
        SetError(kInvalidEnum);
        return false;
    }
    if (g_state.blendSrcRgb != srcRGB || g_state.blendDstRgb != dstRGB ||
        g_state.blendSrcAlpha != srcAlpha || g_state.blendDstAlpha != dstAlpha) {
        g_state.blendSrcRgb = srcRGB;
        g_state.blendDstRgb = dstRGB;
        g_state.blendSrcAlpha = srcAlpha;
        g_state.blendDstAlpha = dstAlpha;
        g_version++;
    }
    return true;
}

bool BlendEquation(int mode) { return BlendEquationSeparate(mode, mode); }

bool BlendEquationSeparate(int rgb, int alpha) {
    if (!IsBlendEquation(rgb) || !IsBlendEquation(alpha)) {
        SetError(kInvalidEnum);
        return false;
    }
    if (g_state.blendEqRgb != rgb || g_state.blendEqAlpha != alpha) {
        g_state.blendEqRgb = rgb;
        g_state.blendEqAlpha = alpha;
        g_version++;
    }
    return true;
}

void BlendColor(float r, float g, float b, float a) {
    g_state.blendColor[0] = r;
    g_state.blendColor[1] = g;
    g_state.blendColor[2] = b;
    g_state.blendColor[3] = a;
    // blendColor 是动态状态（vkCmdSetBlendConstants），不进管线键
}

void EnablePremultipliedAlphaBlend() {
    SetCapability(kBlend, true);
    BlendFuncSeparate(kOne, kOneMinusSrcAlpha, kOne, kOneMinusSrcAlpha);
}

// ---------------- 深度 ----------------

bool DepthFunc(int func) {
    if (!IsCompareFunc(func)) {
        SetError(kInvalidEnum);
        return false;
    }
    if (g_state.depthFunc != func) {
        g_state.depthFunc = func;
        g_version++;
    }
    return true;
}

void DepthMask(bool flag) {
    if (g_state.depthMask != flag) {
        g_state.depthMask = flag;
        g_version++;
    }
}

bool DepthRangef(float zNear, float zFar) {
    if (zNear < 0.0f) zNear = 0.0f;
    if (zNear > 1.0f) zNear = 1.0f;
    if (zFar < 0.0f) zFar = 0.0f;
    if (zFar > 1.0f) zFar = 1.0f;
    if (zNear > zFar) {
        SetError(kInvalidValue);
        return false;
    }
    // depthRange 不在 Vulkan 固定管线里，交给 shader/投影矩阵处理，只做状态记录
    g_state.depthRange[0] = zNear;
    g_state.depthRange[1] = zFar;
    return true;
}

// ---------------- 模板 ----------------

bool StencilFunc(int func, int ref, int mask) {
    return StencilFuncSeparate(kFrontAndBack, func, ref, mask);
}

bool StencilFuncSeparate(int face, int func, int ref, int mask) {
    if (!IsFace(face) || !IsCompareFunc(func)) {
        SetError(kInvalidEnum);
        return false;
    }
    if (ref < 0) ref = 0;
    if (ref > 255) ref = 255;  // 规范：clamp 到模板缓冲位宽，按 8bit 处理

    bool changed = false;
    // 本阶段前后 ref 强制同值：不管 face 取什么，ref 都写到单一槽位
    if (g_state.stencilRef != ref) {
        g_state.stencilRef = ref;
        changed = true;
    }
    FaceState* faces[2] = {nullptr, nullptr};
    int n = 0;
    if (face == kFront || face == kFrontAndBack) faces[n++] = &g_state.frontStencil;
    if (face == kBack || face == kFrontAndBack) faces[n++] = &g_state.backStencil;
    for (int i = 0; i < n; i++) {
        FaceState& f = *faces[i];
        if (f.func != func || f.valueMask != (unsigned)mask) {
            f.func = func;
            f.valueMask = (unsigned)mask;
            changed = true;
        }
    }
    // ref 是动态状态，本不该动版本号；但这里统一处理，且 ref 变化很少
    if (changed) g_version++;
    return true;
}

void StencilMask(int mask) { StencilMaskSeparate(kFrontAndBack, mask); }

void StencilMaskSeparate(int face, int mask) {
    if (!IsFace(face)) {
        SetError(kInvalidEnum);
        return;
    }
    bool changed = false;
    if (face == kFront || face == kFrontAndBack) {
        if (g_state.frontStencil.writeMask != (unsigned)mask) {
            g_state.frontStencil.writeMask = (unsigned)mask;
            changed = true;
        }
    }
    if (face == kBack || face == kFrontAndBack) {
        if (g_state.backStencil.writeMask != (unsigned)mask) {
            g_state.backStencil.writeMask = (unsigned)mask;
            changed = true;
        }
    }
    if (changed) g_version++;
}

bool StencilOp(int sfail, int dpfail, int dppass) {
    return StencilOpSeparate(kFrontAndBack, sfail, dpfail, dppass);
}

bool StencilOpSeparate(int face, int sfail, int dpfail, int dppass) {
    if (!IsFace(face) || !IsStencilOp(sfail) || !IsStencilOp(dpfail) || !IsStencilOp(dppass)) {
        SetError(kInvalidEnum);
        return false;
    }
    bool changed = false;
    if (face == kFront || face == kFrontAndBack) {
        FaceState& f = g_state.frontStencil;
        if (f.sfail != sfail || f.dpfail != dpfail || f.dppass != dppass) {
            f.sfail = sfail;
            f.dpfail = dpfail;
            f.dppass = dppass;
            changed = true;
        }
    }
    if (face == kBack || face == kFrontAndBack) {
        FaceState& f = g_state.backStencil;
        if (f.sfail != sfail || f.dpfail != dpfail || f.dppass != dppass) {
            f.sfail = sfail;
            f.dpfail = dpfail;
            f.dppass = dppass;
            changed = true;
        }
    }
    if (changed) g_version++;
    return true;
}

// ---------------- 光栅化 ----------------

bool CullFace(int mode) {
    if (mode != kFront && mode != kBack && mode != kFrontAndBack) {
        SetError(kInvalidEnum);
        return false;
    }
    if (g_state.cullMode != mode) {
        g_state.cullMode = mode;
        g_version++;
    }
    return true;
}

bool FrontFace(int mode) {
    if (mode != kCw && mode != kCcw) {
        SetError(kInvalidEnum);
        return false;
    }
    if (g_state.frontFace != mode) {
        g_state.frontFace = mode;
        g_version++;
    }
    return true;
}

void PolygonOffset(float factor, float units) {
    if (FloatChanged(g_state.polygonOffsetFactor, factor) ||
        FloatChanged(g_state.polygonOffsetUnits, units)) {
        g_state.polygonOffsetFactor = factor;
        g_state.polygonOffsetUnits = units;
        // depthBias 在 Vulkan 里属于管线状态（无扩展时），所以计入版本
        g_version++;
    }
}

bool LineWidth(float width) {
    if (width <= 0.0f) {
        SetError(kInvalidValue);
        return false;
    }
    if (FloatChanged(g_state.lineWidth, width)) {
        g_state.lineWidth = width;
        g_version++;
    }
    return true;
}

void ColorMask(bool red, bool green, bool blue, bool alpha) {
    bool next[4] = {red, green, blue, alpha};
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        if (g_state.colorMask[i] != next[i]) {
            g_state.colorMask[i] = next[i];
            changed = true;
        }
    }
    if (changed) g_version++;
}

void SampleCoverage(float value, bool invert) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    // 只影响多重采样覆盖率，不进管线键（alphaToCoverage 开关本身才进）
    if (FloatChanged(g_state.sampleCoverageValue, value) || g_state.sampleCoverageInvert != invert) {
        g_state.sampleCoverageValue = value;
        g_state.sampleCoverageInvert = invert;
        g_version++;
    }
}

// ---------------- 视口 / 剪裁 / 清除 ----------------

bool Viewport(int x, int y, int width, int height) {
    if (width < 0 || height < 0) {
        SetError(kInvalidValue);
        return false;
    }
    g_state.viewportBox[0] = x;
    g_state.viewportBox[1] = y;
    g_state.viewportBox[2] = width;
    g_state.viewportBox[3] = height;
    return true;
}

bool Scissor(int x, int y, int width, int height) {
    if (width < 0 || height < 0) {
        SetError(kInvalidValue);
        return false;
    }
    g_state.scissorBox[0] = x;
    g_state.scissorBox[1] = y;
    g_state.scissorBox[2] = width;
    g_state.scissorBox[3] = height;
    return true;
}

void ClearColor(float red, float green, float blue, float alpha) {
    g_state.clearColor[0] = red;
    g_state.clearColor[1] = green;
    g_state.clearColor[2] = blue;
    g_state.clearColor[3] = alpha;
}

void ClearDepthf(float depth) {
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;
    g_state.clearDepth = depth;
}

void ClearStencil(int stencil) {
    if (stencil < 0) stencil = 0;
    if (stencil > 255) stencil = 255;
    g_state.clearStencil = stencil;
}

// ---------------- 绘制拓扑 ----------------

bool SetDrawMode(int glMode) {
    if (glMode < kPoints || glMode > kTriangleFan) {
        SetError(kInvalidEnum);
        return false;
    }
    if (g_state.drawMode != glMode) {
        g_state.drawMode = glMode;
        g_version++;
    }
    return true;
}

bool DrawModeNeedsEmulation(int glMode) {
    return glMode == kTriangleFan || glMode == kLineLoop || glMode < kPoints || glMode > kTriangleFan;
}

// ---------------- 查询 ----------------

bool HandlesPname(int pname) {
    for (size_t i = 0; i < sizeof(kKnownPnames) / sizeof(kKnownPnames[0]); i++) {
        if (kKnownPnames[i] == pname) return true;
    }
    return false;
}

int QueryInteger(int pname, int* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;

    // 先按能力开关处理
    int slot = CapSlotOf(pname);
    if (slot >= 0) {
        out[0] = IsEnabledSlot(slot) ? 1 : 0;
        return 1;
    }

    switch (pname) {
        case kPnameBlendSrcRgb: out[0] = g_state.blendSrcRgb; return 1;
        case kPnameBlendDstRgb: out[0] = g_state.blendDstRgb; return 1;
        case kPnameBlendSrcAlpha: out[0] = g_state.blendSrcAlpha; return 1;
        case kPnameBlendDstAlpha: out[0] = g_state.blendDstAlpha; return 1;
        case kPnameBlendEquationRgb: out[0] = g_state.blendEqRgb; return 1;
        case kPnameBlendEquationAlpha: out[0] = g_state.blendEqAlpha; return 1;
        case kPnameCullFaceMode: out[0] = g_state.cullMode; return 1;
        case kPnameFrontFace: out[0] = g_state.frontFace; return 1;
        case kPnameDepthFunc: out[0] = g_state.depthFunc; return 1;
        case kPnameDepthWritemask: out[0] = g_state.depthMask ? 1 : 0; return 1;
        case kPnameStencilClearValue: out[0] = g_state.clearStencil; return 1;
        case kPnameStencilFunc: out[0] = g_state.frontStencil.func; return 1;
        case kPnameStencilRef: out[0] = g_state.stencilRef; return 1;
        case kPnameStencilValueMask: out[0] = (int)g_state.frontStencil.valueMask; return 1;
        case kPnameStencilWritemask: out[0] = (int)g_state.frontStencil.writeMask; return 1;
        case kPnameStencilFail: out[0] = g_state.frontStencil.sfail; return 1;
        case kPnameStencilPassDepthFail: out[0] = g_state.frontStencil.dpfail; return 1;
        case kPnameStencilPassDepthPass: out[0] = g_state.frontStencil.dppass; return 1;
        case kPnameStencilBackFunc: out[0] = g_state.backStencil.func; return 1;
        case kPnameStencilBackRef: out[0] = g_state.stencilRef; return 1;
        case kPnameStencilBackValueMask: out[0] = (int)g_state.backStencil.valueMask; return 1;
        case kPnameStencilBackWritemask: out[0] = (int)g_state.backStencil.writeMask; return 1;
        case kPnameStencilBackFail: out[0] = g_state.backStencil.sfail; return 1;
        case kPnameStencilBackPassDepthFail: out[0] = g_state.backStencil.dpfail; return 1;
        case kPnameStencilBackPassDepthPass: out[0] = g_state.backStencil.dppass; return 1;
        case kPnameSampleCoverageInvert: out[0] = g_state.sampleCoverageInvert ? 1 : 0; return 1;
        case kPnameColorWritemask:
            if (maxCount < 4) return 0;
            for (int i = 0; i < 4; i++) out[i] = g_state.colorMask[i] ? 1 : 0;
            return 4;
        case kPnameScissorBox:
            if (maxCount < 4) return 0;
            for (int i = 0; i < 4; i++) out[i] = g_state.scissorBox[i];
            return 4;
        case kPnameViewport:
            if (maxCount < 4) return 0;
            for (int i = 0; i < 4; i++) out[i] = g_state.viewportBox[i];
            return 4;
        default:
            SetError(kInvalidEnum);
            return 0;
    }
}

int QueryFloat(int pname, float* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;
    switch (pname) {
        case kPnameDepthClearValue: out[0] = g_state.clearDepth; return 1;
        case kPnameLineWidth: out[0] = g_state.lineWidth; return 1;
        case kPnamePolygonOffsetFactor: out[0] = g_state.polygonOffsetFactor; return 1;
        case kPnamePolygonOffsetUnits: out[0] = g_state.polygonOffsetUnits; return 1;
        case kPnameSampleCoverageValue: out[0] = g_state.sampleCoverageValue; return 1;
        case kPnameBlendColor:
            if (maxCount < 4) return 0;
            for (int i = 0; i < 4; i++) out[i] = g_state.blendColor[i];
            return 4;
        case kPnameColorClearValue:
            if (maxCount < 4) return 0;
            for (int i = 0; i < 4; i++) out[i] = g_state.clearColor[i];
            return 4;
        case kPnameDepthRange:
            if (maxCount < 2) return 0;
            out[0] = g_state.depthRange[0];
            out[1] = g_state.depthRange[1];
            return 2;
        default:
            SetError(kInvalidEnum);
            return 0;
    }
}

bool QueryBoolean(int pname, bool* out) {
    if (!out) return false;
    int iv[4] = {0, 0, 0, 0};
    int n = QueryInteger(pname, iv, 4);
    if (n > 0) {
        *out = iv[0] != 0;
        return true;
    }
    float fv[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    n = QueryFloat(pname, fv, 4);
    if (n > 0) {
        *out = fv[0] != 0.0f;
        return true;
    }
    return false;
}

void GetClearColor(float out[4]) {
    for (int i = 0; i < 4; i++) out[i] = g_state.clearColor[i];
}

float GetClearDepth() { return g_state.clearDepth; }
int GetClearStencil() { return g_state.clearStencil; }

void GetBlendColor(float out[4]) {
    for (int i = 0; i < 4; i++) out[i] = g_state.blendColor[i];
}

void GetScissorBox(int out[4]) {
    for (int i = 0; i < 4; i++) out[i] = g_state.scissorBox[i];
}

void GetViewportBox(int out[4]) {
    for (int i = 0; i < 4; i++) out[i] = g_state.viewportBox[i];
}

float GetLineWidth() { return g_state.lineWidth; }

int GetStencilRef(int face) {
    (void)face;  // 本阶段前后同值
    return g_state.stencilRef;
}

bool GetColorMask(int* out4) {
    if (!out4) return false;
    for (int i = 0; i < 4; i++) out4[i] = g_state.colorMask[i] ? 1 : 0;
    return true;
}

bool GetDepthRangef(float out2[2]) {
    if (!out2) return false;
    out2[0] = g_state.depthRange[0];
    out2[1] = g_state.depthRange[1];
    return true;
}

// ---------------- Vulkan 动态状态 ----------------

VkRect2D GetVulkanScissor(int targetWidth, int targetHeight) {
    VkRect2D r{};
    if (!IsEnabledSlot(CS_ScissorTest)) {
        r.offset.x = 0;
        r.offset.y = 0;
        r.extent.width = (uint32_t)(targetWidth > 0 ? targetWidth : 0);
        r.extent.height = (uint32_t)(targetHeight > 0 ? targetHeight : 0);
        return r;
    }

    int x = g_state.scissorBox[0];
    int y = g_state.scissorBox[1];
    int w = g_state.scissorBox[2];
    int h = g_state.scissorBox[3];
    // GL 原点在左下，Vulkan 在左上
    int vkY = targetHeight - (y + h);

    // 目标外的部分截掉，避免部分驱动对越界 scissor 的处理差异
    if (x < 0) { w += x; x = 0; }
    if (vkY < 0) { h += vkY; vkY = 0; }
    if (x + w > targetWidth) w = targetWidth - x;
    if (vkY + h > targetHeight) h = targetHeight - vkY;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    r.offset.x = x;
    r.offset.y = vkY;
    r.extent.width = (uint32_t)w;
    r.extent.height = (uint32_t)h;
    return r;
}

VkViewport GetVulkanViewport(int targetHeight) {
    VkViewport vp{};
    vp.x = (float)g_state.viewportBox[0];
    vp.y = (float)(targetHeight - (g_state.viewportBox[1] + g_state.viewportBox[3]));
    vp.width = (float)g_state.viewportBox[2];
    vp.height = (float)g_state.viewportBox[3];
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    return vp;
}

void SetClipSpaceYFlip(bool flip) {
    if (g_yFlip != flip) {
        g_yFlip = flip;
        g_version++;
    }
}

bool GetClipSpaceYFlip() { return g_yFlip; }

void SetWarningCallback(WarningCallback callback) { g_warnCallback = callback; }

void SetDualSourceBlendEnabled(bool enabled) {
    if (g_dualSource != enabled) {
        g_dualSource = enabled;
        g_version++;  // SRC1 因子是否被钳制会改变管线键
    }
}

int VkTopology(int glMode, bool* needsEmulation) {
    return (int)ToVkTopology(glMode, needsEmulation);
}

// ---------------- 管线键 ----------------

PipelineKey CurrentPipelineKey(const Formats& formats) {
    PipelineKey k;

    // 输入装配
    k.topology = VkTopology(g_state.drawMode, nullptr);
    k.primitiveRestart = IsEnabledSlot(CS_PrimitiveRestartFixedIndex);

    // 光栅化
    k.cullMode = IsEnabledSlot(CS_CullFace) ? (int)ToVkCullMode(g_state.cullMode) : (int)VK_CULL_MODE_NONE;
    k.frontFace = (int)ToVkFrontFace(g_state.frontFace);
    k.depthBiasEnable = IsEnabledSlot(CS_PolygonOffsetFill);
    // GL: offset = factor * 斜率 + units * 深度单位
    // Vulkan: depthBiasConstantFactor 对应 units，depthBiasSlopeFactor 对应 factor
    k.depthBiasConstant = g_state.polygonOffsetUnits;
    k.depthBiasSlope = g_state.polygonOffsetFactor;
    k.rasterizerDiscard = IsEnabledSlot(CS_RasterizerDiscard);
    k.lineWidth = g_state.lineWidth;

    // 深度
    k.depthTestEnable = IsEnabledSlot(CS_DepthTest);
    // GL 在深度测试关闭时不会写深度，这里保持同样的语义
    k.depthWriteEnable = k.depthTestEnable && g_state.depthMask;
    k.depthCompareOp = (int)ToVkCompareOp(g_state.depthFunc);

    // 模板
    k.stencilTestEnable = IsEnabledSlot(CS_StencilTest);
    k.frontStencil = MakeFace(g_state.frontStencil);
    k.backStencil = MakeFace(g_state.backStencil);

    // 混合
    k.blendEnable = IsEnabledSlot(CS_Blend);
    k.colorBlendOp = (int)ToVkBlendOp(g_state.blendEqRgb);
    k.alphaBlendOp = (int)ToVkBlendOp(g_state.blendEqAlpha);
    int sc = 0;
    int dc = 0;
    int sa = 0;
    int da = 0;
    // Vulkan 不接受的 GL 组合在这里钳制并警告一次（GL 状态本身不变）
    SanitizeBlendFactors(g_state.blendSrcRgb, g_state.blendDstRgb, g_state.blendSrcAlpha,
                         g_state.blendDstAlpha, sc, dc, sa, da);
    k.srcColorBlendFactor = (int)ToVkBlendFactor(sc);
    k.dstColorBlendFactor = (int)ToVkBlendFactor(dc);
    k.srcAlphaBlendFactor = (int)ToVkBlendFactor(sa);
    k.dstAlphaBlendFactor = (int)ToVkBlendFactor(da);
    NormalizeBlendForOp((VkBlendOp)k.colorBlendOp, k.srcColorBlendFactor, k.dstColorBlendFactor);
    NormalizeBlendForOp((VkBlendOp)k.alphaBlendOp, k.srcAlphaBlendFactor, k.dstAlphaBlendFactor);

    k.colorWriteMask = 0;
    if (g_state.colorMask[0]) k.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (g_state.colorMask[1]) k.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (g_state.colorMask[2]) k.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (g_state.colorMask[3]) k.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;

    k.formats = formats;
    return k;
}

const PipelineKey& CachedPipelineKey(const Formats& formats) {
    if (g_cachedVersion != g_version || g_cachedFormats != formats) {
        g_cachedKey = CurrentPipelineKey(formats);
        g_cachedFormats = formats;
        g_cachedVersion = g_version;
    }
    return g_cachedKey;
}

uint64_t Version() { return g_version; }

bool PipelineKey::operator==(const PipelineKey& o) const {
    if (topology != o.topology || primitiveRestart != o.primitiveRestart ||
        cullMode != o.cullMode || frontFace != o.frontFace ||
        depthBiasEnable != o.depthBiasEnable ||
        FloatChanged(depthBiasConstant, o.depthBiasConstant) ||
        FloatChanged(depthBiasSlope, o.depthBiasSlope) ||
        rasterizerDiscard != o.rasterizerDiscard ||
        FloatChanged(lineWidth, o.lineWidth) ||
        depthTestEnable != o.depthTestEnable || depthWriteEnable != o.depthWriteEnable ||
        depthCompareOp != o.depthCompareOp || stencilTestEnable != o.stencilTestEnable ||
        !(frontStencil == o.frontStencil) || !(backStencil == o.backStencil) ||
        blendEnable != o.blendEnable ||
        srcColorBlendFactor != o.srcColorBlendFactor || dstColorBlendFactor != o.dstColorBlendFactor ||
        colorBlendOp != o.colorBlendOp ||
        srcAlphaBlendFactor != o.srcAlphaBlendFactor || dstAlphaBlendFactor != o.dstAlphaBlendFactor ||
        alphaBlendOp != o.alphaBlendOp || colorWriteMask != o.colorWriteMask ||
        formats != o.formats) {
        return false;
    }
    return true;
}

uint64_t PipelineKey::Hash() const {
    uint64_t h = 1469598103934665603ull;
    h = Fnv(h, (uint64_t)topology);
    h = Fnv(h, primitiveRestart ? 1 : 0);
    h = Fnv(h, (uint64_t)cullMode);
    h = Fnv(h, (uint64_t)frontFace);
    h = Fnv(h, depthBiasEnable ? 1 : 0);
    h = Fnv(h, FloatBits(depthBiasConstant));
    h = Fnv(h, FloatBits(depthBiasSlope));
    h = Fnv(h, rasterizerDiscard ? 1 : 0);
    h = Fnv(h, FloatBits(lineWidth));
    h = Fnv(h, depthTestEnable ? 1 : 0);
    h = Fnv(h, depthWriteEnable ? 1 : 0);
    h = Fnv(h, (uint64_t)depthCompareOp);
    h = Fnv(h, stencilTestEnable ? 1 : 0);
    h = HashFace(h, frontStencil);
    h = HashFace(h, backStencil);
    h = Fnv(h, blendEnable ? 1 : 0);
    h = Fnv(h, (uint64_t)srcColorBlendFactor);
    h = Fnv(h, (uint64_t)dstColorBlendFactor);
    h = Fnv(h, (uint64_t)colorBlendOp);
    h = Fnv(h, (uint64_t)srcAlphaBlendFactor);
    h = Fnv(h, (uint64_t)dstAlphaBlendFactor);
    h = Fnv(h, (uint64_t)alphaBlendOp);
    h = Fnv(h, (uint64_t)colorWriteMask);
    h = Fnv(h, (uint64_t)formats.colorFormat);
    h = Fnv(h, (uint64_t)formats.depthFormat);
    h = Fnv(h, (uint64_t)formats.stencilFormat);
    h = Fnv(h, (uint64_t)formats.samples);
    return h;
}

// ---------------- 填充 Vulkan 创建结构 ----------------

void FillInputAssembly(const PipelineKey& key, VkPipelineInputAssemblyStateCreateInfo* out) {
    if (!out) return;
    *out = VkPipelineInputAssemblyStateCreateInfo{};
    out->sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    out->topology = (VkPrimitiveTopology)key.topology;
    out->primitiveRestartEnable = key.primitiveRestart ? VK_TRUE : VK_FALSE;
}

void FillRasterization(const PipelineKey& key, VkPipelineRasterizationStateCreateInfo* out) {
    if (!out) return;
    *out = VkPipelineRasterizationStateCreateInfo{};
    out->sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    // GLES 没有 glDepthClamp：规范要求片元深度强制 clamp 到 [0,1]，
    // 对应 Vulkan 的 depthClampEnable = VK_FALSE（开启 clamp）
    out->depthClampEnable = VK_FALSE;
    out->rasterizerDiscardEnable = key.rasterizerDiscard ? VK_TRUE : VK_FALSE;
    out->polygonMode = VK_POLYGON_MODE_FILL;  // GLES 只有 FILL
    out->cullMode = (VkCullModeFlags)key.cullMode;
    out->frontFace = (VkFrontFace)key.frontFace;
    out->depthBiasEnable = key.depthBiasEnable ? VK_TRUE : VK_FALSE;
    out->depthBiasConstantFactor = key.depthBiasConstant;
    out->depthBiasClamp = 0.0f;  // GL 没有 clamp
    out->depthBiasSlopeFactor = key.depthBiasSlope;
    out->lineWidth = key.lineWidth;
}

void FillDepthStencil(const PipelineKey& key, VkPipelineDepthStencilStateCreateInfo* out) {
    if (!out) return;
    *out = VkPipelineDepthStencilStateCreateInfo{};
    out->sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    out->depthTestEnable = key.depthTestEnable ? VK_TRUE : VK_FALSE;
    out->depthWriteEnable = key.depthWriteEnable ? VK_TRUE : VK_FALSE;
    out->depthCompareOp = (VkCompareOp)key.depthCompareOp;
    out->depthBoundsTestEnable = VK_FALSE;
    out->minDepthBounds = 0.0f;
    out->maxDepthBounds = 1.0f;
    out->stencilTestEnable = key.stencilTestEnable ? VK_TRUE : VK_FALSE;

    VkStencilOpState& front = out->front;
    front.failOp = (VkStencilOp)key.frontStencil.failOp;
    front.passOp = (VkStencilOp)key.frontStencil.passOp;
    front.depthFailOp = (VkStencilOp)key.frontStencil.passDepthFailOp;
    front.compareOp = (VkCompareOp)key.frontStencil.compareOp;
    front.compareMask = key.frontStencil.compareMask;
    front.writeMask = key.frontStencil.writeMask;
    front.reference = 0;  // 动态状态：提交时用 vkCmdSetStencilReference(FRONT_AND_BACK, GetStencilRef)
    // 本阶段前后 ref 强制同值，所以一次 FRONT_AND_BACK 的 set 就覆盖两面

    VkStencilOpState& back = out->back;
    back.failOp = (VkStencilOp)key.backStencil.failOp;
    back.passOp = (VkStencilOp)key.backStencil.passOp;
    back.depthFailOp = (VkStencilOp)key.backStencil.passDepthFailOp;
    back.compareOp = (VkCompareOp)key.backStencil.compareOp;
    back.compareMask = key.backStencil.compareMask;
    back.writeMask = key.backStencil.writeMask;
    back.reference = 0;
}

void FillColorBlendAttachment(const PipelineKey& key, VkPipelineColorBlendAttachmentState* out) {
    if (!out) return;
    *out = VkPipelineColorBlendAttachmentState{};
    out->blendEnable = key.blendEnable ? VK_TRUE : VK_FALSE;
    out->srcColorBlendFactor = (VkBlendFactor)key.srcColorBlendFactor;
    out->dstColorBlendFactor = (VkBlendFactor)key.dstColorBlendFactor;
    out->colorBlendOp = (VkBlendOp)key.colorBlendOp;
    out->srcAlphaBlendFactor = (VkBlendFactor)key.srcAlphaBlendFactor;
    out->dstAlphaBlendFactor = (VkBlendFactor)key.dstAlphaBlendFactor;
    out->alphaBlendOp = (VkBlendOp)key.alphaBlendOp;
    out->colorWriteMask = key.colorWriteMask;
}

void FillMultisample(const PipelineKey& key, VkPipelineMultisampleStateCreateInfo* out) {
    if (!out) return;
    *out = VkPipelineMultisampleStateCreateInfo{};
    out->sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    out->rasterizationSamples = ToVkSamples(key.formats.samples);
    out->sampleShadingEnable = VK_FALSE;
    out->minSampleShading = 1.0f;
    out->pSampleMask = nullptr;
    out->alphaToCoverageEnable = IsEnabledSlot(CS_SampleAlphaToCoverage) ? VK_TRUE : VK_FALSE;
    out->alphaToOneEnable = VK_FALSE;
}

// ---------------- 错误码 ----------------

void SetError(int glError) {
    if (glError != 0 && g_error == 0) g_error = glError;
}

int PeekError() { return g_error; }

int TakeError() {
    int e = g_error;
    g_error = 0;
    return e;
}

} // namespace glstate
} // namespace lime
