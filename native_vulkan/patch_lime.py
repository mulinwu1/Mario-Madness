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
    # 2a 直连：Haxe 侧跳过 glBindFramebuffer 的通用 GL 状态机，直接把 FBO 名交给渲染器
    "DEFINE_PRIME1 (lime_vk_bind_draw_fbo);",
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
    for lib in ("glslang", "glslang-default-resource-limits"):
        inject += '\t\t<lib name="-l%s" />\n' % lib
    # SPIRV-Tools 只在确实构建出来时才链接：glslang 16.x 不再自带它，
    # 我们的 gl_shader 只调 glslang C 接口（SPIR-V 由自己解析），ENABLE_OPT=OFF 即可。
    for lib in ("SPIRV-Tools", "SPIRV-Tools-opt"):
        if os.path.exists(os.path.join(GLSLANG_ANDROID_DIR, "lib", "lib%s.a" % lib)):
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
            # SDL_syswm.h 在 lime 里只在 HX_WINDOWS 下被包含；Android 也要用
            # SDL_GetWindowWMInfo(info.android.window)，所以这里补一个条件包含。
            s = s.replace(anchor, anchor + '\n#include "../../graphics/vulkan/gl_vk_c.h"\n'
                          '#if defined(HX_ANDROID)\n#include <SDL_syswm.h>\n#endif', 1)
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

        # GetScale() 用 SDL_GL_GetDrawableSize / window.width 算 DPI 缩放。
        # Vulkan 后端下 SDL_GL_GetDrawableSize 返回交换链物理像素（≠ 逻辑宽度），
        # 导致 scale=0.808 → openfl __resize() 的 openfl_dpi_aware 分支把 stageWidth
        # 压成 1034（= 1280*0.808）→ flixel 按 1034 排版 → 菜单挤在左侧。
        # Vulkan 激活时固定返回 1.0，让 stageWidth = window.width。
        scale_anchor = "double SDLWindow::GetScale () {"
        scale_inject = (
            "double SDLWindow::GetScale () {\n\n"
            "\t\tint _dbgW = 0, _dbgH = 0;\n"
            "\t\tint _dbgWW = 0, _dbgWH = 0;\n"
            "\t\tSDL_GL_GetDrawableSize (sdlWindow, &_dbgW, &_dbgH);\n"
            "\t\tSDL_GetWindowSize (sdlWindow, &_dbgWW, &_dbgWH);\n"
            "\t\tfprintf (stderr, \"[scale] drawable=%dx%d window=%dx%d ratio=%.4f\\n\",\n"
            "\t\t\t_dbgW, _dbgH, _dbgWW, _dbgWH, _dbgWW > 0 ? double(_dbgW)/_dbgWW : 0.0);\n"
        )
        if scale_anchor in s:
            s = s.replace(scale_anchor, scale_inject, 1)
        else:
            print("WARN: GetScale anchor not found")

        open(sdl, "w", encoding="utf-8").write(s)
        print("patched SDLWindow.cpp for vulkan")
else:
    print("WARN: SDLWindow.cpp not found")

# 5) 治本：lime_window_get_scale 是 openfl 取 window.scale 的唯一 CFFI 入口。
# Vulkan 后端下 SDL_GL_GetDrawableSize 返回交换链物理像素（≠ 逻辑宽度），
# 导致 GetScale()=0.808 → __scale=0.808 → stageWidth=1034 → 菜单挤左侧。
# 固定返回 1.0，让 stageWidth = window.width = 1280。
extint = os.path.join(LIME, "project", "src", "ExternalInterface.cpp")
if os.path.exists(extint):
    s = open(extint, encoding="utf-8").read()
    if "vulkan_scale_fix" in s:
        print("ExternalInterface.cpp already patched")
    else:
        scale_anchor = "\tdouble lime_window_get_scale (value window) {\n\n\t\tWindow* targetWindow = (Window*)val_data (window);\n\t\treturn targetWindow->GetScale ();"
        scale_fix = "\tdouble lime_window_get_scale (value window) {\n\n\t\t// vulkan_scale_fix: return 1.0 (GetScale uses drawable/window px ratio)\n\t\treturn 1.0;"
        if scale_anchor in s:
            s = s.replace(scale_anchor, scale_fix, 1)
            open(extint, "w", encoding="utf-8").write(s)
            print("patched ExternalInterface.cpp: lime_window_get_scale -> 1.0")
        else:
            print("WARN: lime_window_get_scale anchor not found in ExternalInterface.cpp")

    # 诊断：lime_window_get_width 打印 Window::GetWidth() vs SDL_GetWindowSize
    if "vulkan_width_diag" not in s:
        w_anchor = "\tint lime_window_get_width (value window) {\n\n\t\tWindow* targetWindow = (Window*)val_data (window);\n\t\treturn targetWindow->GetWidth ();"
        w_fix = (
            "\tint lime_window_get_width (value window) {\n\n"
            "\t\tWindow* targetWindow = (Window*)val_data (window);\n"
            "\t\tint w = targetWindow->GetWidth ();\n"
            "\t\tint sw = 0, sh = 0;\n"
            "\t\t// vulkan_width_diag: compare Window::GetWidth vs SDL_GetWindowSize\n"
            "\t\tfprintf (stderr, \"[width] Window::GetWidth()=%d\\n\", w);\n"
            "\t\treturn w;"
        )
        if w_anchor in s:
            s = s.replace(w_anchor, w_fix, 1)
            print("patched ExternalInterface.cpp: lime_window_get_width diagnostic")
        else:
            print("WARN: lime_window_get_width anchor not found")
    else:
        print("ExternalInterface.cpp width diag already present")
    open(extint, "w", encoding="utf-8").write(s)
