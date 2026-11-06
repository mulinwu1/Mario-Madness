#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "vk_renderer.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstdio>
#include <string>

static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || (m == WM_KEYDOWN && w == VK_ESCAPE)) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

static std::string ExeDir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? std::string("") : s.substr(0, pos + 1);
}

int main() {
    WNDCLASS wc{};
    wc.lpfnWndProc = WP;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "VKRendererDemo";
    RegisterClass(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("VKRendererDemo", "Vulkan Renderer Demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    if (!hwnd) { printf("[demo] window failed\n"); return 1; }

    std::string base = ExeDir();
    VkRenderer renderer;
    if (!renderer.Init(hwnd, 1280, 720)) {
        printf("[demo] init failed: %s\n", renderer.LastError());
        return 1;
    }
    printf("[demo] renderer initialized\n");

    const int NTEX = 4;
    uint32_t texIds[NTEX] = {};
    int texW[NTEX] = {}, texH[NTEX] = {};
    size_t texBytesTotal = 0;

    for (int i = 0; i < NTEX; i++) {
        char fn[64];
        sprintf(fn, "texture%d.png", i);
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load((base + fn).c_str(), &w, &h, &c, 4);
        if (!px) { printf("[demo] missing %s\n", fn); return 1; }
        texW[i] = w;
        texH[i] = h;
        texIds[i] = renderer.CreateTexture(w, h, px);
        printf("[demo] tex%d %dx%d -> id=%u\n", i, w, h, texIds[i]);
        stbi_image_free(px);
    }

    MSG msg{};
    bool run = true;
    float t = 0.0f;
    DWORD lastFps = GetTickCount();
    int frames = 0;

    while (run) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { run = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!run) break;

        t += 0.016f;

        renderer.SetUniform(0, (float)texW[0]);
        renderer.SetUniform(1, (float)texH[0]);
        renderer.SetUniform(2, -8.0f);
        renderer.SetUniform(3, -2.0f);
        renderer.SetUniform(4, 0.0f);
        renderer.SetUniform(5, 100.0f / 24.0f);
        renderer.SetUniform(6, 0.5f);
        renderer.SetUniform(7, 1.5f);
        renderer.SetUniform(8, (float)texW[0]);
        renderer.SetUniform(9, (float)texH[0]);

        renderer.BeginFrame(0.03f, 0.04f, 0.06f, 1.0f);

        for (int i = 0; i < 320; i++) {
            int g = i / 80;
            int k = i % 80;
            float px = 0.92f * (float)((k * 13 + g * 7) % 100 - 50) / 50.0f * 0.95f;
            float py = 0.52f * (float)(((k * 17 + (int)(t * 90)) % 100) - 50) / 50.0f;
            float sz = 0.11f + 0.04f * (float)(k % 4);
            renderer.DrawSprite(texIds[g], px, py, sz, sz, 0.0f, 0.0f, 1.0f, 1.0f);
        }

        renderer.EndFrame();

        frames++;
        DWORD now = GetTickCount();
        if (now - lastFps >= 1000) {
            printf("[demo] fps=%d textures=%u\n", frames, renderer.TextureCount());
            fflush(stdout);
            frames = 0;
            lastFps = now;
        }

        Sleep(16);
    }

    renderer.Shutdown();
    printf("[demo] clean exit\n");
    return 0;
}
