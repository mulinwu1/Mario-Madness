#pragma once

#include <cstddef>
#include <cstdint>

// 最小 CFFI 垫片。native_vulkan/gl_compat.h 依赖 Lime/Haxe 运行时的 <system/CFFI.h>；
// 工装只需要「声明能通过编译」，真正用到 value/HxString 的入口都在 gl_stubs.cpp 里，
// 而 gl_stubs.cpp 不在本工装的编译集合内（与 .github/workflows 的官方清单一致）。
// 因此这里只给最小定义，不引入任何运行时依赖。

struct HxString {
    const char* ptr;
    HxString() : ptr(nullptr) {}
    HxString(const char* p) : ptr(p) {}
    const char* c_str() const { return ptr; }
};

typedef void* value;