else:
    print("WARN: ExternalInterface.cpp not found")

# 8) 治本：resize 事件用 SDL_GetWindowSize（逻辑 1280）代替 event->window.data1（drawable 1034）
app = os.path.join(LIME, "project", "src", "backend", "sdl", "SDLApplication.cpp")
if os.path.exists(app):
    s = open(app, encoding="utf-8").read()
    if "vulkan_resize_fix" in s:
        print("SDLApplication.cpp already patched")
    else:
        r_anchor = (
            "\t\t\t\tcase SDL_WINDOWEVENT_SIZE_CHANGED:\n\n"
            "\t\t\t\t\twindowEvent.type = WINDOW_RESIZE;\n"
            "\t\t\t\t\twindowEvent.width = event->window.data1;\n"
            "\t\t\t\t\twindowEvent.height = event->window.data2;\n"
            "\t\t\t\t\tbreak;"
        )
        r_fix = (
            "\t\t\t\tcase SDL_WINDOWEVENT_SIZE_CHANGED:\n\n"
            "\t\t\t\t\twindowEvent.type = WINDOW_RESIZE;\n"
            "\t\t\t\t\t// vulkan_resize_fix: use SDL_GetWindowSize (logical) not event data (drawable px)\n"
            "\t\t\t\t\t{\n"
            "\t\t\t\t\t\tSDL_Window* _sw = SDL_GetWindowFromID (event->window.windowID);\n"
            "\t\t\t\t\t\tint _ww = event->window.data1, _wh = event->window.data2;\n"
            "\t\t\t\t\t\tif (_sw) SDL_GetWindowSize (_sw, &_ww, &_wh);\n"
            "\t\t\t\t\t\tfprintf (stderr, \"[resize] data=%dx%d SDL_GetWindowSize=%dx%d sw=%p\\n\",\n"
            "\t\t\t\t\t\t\tevent->window.data1, event->window.data2, _ww, _wh, (void*)_sw);\n"
            "\t\t\t\t\t\twindowEvent.width = _ww;\n"
            "\t\t\t\t\t\twindowEvent.height = _wh;\n"
            "\t\t\t\t\t}\n"
            "\t\t\t\t\tbreak;"
        )
        if r_anchor in s:
            s = s.replace(r_anchor, r_fix, 1)
            open(app, "w", encoding="utf-8").write(s)
            print("patched SDLApplication.cpp: resize event uses SDL_GetWindowSize")
        else:
            print("WARN: resize anchor not found in SDLApplication.cpp")
else:
    print("WARN: SDLApplication.cpp not found")

# 6) 诊断：GetWidth/GetHeight 打印 SDL_GetWindowSize vs SDL_GL_GetDrawableSize 实际值
if os.path.exists(sdl):
    s = open(sdl, encoding="utf-8").read()
    if "vulkan_size_diag" in s:
        print("SDLWindow.cpp size diag already added")
    else:
        w_anchor = "\tint SDLWindow::GetWidth () {\n\n\t\tint width;\n\t\tint height;\n\n\t\tSDL_GetWindowSize (sdlWindow, &width, &height);\n\n\t\treturn width;"
        w_fix = (
            "\tint SDLWindow::GetWidth () {\n\n"
            "\t\tint width;\n\t\tint height;\n"
            "\t\tint dw = 0, dh = 0;\n"
            "\t\tSDL_GetWindowSize (sdlWindow, &width, &height);\n"
            "\t\tSDL_GL_GetDrawableSize (sdlWindow, &dw, &dh);\n"
            "\t\tfprintf (stderr, \"[size] GetWidth: SDL_GetWindowSize=%dx%d drawable=%dx%d\\n\", width, height, dw, dh);\n"
            "\t\treturn width;"
        )
        if w_anchor in s:
            s = s.replace(w_anchor, w_fix, 1)
            open(sdl, "w", encoding="utf-8").write(s)
            print("patched SDLWindow.cpp: GetWidth size diagnostic")
        else:
            print("WARN: GetWidth anchor not found in SDLWindow.cpp")


