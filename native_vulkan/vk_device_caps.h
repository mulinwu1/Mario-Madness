#pragma once

#include <cstdint>

// Vulkan 设备的只读能力快照。
//
// 约束：本头不得出现 Vulkan 类型或平台类型，只放普通 POD；
// vk_renderer.cpp 在 Init 时从 VkPhysicalDeviceProperties / Features /
// FormatProperties 填进来，gl_core 再把它翻译成 GlQueryCaps。
struct VkDeviceCaps {
    bool valid = false;
    char deviceName[256] = {};
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t apiVersion = 0;
    uint32_t driverVersion = 0;
    int deviceType = 0;

    // VkPhysicalDeviceLimits 里与 GL 查询相关的部分
    uint32_t maxImageDimension2D = 0;
    uint32_t maxImageDimensionCube = 0;
    uint32_t maxImageDimension3D = 0;
    uint32_t maxFramebufferWidth = 0;
    uint32_t maxFramebufferHeight = 0;
    uint32_t maxViewportWidth = 0;
    uint32_t maxViewportHeight = 0;
    uint32_t maxPerStageDescriptorSampledImages = 0;
    uint32_t maxDescriptorSetSampledImages = 0;
    uint32_t maxVertexInputAttributes = 0;
    uint32_t maxVertexInputBindings = 0;
    uint32_t maxVertexOutputComponents = 0;
    uint32_t maxFragmentInputComponents = 0;
    uint32_t maxFragmentOutputAttachments = 0;
    uint32_t maxColorAttachments = 0;
    uint32_t framebufferColorSampleCounts = 0;
    uint32_t framebufferDepthSampleCounts = 0;
    uint32_t subPixelPrecisionBits = 0;
    float maxSamplerAnisotropy = 0.0f;
    float lineWidthRange[2] = {1.0f, 1.0f};
    float pointSizeRange[2] = {1.0f, 1.0f};
    uint32_t maxUniformBufferRange = 0;

    // 只记录实际启用的 feature；当前 dci.pEnabledFeatures 全 false，
    // 所以这些值在 2A 里应保持 false，除非以后明确开启。
    bool samplerAnisotropy = false;
    bool wideLines = false;
    bool independentBlend = false;
    bool depthClamp = false;

    // 格式能力（optimal tiling）
    bool colorBgra8 = false;
    bool colorRgba8 = false;
    bool depth24s8 = false;
    bool depth32f = false;

    // 实际启用的 device extension，逗号分隔，便于日志/报告。
    char enabledDeviceExtensions[256] = {};
};
