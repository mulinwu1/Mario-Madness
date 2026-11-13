#pragma once

#include "vk_device_caps.h"

namespace lime {
namespace glquery {

// 翻译层最终愿意公开的 GL 能力。所有值都必须是“设备限制 ∩ 已实现上限”，
// 没实现的能力用 0 表示，不能拿设备上限或隐藏 GL 的值来充数。
struct Caps {
    bool ready = false;

    int maxTextureSize = 0;
    int maxViewportWidth = 0;
    int maxViewportHeight = 0;
    int maxTextureImageUnits = 0;
    int maxCombinedTextureImageUnits = 0;
    int maxVertexTextureImageUnits = 0;
    int maxVertexAttribs = 0;
    int maxColorAttachments = 0;
    int maxDrawBuffers = 0;
    int maxSamples = 0;
    int samples = 0;
    int sampleBuffers = 0;
    int maxRenderbufferSize = 0;
    int maxCubeMapTextureSize = 0;
    int max3DTextureSize = 0;
    int maxArrayTextureLayers = 0;
    int maxVertexUniformVectors = 0;
    int maxFragmentUniformVectors = 0;
    int maxVaryingVectors = 0;
    int subpixelBits = 0;
    int redBits = 0;
    int greenBits = 0;
    int blueBits = 0;
    int alphaBits = 0;
    int maxTextureMaxAnisotropy = 1;
    float aliasedLineWidthRange[2] = {1.0f, 1.0f};
    float aliasedPointSizeRange[2] = {1.0f, 1.0f};
    int implementationColorReadFormat = 0;
    int implementationColorReadType = 0;
    int numCompressedTextureFormats = 0;
};

// 只读状态 getter；由 gl_core 在 renderer ready 后安装。
struct State {
    int (*activeTexture)() = nullptr;
    int (*boundTexture2D)() = nullptr;
    int (*boundFramebuffer)() = nullptr;
    int (*boundReadFramebuffer)() = nullptr;
    int (*boundRenderbuffer)() = nullptr;
    int (*currentProgram)() = nullptr;
    int (*boundArrayBuffer)() = nullptr;
    int (*boundElementBuffer)() = nullptr;
    bool (*pixelStore)(int pname, int* value) = nullptr;
};

void SetCaps(const Caps& caps);
void ClearCaps();
void SetState(const State* state);

// 返回 -1 = 未处理；0 = 已处理但零分量；>0 = 写入的分量数。
int QueryInteger(int pname, int* out, int maxCount);
int QueryFloat(int pname, float* out, int maxCount);
int QueryBooleanv(int pname, unsigned char* out, int maxCount);

// 从 VkDeviceCaps 生成 Caps：min(设备限制, 已实现上限)。
Caps BuildCaps(const VkDeviceCaps& device, bool ready);

// 未处理 pname：记一次 GL_INVALID_ENUM，并打一条 [glquery] 日志（同 pname 只打一次）。
void Unhandled(int pname);

// 退出时输出 handled/unhandled 汇总。
void Summary();

} // namespace glquery
} // namespace lime
