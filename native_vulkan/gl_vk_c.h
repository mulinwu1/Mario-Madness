#pragma once

// 纯 C 接口，避免 SDLWindow.cpp 引入 vulkan.h / hxcpp 头
// nativeWindow 为原生窗口句柄，各平台统一按 void* 传递，本层不区分平台
// 必须和 gl_core.cpp 的 namespace lime 保持一致，否则链接时符号不匹配
namespace lime {

void gl_vk_init (void* nativeWindow, int width, int height);
bool gl_vk_active ();
void gl_vk_end_frame ();
void gl_vk_shutdown ();

}
