import os
import re
import sys

LIME = os.environ.get("LIME_SRC", r"C:\lime-src")
GLSLANG_ANDROID_DIR = os.environ.get("GLSLANG_ANDROID_DIR", "").strip().replace("\\", "/")


def detect_platform():
    # 判定顺序：显式 LIME_PLATFORM 永远最高优先；缺失时只信 Python 自身运行平台。
    # 不能用 ANDROID_NDK_ROOT / ANDROID_NDK_HOME 推断：GitHub 的 windows-latest
    # 镜像自带这两个变量，会把 Windows 轮误判成 android（run 42395291038 的根因）。
    p = os.environ.get("LIME_PLATFORM", "").strip().lower()
    if p in ("windows", "android"):
        return p
    if p:
        print("ERROR: LIME_PLATFORM=%r (expected windows|android)" % p)
        sys.exit(1)
    return "windows" if sys.platform.startswith("win") else "android"


PLATFORM = detect_platform()
print("[patch_lime] platform=%s" % PLATFORM)


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

# 自动扫描我们自己的 vulkan 源文件，凡是里面定义了同名 lime_gl_*，
# 一律视为“由我们接管”，把 lime 原实现摘掉，避免同一 ndll 里重复定义。
# 这样 FBO / 纹理参数 / 状态机 / shader 各路上线时不需要再手工维护名字清单。
VULKAN_DIR = os.path.join(LIME, "project", "src", "graphics", "vulkan")
DEF_RE = re.compile(
    r"^\s*(?:void|int|bool|float|double|value)\s+(lime_gl_[A-Za-z0-9_]+)\s*\(",
    re.M,
)


def collect_more():
    names = set()
    if not os.path.isdir(VULKAN_DIR):
        return names
    for fn in os.listdir(VULKAN_DIR):
        if not fn.endswith(".cpp"):
            continue
        try:
            src = open(os.path.join(VULKAN_DIR, fn), encoding="utf-8").read()
        except OSError:
            continue
        for m in DEF_RE.finditer(src):
            names.add(m.group(1))
    return names


extra = collect_more()
if extra:
    for n in sorted(extra):
        if n not in CORE:
            CORE.append(n)
    print("auto-collected vulkan entrypoints:", len(extra))

print("total replaced entrypoints:", len(CORE))

# shader 子系统的关键入口必须全部被收集到，否则说明 gl_shader.cpp 没被扫到，
# 或者链接前就被 Build.xml 排除了 —— 让 CI 直接失败比编出一个 no-op 的 ndll 好。
CRITICAL = [
    "lime_gl_create_shader", "lime_gl_shader_source", "lime_gl_compile_shader",
    "lime_gl_create_program", "lime_gl_link_program", "lime_gl_use_program",
    "lime_gl_get_uniform_location", "lime_gl_uniform1f", "lime_gl_uniform4fv",
    "lime_gl_uniform_matrix4fv",
]
missing = [n for n in CRITICAL if n not in CORE]
if missing:
    print("ERROR: shader entrypoints missing from collected set:", missing)
    sys.exit(1)


p = os.path.join(LIME, "project", "src", "graphics", "opengl", "OpenGLBindings.cpp")
text = open(p, encoding="utf-8").read()

# 1) remove the original implementations we replace
removed = 0
RET = r"(?:void|int|bool|float|double|value)"
for name in CORE:
    pat = re.compile(r"\n\t" + RET + r"\s+" + name + r"\s*\([^)]*\)\s*\{.*?\n\t\}\n", re.S)
    new_text, n = pat.subn("\n", text, count=1)
    if n:
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

# 2b) 登记 packet 解码通道的 cpp prim。
# 这些符号不是 lime 原生导出的，OpenGLBindings 的注册表里没有它们；不解 CFFI.load 会失败。
PKT_PRIM_LINES = [
    "DEFINE_PRIME0 (lime_vk_pkt_mode);",
    "DEFINE_PRIME2 (lime_vk_pkt_emit);",
    "DEFINE_PRIME0 (lime_vk_pkt_utag);",
    "DEFINE_PRIME2 (lime_vk_pkt_frame);",
]
if "lime_vk_pkt_mode" not in text:
    prim_anchor = "\tDEFINE_PRIME4v (lime_gl_wait_sync);"
    if prim_anchor in text:
        injected = prim_anchor + "\n" + "".join("\t%s\n" % l for l in PKT_PRIM_LINES)
        text = text.replace(prim_anchor, injected, 1)
        print("registered packet prims:", len(PKT_PRIM_LINES))
    else:
        print("ERROR: DEFINE_PRIME anchor for packet prims not found")
        sys.exit(1)
else:
    print("packet prims already registered")

open(p, "w", encoding="utf-8").write(text)

