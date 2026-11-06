#pragma once

class VkRenderer;

namespace lime {

void gl_vk_attach_renderer(VkRenderer* r);
VkRenderer* gl_vk_get_renderer();

// 帧目标对接（gl_fbo.cpp 提供，gl_core 的 clear / 帧末 present 走这里）
bool gl_vk_handle_clear();
void gl_vk_present_frame();
uint32_t gl_vk_current_target();

// 纹理查询（gl_texture_params.cpp 提供）
uint32_t gl_vk_texture_id(int glName);
bool gl_vk_texture_size(int glName, int* width, int* height);

}

#include "gl_compat.h"
