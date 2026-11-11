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
#include <cstring>
#include <vector>

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

uint64_t TicksMs () {
    return (uint64_t)GetTickCount64 ();
}

double NowMs () {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency (&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter (&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

bool WindowClientSize (void* nativeWindow, int* outW, int* outH) {
    if (!nativeWindow) return false;
    RECT rc;
    if (!GetClientRect ((HWND)nativeWindow, &rc)) return false;
    if (outW) *outW = (int)(rc.right - rc.left);
    if (outH) *outH = (int)(rc.bottom - rc.top);
    return true;
}

bool WindowIsLayered (void* nativeWindow) {
    if (!nativeWindow) return false;
    return (GetWindowLongA ((HWND)nativeWindow, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
}

void ClearChromaRegion (void* nativeWindow, bool* active) {
    if (active && *active && nativeWindow) {
        SetWindowRgn ((HWND)nativeWindow, nullptr, TRUE);
    }
    if (active) *active = false;
}

// 把“非色键色”的像素合成窗口 region（窗口 region 之外 = 桌面）。
// px 是 B8G8R8A8 交换链像素；色键色默认 RGB(87,25,0)（游戏 setTransparency 的 6487）。
// DXGI flip 模型不支持 SetLayeredWindowAttributes 色键，用这个办法等效实现。
bool ApplyChromaRegion (void* nativeWindow, const uint8_t* px, int w, int h,
                        int okR, int okG, int okB, bool* active) {
    if (!nativeWindow || !px || w <= 0 || h <= 0) return false;
    HWND hwnd = (HWND)nativeWindow;
    RECT wr, cr;
    if (!GetWindowRect (hwnd, &wr) || !GetClientRect (hwnd, &cr)) return false;
    POINT pt{0, 0};
    ClientToScreen (hwnd, &pt);
    const int offX = pt.x - wr.left;
    const int offY = pt.y - wr.top;

    std::vector<RECT> rects;
    rects.reserve ((size_t)h * 2);
    size_t chroma = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = px + (size_t)y * (size_t)w * 4;
        int x = 0;
        while (x < w) {
            const uint8_t* p = row + (size_t)x * 4;
            if (p[2] == okR && p[1] == okG && p[0] == okB) { ++x; ++chroma; continue; }
            const int x0 = x;
            while (x < w) {
                p = row + (size_t)x * 4;
                if (p[2] == okR && p[1] == okG && p[0] == okB) break;
                ++x;
            }
            RECT r{offX + x0, offY + y, offX + x, offY + y + 1};
            rects.push_back (r);
        }
    }
    const double ratio = (double)chroma / ((double)w * (double)h);
    if (ratio < 0.02 || rects.empty () || rects.size () > 6000) {
        ClearChromaRegion (nativeWindow, active);
        return false;
    }
    const size_t bytes = sizeof (RGNDATAHEADER) + rects.size () * sizeof (RECT);
    std::vector<uint8_t> buf (bytes);
    RGNDATA* rd = (RGNDATA*)buf.data ();
    rd->rdh.dwSize = sizeof (RGNDATAHEADER);
    rd->rdh.iType = RDH_RECTANGLES;
    rd->rdh.nCount = (DWORD)rects.size ();
    rd->rdh.nRgnSize = (DWORD)(rects.size () * sizeof (RECT));
    rd->rdh.rcBound.left = 0;
    rd->rdh.rcBound.top = 0;
    rd->rdh.rcBound.right = w + offX;
    rd->rdh.rcBound.bottom = h + offY;
    memcpy (rd->Buffer, rects.data (), rects.size () * sizeof (RECT));
    HRGN rgn = ExtCreateRegion (nullptr, (DWORD)bytes, rd);
    if (!rgn) return false;
    SetWindowRgn (hwnd, rgn, TRUE);   // 成功后系统接管 region，不能 DeleteObject
    if (active) *active = true;
    static int s_logged = 0;
    if (s_logged < 10) {
        s_logged++;
        fprintf (stderr, "[vk] colorkey region rects=%zu chroma=%.1f%% size=%dx%d\n",
                 rects.size (), ratio * 100.0, w, h);
    }
    return true;
}

}