# 7) 治本：lime/ui/Window.hx 的 get_width()/get_height() 返回缓存的 __width/__height，
#    而 resize 事件传的是 drawable 尺寸（1034）而非逻辑宽度（1280）。
#    改成每次调 NativeCFFI.lime_window_get_width/height（→ SDL_GetWindowSize → 逻辑尺寸）。
nw = os.path.join(LIME, "src", "lime", "ui", "Window.hx")
if os.path.exists(nw):
    s = open(nw, encoding="utf-8").read()
    if "vulkan_width_live" in s:
        print("Window.hx already patched")
    else:
        w_anchor = "\t@:noCompletion private inline function get_width():Int\n\t{\n\t\treturn __width;\n\t}"
        w_fix = (
            "\t@:noCompletion private inline function get_width():Int\n\t{\n"
            "\t\t// vulkan_width_live: return live SDL size, not cached __width\n"
            "\t\tif (__backend != null && __backend.handle != null)\n"
            "\t\t\treturn @:privateAccess NativeCFFI.lime_window_get_width(__backend.handle);\n"
            "\t\treturn __width;\n\t}"
        )
        h_anchor = "\t@:noCompletion private inline function get_height():Int\n\t{\n\t\treturn __height;\n\t}"
        h_fix = (
            "\t@:noCompletion private inline function get_height():Int\n\t{\n"
            "\t\t// vulkan_width_live: return live SDL size\n"
            "\t\tif (__backend != null && __backend.handle != null)\n"
            "\t\t\treturn @:privateAccess NativeCFFI.lime_window_get_height(__backend.handle);\n"
            "\t\treturn __height;\n\t}"
        )
        # 同时修 resize()：用实时查询覆盖 __width/__height，不信任事件传来的 drawable 尺寸
        r_anchor = "\tpublic function resize(width:Int, height:Int):Void\n\t{\n\t\t__backend.resize(width, height);\n\n\t\t__width = width;\n\t\t__height = height;\n\t}"
        r_fix = (
            "\tpublic function resize(width:Int, height:Int):Void\n\t{\n"
            "\t\t__backend.resize(width, height);\n\n"
            "\t\t// vulkan_width_live: use live SDL size, not event-provided drawable px\n"
            "\t\tif (__backend != null && __backend.handle != null) {\n"
            "\t\t\t__width = @:privateAccess NativeCFFI.lime_window_get_width(__backend.handle);\n"
            "\t\t\t__height = @:privateAccess NativeCFFI.lime_window_get_height(__backend.handle);\n"
            "\t\t} else {\n"
            "\t\t\t__width = width;\n\t\t\t__height = height;\n\t\t}\n\t}"
        )
        if r_anchor in s:
            s = s.replace(r_anchor, r_fix, 1)
            print("patched Window.hx: resize() uses live size")
        else:
            print("WARN: resize anchor not found in Window.hx")

        ok = True
        # 先加 import（如果没有）
        imp_anchor = "import lime.app.Application;"
        imp_add = "import lime.app.Application;\nimport lime._internal.backend.native.NativeCFFI;"
        if "import lime._internal.backend.native.NativeCFFI" not in s:
            if imp_anchor in s:
                s = s.replace(imp_anchor, imp_add, 1)
                print("patched Window.hx: added NativeCFFI import")
            else:
                print("WARN: import anchor not found in Window.hx"); ok = False
        if w_anchor in s:
            s = s.replace(w_anchor, w_fix, 1)
            print("patched Window.hx: get_width -> live query")
        else:
            print("WARN: get_width anchor not found in Window.hx"); ok = False
        if h_anchor in s:
            s = s.replace(h_anchor, h_fix, 1)
            print("patched Window.hx: get_height -> live query")
        else:
            print("WARN: get_height anchor not found in Window.hx"); ok = False
        if ok:
            open(nw, "w", encoding="utf-8").write(s)
        else:
            print("Window.hx NOT patched (anchor mismatch)")
else:
    print("WARN: Window.hx not found at", nw)
