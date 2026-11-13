// 2B：GL 字符串查询的 Vulkan 实现。
//
// 版本字符串是“GLES 兼容契约”，不是 Vulkan apiVersion：
//   - Lime/OpenFL 用 GL.VERSION 判断 OPENGL / OPENGLES 并解析主版本号；
//   - 这里返回 GLES 3.0 兼容串，让旧 exe 不需要改 Haxe 就能选对 renderer；
//   - 真正的 Vulkan apiVersion 只在 [vk] 日志和 VkDeviceCaps 里，不冒充 GL 版本。
//
// 扩展列表本期为空：没有实现的扩展不向外声明。OpenFL 因此不会拿到
// EXT_texture_filter_anisotropic 等对象；对应能力查询返回保守值。

#include "gl_compat.h"
#include "gl_vk_bridge.h"
#include "vk_renderer.h"

#include <cstring>

namespace lime {
namespace {

const char* DeviceName() {
    VkRenderer* renderer = gl_vk_get_renderer();
    if (renderer && renderer->IsReady() && renderer->Caps().deviceName[0]) {
        return renderer->Caps().deviceName;
    }
    return "Lime Vulkan";
}

} // namespace

value lime_gl_get_string (int pname) {
    switch (pname) {
        case 0x1F00:  // GL_VENDOR
            return alloc_string ("Lime Vulkan");
        case 0x1F01:  // GL_RENDERER
            return alloc_string (DeviceName ());
        case 0x1F02:  // GL_VERSION
            // 必须保持桌面 OpenGL 类型：OpenFL 的 AGALConverter 会把含
            // "OpenGL ES"/"WebGL" 的串当成 limited profile，生成 #version 100，
            // 改变 shader 路径并导致颜色/通道异常。这里与隐藏桌面 GL 的类型一致。
            return alloc_string ("OpenGL 3.0 (Lime Vulkan)");
        case 0x1F03:  // GL_EXTENSIONS
            // 只声明翻译层真的支持的扩展。BGRA 上传在 gl_core.cpp 已实现
            // （GL_BGRA_EXT = 0x80E1 会转成 RGBA 再上传）；不声明它，
            // OpenFL 会按 RGBA 解释 BGRA 像素，出现红蓝互换。
            return alloc_string ("GL_EXT_texture_format_BGRA8888 GL_EXT_bgra");
        case 0x8B8C:  // GL_SHADING_LANGUAGE_VERSION
            return alloc_string ("OpenGL GLSL 1.30 (Lime Vulkan)");
        default:
            return alloc_null ();
    }
}

value lime_gl_get_stringi (int pname, int index) {
    if (pname == 0x1F03) {  // GL_EXTENSIONS
        if (index == 0) return alloc_string ("EXT_texture_format_BGRA8888");
        if (index == 1) return alloc_string ("EXT_bgra");
    }
    return alloc_null ();
}

value lime_gl_get_extension (HxString name) {
    (void) name;
    // 没有实现的扩展一律不返回对象，避免上层误用。
    return alloc_null ();
}

} // namespace lime
