#pragma once

// 纯 C 接口，避免 SDLWindow.cpp 引入 vulkan.h / hxcpp 头
// hwnd 在 Windows 下是 HWND，Android 下换成 ANativeWindow*
void gl_vk_init (void* hwnd, int width, int height);
bool gl_vk_active ();
void gl_vk_end_frame ();
void gl_vk_shutdown ();
