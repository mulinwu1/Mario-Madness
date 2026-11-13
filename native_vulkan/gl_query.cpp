#include "gl_query.h"
#include "gl_state.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace lime {
namespace glquery {
namespace {

// GL 常量：本文件不 include GL 头，避免和 lime 的宏/类型撞名。
enum : int {
    kMaxTextureSize = 0x0D33,
    kMaxViewportDims = 0x0D3A,
    kMaxTextureImageUnits = 0x8872,
    kMaxCombinedTextureImageUnits = 0x8B4D,
    kMaxVertexTextureImageUnits = 0x8B4C,
    kMaxVertexAttribs = 0x8869,
    kMaxColorAttachments = 0x8CDF,
    kMaxDrawBuffers = 0x8824,
    kMaxSamples = 0x8D57,
    kSamples = 0x80A9,
    kSampleBuffers = 0x80A8,
    kMaxRenderbufferSize = 0x84E8,
    kMaxCubeMapTextureSize = 0x851C,
    kMax3DTextureSize = 0x8073,
    kMaxArrayTextureLayers = 0x88FF,
    kMaxVertexUniformVectors = 0x8DFB,
    kMaxFragmentUniformVectors = 0x8DFD,
    kMaxVaryingVectors = 0x8DFC,
    kSubpixelBits = 0x0D50,
    kRedBits = 0x0D52,
    kGreenBits = 0x0D53,
    kBlueBits = 0x0D54,
    kAlphaBits = 0x0D55,
    kDepthBits = 0x0D56,
    kStencilBits = 0x0D57,
    kMaxTextureMaxAnisotropy = 0x84FF,
    kAliasedLineWidthRange = 0x846E,
    kAliasedPointSizeRange = 0x846D,
    kImplementationColorReadFormat = 0x8B9B,
    kImplementationColorReadType = 0x8B9A,
    // GLES/WebGL：NUM 是 0x86A2，列表是 0x86A3。之前写反了。
    kNumCompressedTextureFormats = 0x86A2,
    kCompressedTextureFormats = 0x86A3,
    kNumExtensions = 0x821D,

    kActiveTexture = 0x84E0,
    kTextureBinding2D = 0x8069,
    kTextureBindingCubeMap = 0x8514,
    kFramebufferBinding = 0x8CA6,
    kReadFramebufferBinding = 0x8CAA,
    kRenderbufferBinding = 0x8CA7,
    kCurrentProgram = 0x8B8D,
    kArrayBufferBinding = 0x8894,
    kElementArrayBufferBinding = 0x8895,
    kVertexArrayBinding = 0x85B5,
    kSamplerBinding = 0x8919,
    kTransformFeedbackBinding = 0x8E25,

