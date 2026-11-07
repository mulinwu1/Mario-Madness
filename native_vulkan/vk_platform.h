#pragma once

#include <cstdint>

// 平台边界层：公共头里只允许出现 void* / uint64_t / POD，不允许任何平台类型或平台 API 符号。
// 桌面实现 = vk_platform_win32.cpp（win32 surface）
// 安卓实现 = vk_platform_android.cpp（android surface）
// 两个实现由 patch_lime.py 按平台分别加入 Build.xml，同一平台只编译其中一个。
//
// Vulkan 入口统一运行时加载：桌面走 vulkan-1.dll，安卓走 libvulkan.so，
// 不静态链接任何 Vulkan loader。
namespace vkplat {

// 加载 Vulkan loader；失败返回 nullptr 并写 err（最多 errCap 字节）
void* LoadVulkan (char* err, int errCap);

// 取入口地址（各平台的符号解析方式不同，细节只在平台实现里）
void* Proc (void* lib, const char* name);

// 实例扩展清单：VK_KHR_surface + 平台 surface 扩展
const char* const* InstanceExtensions (uint32_t* count);

// 用原生窗口句柄建 surface。
// instance = VkInstance（uint64_t 传，避免 32 位截断）；gipa = PFN_vkGetInstanceProcAddr。
bool CreateSurface (uint64_t instance, void* gipa, void* nativeWindow,
                    uint64_t* outSurface, char* err, int errCap);

}
