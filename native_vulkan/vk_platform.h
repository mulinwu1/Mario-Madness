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

// 可执行文件所在目录（含末尾分隔符，例如 "C:\app\"）。
// 只用 char 缓冲、不返回字符串，保持本头无平台类型。
// 取不到时写空串（调用方应视作"没有外部资源目录"）。
// 安卓实现恒返回空串：那里没有"可执行文件旁边的资源目录"这个概念。
void ExeDir (char* out, int outCap);

// 单调毫秒（交换链重建节流、色键扫描节流等）。
uint64_t TicksMs ();

// 高精度毫秒（只用于诊断计时，不参与逻辑）。
double NowMs ();

// 窗口客户区尺寸；Android 恒返回 false（用 surface 尺寸即可，没有"窗口边框"概念）。
bool WindowClientSize (void* nativeWindow, int* outW, int* outH);

// 窗口是否带 layered 样式（Windows 色键透明用）；Android 恒 false。
bool WindowIsLayered (void* nativeWindow);

// 把"非色键色"的像素合成窗口 region（窗口 region 之外 = 桌面）。
// px 是 B8G8R8A8 交换链像素。Android 没有窗口 region 概念，直接返回 false 并清 active。
bool ApplyChromaRegion (void* nativeWindow, const uint8_t* px, int w, int h,
                        int okR, int okG, int okB, bool* active);

// 清掉窗口 region（如果当前生效）。Android 为空操作。
void ClearChromaRegion (void* nativeWindow, bool* active);

}
