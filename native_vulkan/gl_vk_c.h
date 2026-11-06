#pragma once

// 纯 C 接口，避免 SDLWindow.cpp 引入 vulkan.h / hxcpp 头
// hwnd 在 Windows 下是 HWND，Android 下换成 ANativeWindow*
// 必须和 gl_core.cpp 的 namespace lime 保持一致，否则链接时符号不匹配
namespace lime {

void gl_vk_init (void* hwnd, int width, int height);
bool gl_vk_active ();
void gl_vk_end_frame ();
void gl_vk_shutdown ();

}
