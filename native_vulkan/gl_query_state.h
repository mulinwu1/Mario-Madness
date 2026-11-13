#pragma once

namespace lime {

// gl_query 使用的只读状态 getter。
// 实现分散在 gl_core.cpp / gl_fbo.cpp / gl_texture_params.cpp，避免 gl_query
// 直接依赖这些模块的内部 static 变量。
int gl_vk_query_active_texture();
int gl_vk_query_bound_texture_2d();
int gl_vk_query_bound_framebuffer();
int gl_vk_query_bound_read_framebuffer();
int gl_vk_query_bound_renderbuffer();
int gl_vk_query_current_program();
int gl_vk_query_bound_array_buffer();
int gl_vk_query_bound_element_buffer();

// pname 属于 pixelStore 时返回 true 并写 value；否则返回 false。
bool gl_vk_query_pixel_store(int pname, int* value);

} // namespace lime
