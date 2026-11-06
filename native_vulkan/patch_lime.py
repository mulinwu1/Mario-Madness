import os
import re
import sys

LIME = os.environ.get("LIME_SRC", r"C:\lime-src")

CORE = [
    "lime_gl_clear", "lime_gl_clear_color", "lime_gl_viewport",
    "lime_gl_enable", "lime_gl_disable", "lime_gl_blend_func",
    "lime_gl_color_mask", "lime_gl_scissor",
    "lime_gl_create_texture", "lime_gl_bind_texture", "lime_gl_delete_texture",
    "lime_gl_tex_image_2d", "lime_gl_active_texture",
    "lime_gl_get_error",
    "lime_gl_create_buffer", "lime_gl_bind_buffer", "lime_gl_buffer_data",
    "lime_gl_buffer_sub_data", "lime_gl_delete_buffer",
    "lime_gl_vertex_attrib_pointer", "lime_gl_enable_vertex_attrib_array",
    "lime_gl_disable_vertex_attrib_array", "lime_gl_draw_arrays",
    "lime_gl_draw_elements",
]

p = os.path.join(LIME, "project", "src", "graphics", "opengl", "OpenGLBindings.cpp")
text = open(p, encoding="utf-8").read()

# 1) remove the original implementations we replace
removed = 0
for name in CORE:
    pat = re.compile(r"\n\tvoid\s+" + name + r"\s*\([^)]*\)\s*\{.*?\n\t\}\n", re.S)
    new_text, n = pat.subn("\n", text, count=1)
    if n:
        removed += 1
        text = new_text
    else:
        # try int-returning variant
        pat2 = re.compile(r"\n\tint\s+" + name + r"\s*\([^)]*\)\s*\{.*?\n\t\}\n", re.S)
        new_text, n2 = pat2.subn("\n", text, count=1)
        if n2:
            removed += 1
            text = new_text
        else:
            print("WARN: could not remove", name)
print("removed implementations:", removed, "/", len(CORE))

# 2) add include for our declarations so DEFINE_PRIME sees them
inc = '#include "OpenGLBindings.h"'
if '#include "../vulkan/gl_compat.h"' not in text:
    if inc in text:
        text = text.replace(inc, inc + '\n#include "../vulkan/gl_compat.h"', 1)
        print("added vulkan include")
    else:
        print("WARN: OpenGLBindings.h include anchor not found")

open(p, "w", encoding="utf-8").write(text)

# 3) Build.xml: add vk sources, do NOT add gl_stubs.cpp (it would duplicate symbols)
b = os.path.join(LIME, "project", "Build.xml")
xml = open(b, encoding="utf-8").read()
inject = """		<file name="src/graphics/vulkan/vk_renderer.cpp" />
		<file name="src/graphics/vulkan/gl_core.cpp" />
"""
anchor = '<file name="src/graphics/Image.cpp" />'
if "vk_renderer.cpp" in xml:
    print("build.xml already patched")
elif anchor in xml:
    xml = xml.replace(anchor, anchor + "\n" + inject)
    open(b, "w", encoding="utf-8").write(xml)
    print("patched Build.xml")
else:
    print("ERROR: Build.xml anchor not found")
    sys.exit(1)

print("done")

# 4) 激活 Vulkan：窗口创建后初始化，ContextFlip 里提交
sdl = os.path.join(LIME, "project", "src", "backends", "sdl", "SDLWindow.cpp")
if os.path.exists(sdl):
    s = open(sdl, encoding="utf-8").read()
    if "gl_vk_c.h" in s:
        print("SDLWindow.cpp already patched")
    else:
        anchor = '#include "../../graphics/opengl/OpenGLBindings.h"'
        if anchor in s:
            s = s.replace(anchor, anchor + '\n#include "../../graphics/vulkan/gl_vk_c.h"', 1)
        else:
            print("WARN: SDLWindow include anchor not found")

        create_anchor = "\t\tif (!sdlWindow) {"
        inject_init = (
            "\t\t#ifdef HX_WINDOWS\n"
            "\t\t{\n"
            "\t\t\tSDL_SysWMinfo wmInfo;\n"
            "\t\t\tSDL_VERSION (&wmInfo.version);\n"
            "\t\t\tif (sdlWindow && SDL_GetWindowWMInfo (sdlWindow, &wmInfo)) {\n"
            "\t\t\t\tgl_vk_init ((void*)wmInfo.info.win.window, width, height);\n"
            "\t\t\t}\n"
            "\t\t}\n"
            "\t\t#endif\n\n"
        )
        if create_anchor in s:
            s = s.replace(create_anchor, inject_init + create_anchor, 1)
        else:
            print("WARN: SDLWindow create anchor not found")

        flip_anchor = "void SDLWindow::ContextFlip () {"
        inject_flip = (
            "void SDLWindow::ContextFlip () {\n\n"
            "\t\tif (gl_vk_active ()) {\n\n"
            "\t\t\tgl_vk_end_frame ();\n"
            "\t\t\treturn;\n\n"
            "\t\t}\n"
        )
        if flip_anchor in s:
            s = s.replace(flip_anchor, inject_flip, 1)
        else:
            print("WARN: ContextFlip anchor not found")

        open(sdl, "w", encoding="utf-8").write(s)
        print("patched SDLWindow.cpp for vulkan")
else:
    print("WARN: SDLWindow.cpp not found")

