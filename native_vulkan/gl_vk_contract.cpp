#include "gl_vk_contract.h"

namespace lime {

static const GlVkCoreState* g_coreState = nullptr;
static const GlVkRendererExt* g_rendererExt = nullptr;

void gl_vk_install_core_state (const GlVkCoreState* state) { g_coreState = state; }
const GlVkCoreState* gl_vk_core_state () { return g_coreState; }

void gl_vk_install_renderer_ext (const GlVkRendererExt* ext) { g_rendererExt = ext; }
const GlVkRendererExt* gl_vk_renderer_ext () { return g_rendererExt; }

} // namespace lime