# 3) Build.xml: 按平台注入源文件；不要加 gl_stubs.cpp（会重复符号）
COMMON_SOURCES = [
    "vk_renderer.cpp",
    "gl_core.cpp",
    "gl_shader.cpp",
    "gl_vk_contract.cpp",
    "gl_fbo.cpp",
    "gl_texture_params.cpp",
    "gl_state.cpp",
    "vk_packet.cpp",
    "gl_packet.cpp",
    "vk_tuning.cpp",
]
PLATFORM_SOURCES = {
    "windows": ["vk_platform_win32.cpp"],
    "android": ["vk_platform_android.cpp"],
}

sources = COMMON_SOURCES + PLATFORM_SOURCES[PLATFORM]


def assert_sources_present():
    # 准备阶段结束后，树里必须真的有这些文件。否则不要把它拖到链接阶段变成
    # 上百行 LNK2019（run 42395291038 就是这样浪费了 87 秒）。
    need = sources + ["gl_compat.h", "vk_packet.h", "gl_packet.h", "vk_tuning.h"]
    missing = [n for n in need if not os.path.exists(os.path.join(VULKAN_DIR, n))]
    if not missing:
        return
    print("ERROR: platform=%s build is missing sources: %s" % (PLATFORM, ", ".join(missing)))
    print("ERROR: actual contents of %s:" % VULKAN_DIR)
    try:
        for name in sorted(os.listdir(VULKAN_DIR)):
            print("  %s" % name)
    except OSError as exc:
        print("  (cannot list: %s)" % exc)
    sys.exit(1)


assert_sources_present()

inject = "".join('\t\t<file name="src/graphics/vulkan/%s" />\n' % name for name in sources)

if PLATFORM == "android":
    # 安卓：glslang 静态链进 liblime.so（桌面的 lime_glslang.dll 分包做法不复制到这里）。
    # GLSLANG_ANDROID_DIR 必须指向一份 arm64-v8a 的 glslang 构建（include/ + lib/）。
    if not GLSLANG_ANDROID_DIR:
        print("ERROR: GLSLANG_ANDROID_DIR not set (android needs static glslang include/ + lib/)")
        sys.exit(1)
    inject += '\t\t<compilerflag value="-I%s/include" />\n' % GLSLANG_ANDROID_DIR
    inject += '\t\t<lib name="-L%s/lib" />\n' % GLSLANG_ANDROID_DIR
    for lib in ("glslang", "glslang-default-resource-limits", "SPIRV-Tools", "SPIRV-Tools-opt"):
        inject += '\t\t<lib name="-l%s" />\n' % lib

b = os.path.join(LIME, "project", "Build.xml")
xml = open(b, encoding="utf-8").read()
anchor = '<file name="src/graphics/Image.cpp" />'
platform_needle = PLATFORM_SOURCES[PLATFORM][0]
if platform_needle in xml:
    # 缓存树可能带着上一轮补丁（源文件清单不全）。这里逐项核对，
    # 缺一个就失败，避免用旧清单编出一个缺符号的 ndll。
    stale = [n for n in sources if ('src/graphics/vulkan/%s"' % n) not in xml]
    if stale:
        print("ERROR: build.xml is patched but stale (platform=%s), missing: %s" % (PLATFORM, ", ".join(stale)))
        print("ERROR: reset the cached lime source and re-run, do not link with an incomplete source list")
        sys.exit(1)
    print("build.xml already patched (platform=%s, %d sources verified)" % (PLATFORM, len(sources)))
elif "vk_renderer.cpp" in xml:
    print("ERROR: build.xml already has vulkan sources but missing %s" % platform_needle)
    sys.exit(1)
elif anchor in xml:
    xml = xml.replace(anchor, anchor + "\n" + inject)
    open(b, "w", encoding="utf-8").write(xml)
    print("patched Build.xml (platform=%s, %d sources)" % (PLATFORM, len(sources)))
else:
    print("ERROR: Build.xml anchor not found")
    sys.exit(1)

print("done")

# 4) 激活 Vulkan：窗口创建后初始化，ContextFlip 里提交
sdl = os.path.join(LIME, "project", "src", "backend", "sdl", "SDLWindow.cpp")
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
        # 窗口句柄获取按平台分支：Windows 取 info.win.window，安卓取 info.android.window，
        # 统一转成 void* 交给 gl_vk_init；公共头里不出现任何平台类型。
        inject_init = (
            "\t\t#if defined(HX_WINDOWS) || defined(HX_ANDROID)\n"
            "\t\t{\n"
            "\t\t\tSDL_SysWMinfo wmInfo;\n"
            "\t\t\tSDL_VERSION (&wmInfo.version);\n"
            "\t\t\tif (sdlWindow && SDL_GetWindowWMInfo (sdlWindow, &wmInfo)) {\n"
            "\t\t\t\t#if defined(HX_WINDOWS)\n"
            "\t\t\t\t\tgl_vk_init ((void*)wmInfo.info.win.window, width, height);\n"
            "\t\t\t\t#elif defined(HX_ANDROID)\n"
            "\t\t\t\t\tgl_vk_init ((void*)wmInfo.info.android.window, width, height);\n"
            "\t\t\t\t#endif\n"
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

