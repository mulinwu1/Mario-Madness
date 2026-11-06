// native_vulkan/gl_fbo.cpp 的编译垫片。
//
// 上游缺陷：gl_fbo.cpp 在 EnsureFboTarget() 里引用了 g_rendererExt，但这个变量在
// gl_vk_contract.cpp 里是文件级 static（外部不可见），gl_fbo.cpp 自身也没有声明它，
// 因此该文件单独编译必然失败（C2065）。
//
// 边界要求不改 native_vulkan 源码，所以这里在 lime 的匿名命名空间里补一个定义，
// 再把原文件原样文本包含进来。取值为空指针 → gl_fbo 走「没有 Ext 就退回
// CreateRenderTarget」的降级分支，正是工装需要的路径。
//
// 说明：同一 TU 内的匿名命名空间是同一个命名空间，因此下面这个名字对
// gl_fbo.cpp 里的引用可见。

#include "gl_vk_bridge.h"
#include "gl_vk_contract.h"
#include "vk_renderer.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace lime {
namespace {
const GlVkRendererExt* g_rendererExt = nullptr;
}
}

#include "../../../native_vulkan/gl_fbo.cpp"
