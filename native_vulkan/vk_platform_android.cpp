// 平台实现（Android arm64-v8a）。
// 约定：Android 类型只允许出现在本文件；公共头里只出现 void*。
// 由 patch_lime.py 在 LIME_PLATFORM=android 时注入 Build.xml（桌面不编译本文件）。
#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES

#include <android/native_window.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include "vk_platform.h"

#include <cstdint>
#include <cstdio>
#include <ctime>

namespace vkplat {

static const char* kInstanceExtensions[] = {
    "VK_KHR_surface",
    "VK_KHR_android_surface",
};

void* LoadVulkan (char* err, int errCap) {
    void* h = dlopen ("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* d = dlerror ();
        if (err && errCap > 0) {
            snprintf (err, (size_t)errCap, "libvulkan.so load failed: %s", d ? d : "unknown");
        }
        return nullptr;
    }
    return h;
}

void* Proc (void* lib, const char* name) {
    if (!lib || !name) return nullptr;
    return dlsym (lib, name);
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

    auto create = (PFN_vkCreateAndroidSurfaceKHR)
            ((PFN_vkGetInstanceProcAddr)gipa) ((VkInstance)instance, "vkCreateAndroidSurfaceKHR");
    if (!create) {
        if (err && errCap > 0) snprintf (err, (size_t)errCap, "vkCreateAndroidSurfaceKHR missing");
        return false;
    }

    VkAndroidSurfaceCreateInfoKHR sci {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = (ANativeWindow*)nativeWindow;

    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkResult r = create ((VkInstance)instance, &sci, nullptr, &surf);
    if (r != VK_SUCCESS) {
        if (err && errCap > 0) snprintf (err, (size_t)errCap, "vkCreateAndroidSurfaceKHR err=%d", (int)r);
        return false;
    }

    *outSurface = (uint64_t)surf;
    return true;
}

// 安卓恒为空串：进程可执行文件在 /system/bin/app_process*，其目录不是资源目录，
// 拼出的路径必然 fopen 失败（每次多一次无效 IO 与一条误导性失败路径）。
// 调用方拿到空串就应直接走内嵌资源，不做外部文件探测。
void ExeDir (char* out, int outCap) {
    if (!out || outCap <= 0) return;
    out[0] = '\0';
}

static uint64_t MonotonicNs () {
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t TicksMs () {
    return MonotonicNs () / 1000000ull;
}

double NowMs () {
    return (double)MonotonicNs () / 1000000.0;
}

// Android 没有 Win32 的"客户区"概念；surface 尺寸就是全部信息。
bool WindowClientSize (void* nativeWindow, int* outW, int* outH) {
    (void)nativeWindow; (void)outW; (void)outH;
    return false;
}

bool WindowIsLayered (void* nativeWindow) {
    (void)nativeWindow;
    return false;
}

bool ScreenSize (int* outW, int* outH) {
    (void)outW; (void)outH;
    return false;
}

// 窗口 region 色键是 Windows 专有做法；Android 上色键透明不生效（原版也没有）。
bool ApplyChromaRegion (void* nativeWindow, const uint8_t* px, int w, int h,
                        int okR, int okG, int okB, bool* active) {
    (void)nativeWindow; (void)px; (void)w; (void)h; (void)okR; (void)okG; (void)okB;
    if (active) *active = false;
    return false;
}

void ClearChromaRegion (void* nativeWindow, bool* active) {
    (void)nativeWindow;
    if (active) *active = false;
}

}
