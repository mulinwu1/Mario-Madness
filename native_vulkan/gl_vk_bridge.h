#pragma once

class VkRenderer;

namespace lime {

void gl_vk_attach_renderer(VkRenderer* r);
VkRenderer* gl_vk_get_renderer();

}

#include "gl_compat.h"
