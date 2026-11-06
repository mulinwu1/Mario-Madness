#pragma once

// 强制包含（cl /FI）：让每个翻译单元在解析 native_vulkan 头文件之前就拿到定宽整数类型。
//
// 原因：native_vulkan/gl_vk_bridge.h 在包含 gl_compat.h 之前就用了 uint32_t
// （官方构建里靠前一个头文件先间接引入 <cstdint>，从 gl_fbo.cpp 这种直接首包含
//  gl_vk_bridge.h 的顺序就会编译失败）。工装不改 native 源码，所以在这里补上，
// 避免依赖包含顺序。

#include <cstddef>
#include <cstdint>
