#include "gl_compat.h"
#include "vk_renderer.h"

namespace lime {

static VkRenderer* g_renderer = nullptr;
static float g_clearR = 0.0f, g_clearG = 0.0f, g_clearB = 0.0f, g_clearA = 1.0f;
static int g_vpX = 0, g_vpY = 0, g_vpW = 0, g_vpH = 0;

// bridge hooks (declared in gl_vk_bridge.h)
void gl_vk_attach_renderer(VkRenderer* r) { g_renderer = r; }
VkRenderer* gl_vk_get_renderer() { return g_renderer; }

void lime_gl_clear_color(float red, float green, float blue, float alpha) {
    g_clearR = red;
    g_clearG = green;
    g_clearB = blue;
    g_clearA = alpha;
}

void lime_gl_clear(int mask) {
    if (!g_renderer) return;
    g_renderer->BeginFrame(g_clearR, g_clearG, g_clearB, g_clearA);
    g_renderer->EndFrame();
}

void lime_gl_viewport(int x, int y, int width, int height) {
    g_vpX = x;
    g_vpY = y;
    g_vpW = width;
    g_vpH = height;
}

} // namespace lime
