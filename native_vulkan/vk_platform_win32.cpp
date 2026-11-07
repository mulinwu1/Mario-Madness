// 平台实现（桌面 Windows）。
// 约定：Win32 符号只允许出现在本文件；vk_renderer.h / vk_platform.h 里不得出现
// windows.h、HWND、HINSTANCE、LoadLibraryA、GetProcAddress、GetModuleHandle。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES

#include <windows.h>
#include <vulkan/vulkan.h>

#include "vk_platform.h"

#include <cstdint>
#include <cstdio>

namespace vkplat {

static const char* kInstanceExtensions[] = {
    "VK_KHR_surface",
    "VK_KHR_win32_surface",
};

void* LoadVulkan (char* err, int errCap) {
    HMODULE h = LoadLibraryA ("vulkan-1.dll");
    if (!h) {
        if (err && errCap > 0) snprintf (err, (size_t)errCap, "vulkan-1.dll missing");
        return nullptr;
    }
    return (void*)h;
}

void* Proc (void* lib, const char* name) {
    if (!lib || !name) return nullptr;
    return (void*)GetProcAddress ((HMODULE)lib, name);
}

const char* const* InstanceExtensions (uint32_t* count) {
    if (count) *count = (uint32_t)(sizeof (kInstanceExtensions) / sizeof (kInstanceExtensions[0]));
    return kInstanceExtensions;
}

bool CreateSurface (uint64_t instance, void* gipa, void* nativeWindow,
                    uint64_t* outSurface, char* err, int errCap) {
    if (!gipa || !nativeWindow || !outSurface) {
        if (err && errCap > 0) snprintf (err, (size_t)errCap, "invalid surface args");
        return false;
    }

    auto create = (PFN_vkCreateWin32SurfaceKHR)
            ((PFN_vkGetInstanceProcAddr)gipa) ((VkInstance)instance, "vkCreateWin32SurfaceKHR");
    if (!create) {
        if (err && errCap > 0) snprintf (err, (size_t)errCap, "vkCreateWin32SurfaceKHR missing");
        return false;
    }

    VkWin32SurfaceCreateInfoKHR sci {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandle (nullptr);
    sci.hwnd = (HWND)nativeWindow;

    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkResult r = create ((VkInstance)instance, &sci, nullptr, &surf);
    if (r != VK_SUCCESS) {
        if (err && errCap > 0) snprintf (err, (size_t)errCap, "vkCreateWin32SurfaceKHR err=%d", (int)r);
        return false;
    }

    *outSurface = (uint64_t)surf;
    return true;
}

// ExeDirLocal 的实现从 vk_renderer.cpp 搬到这里：共用文件里不能再出现 MAX_PATH /
// GetModuleFileNameA（CI run 42395291038 的 C2065/C3861 就是这么来的）。
void ExeDir (char* out, int outCap) {
    if (!out || outCap <= 0) return;
    out[0] = '\0';

    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA (nullptr, p, MAX_PATH);
    // 返回 MAX_PATH 表示缓冲被截断，此时路径不可靠，直接当作取不到
    if (n == 0 || n >= (DWORD)MAX_PATH) return;

    int cut = -1;
    for (int i = (int)n - 1; i >= 0; --i) {
        if (p[i] == '\\' || p[i] == '/') { cut = i; break; }
    }
    if (cut < 0) return;

    int len = cut + 1;
    if (len > outCap - 1) len = outCap - 1;
    for (int i = 0; i < len; ++i) out[i] = p[i];
    out[len] = '\0';
}

}