    // pixelStore
    kPackAlignment = 0x0D05,
    kUnpackAlignment = 0x0CF5,
    kPackRowLength = 0x0D02,
    kUnpackRowLength = 0x0CF2,
    kPackSkipRows = 0x0D03,
    kUnpackSkipRows = 0x0CF3,
    kPackSkipPixels = 0x0D04,
    kUnpackSkipPixels = 0x0CF4,
    kUnpackFlipY = 0x9240,
    kUnpackPremultiplyAlpha = 0x9241,
    kUnpackColorspaceConversion = 0x9243
};

Caps g_caps;
const State* g_state = nullptr;
std::vector<int> g_unhandledPnames;
unsigned long long g_unhandledCalls = 0;

int MinCap(unsigned int deviceValue, int implValue) {
    if (deviceValue == 0) return 0;
    int value = (int)deviceValue;
    return value < implValue ? value : implValue;
}

int RoundToInt(float value) {
    if (!std::isfinite(value)) return 0;
    if (value <= (float)INT_MIN) return INT_MIN;
    if (value >= (float)INT_MAX) return INT_MAX;
    return (int)std::lround(value);
}

int CopyIntAsFloat(int count, const int* src, float* out, int maxCount) {
    if (count <= 0) return 0;
    if (count > maxCount) return 0;
    for (int i = 0; i < count; i++) out[i] = (float)src[i];
    return count;
}

int CopyIntAsBool(int count, const int* src, unsigned char* out, int maxCount) {
    if (count <= 0) return 0;
    if (count > maxCount) return 0;
    for (int i = 0; i < count; i++) out[i] = src[i] ? 1 : 0;
    return count;
}

int CopyFloatAsBool(int count, const float* src, unsigned char* out, int maxCount) {
    if (count <= 0) return 0;
    if (count > maxCount) return 0;
    for (int i = 0; i < count; i++) out[i] = src[i] != 0.0f ? 1 : 0;
    return count;
}

int QueryPixelStoreInt(int pname, int* out) {
    if (!g_state || !g_state->pixelStore) return -1;
    int value = 0;
    if (!g_state->pixelStore(pname, &value)) return -1;
    out[0] = value;
    return 1;
}

int QueryBindingInt(int pname, int* out) {
    switch (pname) {
        case kVertexArrayBinding:
        case kSamplerBinding:
        case kTransformFeedbackBinding:
        case kTextureBindingCubeMap:
            out[0] = 0;
            return 1;
        case kActiveTexture:
            if (!g_state || !g_state->activeTexture) return -1;
            out[0] = g_state->activeTexture();
            return 1;
        case kTextureBinding2D:
            if (!g_state || !g_state->boundTexture2D) return -1;
            out[0] = g_state->boundTexture2D();
            return 1;
        case kFramebufferBinding:
            if (!g_state || !g_state->boundFramebuffer) return -1;
            out[0] = g_state->boundFramebuffer();
            return 1;
        case kReadFramebufferBinding:
            if (!g_state || !g_state->boundReadFramebuffer) return -1;
            out[0] = g_state->boundReadFramebuffer();
            return 1;
        case kRenderbufferBinding:
            if (!g_state || !g_state->boundRenderbuffer) return -1;
            out[0] = g_state->boundRenderbuffer();
            return 1;
        case kCurrentProgram:
            if (!g_state || !g_state->currentProgram) return -1;
            out[0] = g_state->currentProgram();
            return 1;
        case kArrayBufferBinding:
            if (!g_state || !g_state->boundArrayBuffer) return -1;
            out[0] = g_state->boundArrayBuffer();
            return 1;
        case kElementArrayBufferBinding:
            if (!g_state || !g_state->boundElementBuffer) return -1;
            out[0] = g_state->boundElementBuffer();
            return 1;
        default:
            return -1;
    }
}

int QueryCapsInt(int pname, int* out, int maxCount) {
    if (!g_caps.ready) return -1;
    switch (pname) {
        case kMaxTextureSize: out[0] = g_caps.maxTextureSize; return 1;
        case kMaxViewportDims:
            if (maxCount < 2) return 0;
            out[0] = g_caps.maxViewportWidth;
            out[1] = g_caps.maxViewportHeight;
            return 2;
        case kMaxTextureImageUnits: out[0] = g_caps.maxTextureImageUnits; return 1;
        case kMaxCombinedTextureImageUnits: out[0] = g_caps.maxCombinedTextureImageUnits; return 1;
        case kMaxVertexTextureImageUnits: out[0] = g_caps.maxVertexTextureImageUnits; return 1;
        case kMaxVertexAttribs: out[0] = g_caps.maxVertexAttribs; return 1;
        case kMaxColorAttachments: out[0] = g_caps.maxColorAttachments; return 1;
        case kMaxDrawBuffers: out[0] = g_caps.maxDrawBuffers; return 1;
        case kMaxSamples: out[0] = g_caps.maxSamples; return 1;
        case kSamples: out[0] = g_caps.samples; return 1;
        case kSampleBuffers: out[0] = g_caps.sampleBuffers; return 1;
        case kMaxRenderbufferSize: out[0] = g_caps.maxRenderbufferSize; return 1;
        case kMaxCubeMapTextureSize: out[0] = g_caps.maxCubeMapTextureSize; return 1;
        case kMax3DTextureSize: out[0] = g_caps.max3DTextureSize; return 1;
        case kMaxArrayTextureLayers: out[0] = g_caps.maxArrayTextureLayers; return 1;
        case kMaxVertexUniformVectors: out[0] = g_caps.maxVertexUniformVectors; return 1;
        case kMaxFragmentUniformVectors: out[0] = g_caps.maxFragmentUniformVectors; return 1;
        case kMaxVaryingVectors: out[0] = g_caps.maxVaryingVectors; return 1;
        case kSubpixelBits: out[0] = g_caps.subpixelBits; return 1;
        case kRedBits: out[0] = g_caps.redBits; return 1;
        case kGreenBits: out[0] = g_caps.greenBits; return 1;
        case kBlueBits: out[0] = g_caps.blueBits; return 1;
        case kAlphaBits: out[0] = g_caps.alphaBits; return 1;
        case kDepthBits: out[0] = 0; return 1;
        case kStencilBits: out[0] = 0; return 1;
        case kMaxTextureMaxAnisotropy: out[0] = g_caps.maxTextureMaxAnisotropy; return 1;
        case kImplementationColorReadFormat: out[0] = g_caps.implementationColorReadFormat; return 1;
        case kImplementationColorReadType: out[0] = g_caps.implementationColorReadType; return 1;
        case kNumCompressedTextureFormats: out[0] = g_caps.numCompressedTextureFormats; return 1;
        case kCompressedTextureFormats: return 0;  // 已处理，但压缩格式列表为空
        case kNumExtensions: out[0] = 2; return 1;  // 与 gl_string.cpp 的扩展列表一致
        case kAliasedLineWidthRange:
            if (maxCount < 2) return 0;
            out[0] = RoundToInt(g_caps.aliasedLineWidthRange[0]);
            out[1] = RoundToInt(g_caps.aliasedLineWidthRange[1]);
            return 2;
        case kAliasedPointSizeRange:
            if (maxCount < 2) return 0;
            out[0] = RoundToInt(g_caps.aliasedPointSizeRange[0]);
            out[1] = RoundToInt(g_caps.aliasedPointSizeRange[1]);
            return 2;
        default: return -1;
    }
}

} // namespace

void SetCaps(const Caps& caps) {
    // gl_vk_shutdown 目前没有在退出路径上被调用；这里挂一个 atexit，
    // 保证即使进程直接退出也能看到 handled/unhandled 汇总。
    static bool summaryHooked = false;
    if (!summaryHooked) {
        std::atexit([] { Summary(); });
        summaryHooked = true;
    }
    g_caps = caps;
}
void ClearCaps() { g_caps = Caps(); }
void SetState(const State* state) { g_state = state; }

Caps BuildCaps(const VkDeviceCaps& device, bool ready) {
    Caps caps;
    caps.maxTextureSize = MinCap(device.maxImageDimension2D, 4096);
    caps.maxViewportWidth = MinCap(device.maxViewportWidth, 16384);
    caps.maxViewportHeight = MinCap(device.maxViewportHeight, 16384);
    caps.maxTextureImageUnits = MinCap(device.maxPerStageDescriptorSampledImages, 16);
    caps.maxCombinedTextureImageUnits = MinCap(device.maxDescriptorSetSampledImages, 16);
    caps.maxVertexTextureImageUnits = 0;  // 当前管线不承诺顶点纹理采样
    caps.maxVertexAttribs = MinCap(device.maxVertexInputAttributes, 16);
    caps.maxColorAttachments = 1;
    caps.maxDrawBuffers = 1;
    caps.maxSamples = 1;
    caps.samples = 0;
    caps.sampleBuffers = 0;

    unsigned int maxFb = std::min(device.maxFramebufferWidth, device.maxFramebufferHeight);
    caps.maxRenderbufferSize = MinCap(maxFb, 4096);
    caps.maxCubeMapTextureSize = 0;
    caps.max3DTextureSize = 0;
    caps.maxArrayTextureLayers = 0;

    // 顶点/片元共用同一个 program UBO；两个阶段都报满 maxUniformBufferRange
    // 会让总和超预算。按共享预算折半，再各自封顶 1024。
    unsigned int uniformVectors = device.maxUniformBufferRange / 16u;
    unsigned int perStageVectors = uniformVectors > 1 ? uniformVectors / 2 : uniformVectors;
    caps.maxVertexUniformVectors = MinCap(perStageVectors, 1024);
    caps.maxFragmentUniformVectors = MinCap(perStageVectors, 1024);

    unsigned int varyingComponents = std::min(device.maxVertexOutputComponents,
                                              device.maxFragmentInputComponents);
    // 不再凭“内建占 4 个 vec4”扣减：那会把 min-spec 设备压到 GLES3 下限以下。
    // Vulkan 的 component 上限本身是硬件限制，真正超限由 SPIR-V 链接期报错。
    int varyingVectors = (int)(varyingComponents / 4u);
    if (varyingVectors > 32) varyingVectors = 32;
    caps.maxVaryingVectors = varyingVectors;

    caps.subpixelBits = (int)device.subPixelPrecisionBits;
    const bool hasColor = device.colorBgra8 || device.colorRgba8;
    caps.redBits = hasColor ? 8 : 0;
    caps.greenBits = hasColor ? 8 : 0;
    caps.blueBits = hasColor ? 8 : 0;
    caps.alphaBits = hasColor ? 8 : 0;
    caps.maxTextureMaxAnisotropy =
        (device.samplerAnisotropy && device.maxSamplerAnisotropy >= 1.0f)
            ? (int)device.maxSamplerAnisotropy : 1;

    if (device.wideLines) {
        caps.aliasedLineWidthRange[0] = device.lineWidthRange[0];
        caps.aliasedLineWidthRange[1] = device.lineWidthRange[1];
    } else {
        caps.aliasedLineWidthRange[0] = 1.0f;
        caps.aliasedLineWidthRange[1] = 1.0f;
    }
    caps.aliasedPointSizeRange[0] = 1.0f;
    caps.aliasedPointSizeRange[1] = 1.0f;

    // 离屏 RenderTarget 已支持 RGBA8 回读；默认缓冲仍未接，但格式/类型本身是合法的。
    caps.implementationColorReadFormat = 0x1908;  // GL_RGBA
    caps.implementationColorReadType = 0x1401;    // GL_UNSIGNED_BYTE
    caps.numCompressedTextureFormats = 0;

    caps.ready = ready && device.valid &&
                 caps.maxTextureSize > 0 &&
                 caps.maxViewportWidth > 0 &&
                 caps.maxViewportHeight > 0;
    return caps;
}

int QueryInteger(int pname, int* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;

    if (glstate::HandlesPname(pname)) {
        return glstate::QueryInteger(pname, out, maxCount);
    }

    int n = QueryPixelStoreInt(pname, out);
    if (n >= 0) return n;
    n = QueryBindingInt(pname, out);
    if (n >= 0) return n;
    n = QueryCapsInt(pname, out, maxCount);
    if (n >= 0) return n;
    return -1;
}

int QueryFloat(int pname, float* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;

    if (glstate::HandlesPname(pname)) {
        return glstate::QueryFloat(pname, out, maxCount);
    }

    int iv[16] = {};
    int n = QueryPixelStoreInt(pname, iv);
    if (n >= 0) return CopyIntAsFloat(n, iv, out, maxCount);
    n = QueryBindingInt(pname, iv);
    if (n >= 0) return CopyIntAsFloat(n, iv, out, maxCount);

    // 浮点范围必须走 float 路径，不能被 QueryCapsInt 转成 int 再转回来。
    if (pname == kAliasedLineWidthRange || pname == kAliasedPointSizeRange) {
        if (!g_caps.ready) return -1;
        if (maxCount < 2) return 0;
        const float* range = (pname == kAliasedLineWidthRange)
                                 ? g_caps.aliasedLineWidthRange
                                 : g_caps.aliasedPointSizeRange;
        out[0] = range[0];
        out[1] = range[1];
        return 2;
    }

    n = QueryCapsInt(pname, iv, 16);
    if (n >= 0) return CopyIntAsFloat(n, iv, out, maxCount);
    return -1;
}

int QueryBooleanv(int pname, unsigned char* out, int maxCount) {
    if (!out || maxCount <= 0) return 0;

    if (glstate::HandlesPname(pname)) {
        return glstate::QueryBooleanv(pname, out, maxCount);
    }

    int iv[16] = {};
    int n = QueryInteger(pname, iv, 16);
    if (n >= 0) return CopyIntAsBool(n, iv, out, maxCount);

    float fv[16] = {};
    n = QueryFloat(pname, fv, 16);
    if (n >= 0) return CopyFloatAsBool(n, fv, out, maxCount);
    return -1;
}

void Unhandled(int pname) {
    g_unhandledCalls++;
    bool seen = false;
    for (size_t i = 0; i < g_unhandledPnames.size(); i++) {
        if (g_unhandledPnames[i] == pname) { seen = true; break; }
    }
    if (!seen) {
        g_unhandledPnames.push_back(pname);
        std::fprintf(stderr, "[glquery] unhandled pname=0x%04X\n", pname);
    }
    glstate::SetError(0x0500);  // GL_INVALID_ENUM
}

void Summary() {
    static bool printed = false;
    if (printed) return;
    printed = true;
    std::fprintf(stderr, "[glquery] summary unhandled_calls=%llu unhandled_pnames=%zu\n",
                 g_unhandledCalls, g_unhandledPnames.size());
    for (size_t i = 0; i < g_unhandledPnames.size(); i++) {
        std::fprintf(stderr, "[glquery] unhandled-list pname=0x%04X\n",
                     g_unhandledPnames[i]);
    }
}

} // namespace glquery
} // namespace lime
