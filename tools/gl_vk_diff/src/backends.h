#pragma once
#include <string>

#include "igl.h"

namespace gvd {

// GL/ANGLE 参考后端。angleDir 里需要有 libEGL.dll / libGLESv2.dll（找不到时返回 nullptr）。
IGl* CreateGlBackend(const char* angleDir, std::string& problem);

// Vulkan 后端。
//  mode=0：直接驱动 VkRenderer（验证渲染器本身的像素正确性）
//  mode=1：经 lime_gl_* 翻译层驱动（验证 GL 语义翻译是否到位）
IGl* CreateVkBackend(int mode, int width, int height, std::string& problem);

}  // namespace gvd
