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

# 与 Build.xml 注入共用同一份清单；gl_stubs.cpp 明确不编译，也不能提供接管依据。
COMMON_SOURCES = [
    "vk_renderer.cpp",
    "gl_core.cpp",
    "gl_shader.cpp",
    "gl_vk_contract.cpp",
    "gl_fbo.cpp",
    "gl_texture_params.cpp",
    "gl_state.cpp",
    "gl_query.cpp",
    "gl_string.cpp",
    "vk_packet.cpp",
    "gl_packet.cpp",
    "vk_tuning.cpp",
]
PLATFORM_SOURCES = {
    "windows": ["vk_platform_win32.cpp"],
    "android": ["vk_platform_android.cpp"],
}
sources = COMMON_SOURCES + PLATFORM_SOURCES[PLATFORM]
VULKAN_DIR = os.path.join(LIME, "project", "src", "graphics", "vulkan")


def _gl_cpp_code(text):
    # 屏蔽注释/字符串但保留位置与换行；括号、花括号不能由诊断字符串提供。
    tokens = re.compile(
        r'//[^\n]*|/\*[\s\S]*?\*/|'
        r'R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)"|'
        r'"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\''
    )
    return tokens.sub(lambda m: re.sub(r"[^\r\n]", " ", m.group()), text)


def _gl_closing(code, start, opening, closing, label):
    depth = 0
    for pos in range(start, len(code)):
        if code[pos] == opening:
            depth += 1
        elif code[pos] == closing:
            depth -= 1
            if depth == 0:
                return pos
    raise ValueError("%s: unclosed %s" % (label, opening))


def _gl_function_entries(text, label):
    # 针对这里的 CFFI 签名，不是 C++ 预处理器；编译源选择由 sources 决定。
    # 直接匹配 lime_gl_ 名字，不会把 HL_NAME(hl_gl_...) 的旧实现当作 CFFI。
    code = _gl_cpp_code(text)
    signature = re.compile(
        r"\b(?:void|int|bool|float|double|value|HxString)\s+"
        r"(lime_gl_[A-Za-z0-9_]+)\s*\("
    )
    entries = {}
    for match in signature.finditer(code):
        name = match.group(1)
        end = _gl_closing(code, match.end() - 1, "(", ")", label + " " + name) + 1
        while end < len(code) and code[end].isspace():
            end += 1
        if end < len(code) and code[end] in "{;":
            kind = "definition" if code[end] == "{" else "declaration"
            if kind == "definition":
                end = _gl_closing(code, end, "{", "}", label + " " + name)
            entries.setdefault(name, []).append((match.start(), end + 1, kind))
    return entries


def collect_more(directory, source_names):
    definitions = {}
    for source_name in source_names:
        path = os.path.join(directory, source_name)
        try:
            with open(path, "rb") as source:
                text = source.read().decode("utf-8")
        except OSError as exc:
            raise ValueError("GL collection: missing/unreadable source %s: %s" % (path, exc)) from exc
        for name, entries in _gl_function_entries(text, path).items():
            for start, end, kind in entries:
                if kind != "definition":
                    continue
                if name in definitions:
                    raise ValueError("GL collection: duplicate definition %s in %s and %s"
                                     % (name, definitions[name], path))
                definitions[name] = path
    return set(definitions)


extra = collect_more(VULKAN_DIR, sources)
missing_core = [name for name in CORE if name not in extra]
if missing_core:
    raise ValueError("GL collection: required compiled definitions missing: " + ", ".join(missing_core))
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


def _gl_normalize_newlines(text, label):
    crlf = text.count("\r\n")
    if "\r" in text.replace("\r\n", "") or (crlf and crlf != text.count("\n")):
        raise ValueError("OpenGLBindings %s: unsupported mixed newlines" % label)
    return text.replace("\r\n", "\n"), "\r\n" if crlf else "\n"


def _gl_include_entries(text, header):
    code = _gl_cpp_code(text)
    pattern = re.compile(r'^[ \t\ufeff]*#[ \t]*include[ \t]+"' + re.escape(header) + r'"[^\n]*', re.M)
    return [m for m in pattern.finditer(text) if "#" in code[m.start():m.end()]]


def _gl_prime_entries(text):
    pattern = re.compile(
        r"\b(DEFINE_PRIME[0-9]+v?)\s*\(\s*(lime_[A-Za-z0-9_]+)\s*\)\s*;"
    )
    entries = {}
    for match in pattern.finditer(_gl_cpp_code(text)):
        entries.setdefault(match.group(2), []).append(match)
    return entries


def patch_gl_bindings(text, declarations, names, packet_prim_lines):
    # 纯转换：全部核查通过才返回；调用者在此之后才允许写 OpenGLBindings。
    text, newline = _gl_normalize_newlines(text, "cpp")
    declarations, _ = _gl_normalize_newlines(declarations, "gl_compat.h")
    names = tuple(names)
    if len(names) != len(set(names)):
        raise ValueError("OpenGLBindings: duplicate takeover name")
    includes = _gl_include_entries(text, "OpenGLBindings.h")
    compat = _gl_include_entries(text, "../vulkan/gl_compat.h")
    if len(includes) != 1:
        raise ValueError("OpenGLBindings: include anchor OpenGLBindings.h expected once (found %d)" % len(includes))
    if len(compat) > 1:
        raise ValueError("OpenGLBindings: duplicate gl_compat.h include anchor")
    functions = _gl_function_entries(text, "OpenGLBindings")
    header = _gl_function_entries(declarations, "gl_compat.h")
    primes = _gl_prime_entries(text)
    removals = []
    for name in names:
        definitions = [entry for entry in functions.get(name, ()) if entry[2] == "definition"]
        if len(definitions) > 1:
            raise ValueError("OpenGLBindings: duplicate CFFI definition " + name)
        declared = [entry for entry in header.get(name, ()) if entry[2] == "declaration"]
        if len(declared) != 1:
            raise ValueError("OpenGLBindings: gl_compat.h declaration expected once for " + name)
        if len(primes.get(name, ())) != 1:
            raise ValueError("OpenGLBindings: PRIME registration expected once for " + name)
        if definitions:
            removals.append(definitions[0][:2])
        elif not compat:
            raise ValueError("OpenGLBindings: missing original CFFI definition " + name)
        # include + 头文件声明 + PRIME 只能证明结构兼容，不能证明删除来自本脚本。
        # 不制造 provenance 标记；已补丁/部分补丁输入仅在上述结构完整时接受缺失。

    # 2b 同样先核查注册锚点；不因函数调用、注释中的 packet 名字跳过注册。
    wait = primes.get("lime_gl_wait_sync", ())
    if len(wait) != 1 or wait[0].group(1) != "DEFINE_PRIME4v":
        raise ValueError("OpenGLBindings: packet PRIME anchor lime_gl_wait_sync expected once")
    packet_names = []
    for line in packet_prim_lines:
        entry = _gl_prime_entries(line)
        if len(entry) != 1:
            raise ValueError("OpenGLBindings: invalid packet PRIME line " + line)
        name, matches = next(iter(entry.items()))
        packet_names.append(name)
        registered = primes.get(name, ())
        if len(registered) > 1 or (registered and registered[0].group(1) != matches[0].group(1)):
            raise ValueError("OpenGLBindings: duplicate/invalid packet PRIME registration " + name)
    packet_counts = [len(primes.get(name, ())) for name in packet_names]
    if any(packet_counts) and not all(packet_counts):
        raise ValueError("OpenGLBindings: incomplete packet PRIME registrations")

    edits = [(start, end, "") for start, end in removals]
    if not compat:
        pos = includes[0].end()
        edits.append((pos, pos, '\n#include "../vulkan/gl_compat.h"'))
    if not any(packet_counts):
        pos = wait[0].end()
        edits.append((pos, pos, "\n" + "".join("\t%s\n" % line for line in packet_prim_lines)))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text.replace("\n", newline)


def patch_gl_bindings_files(bindings_path, declarations_path, names, packet_prim_lines,
                            query_shims=False):
    with open(bindings_path, "rb") as source:
        before = source.read()
    with open(declarations_path, "rb") as source:
        declarations = source.read()
    text = patch_gl_bindings(before.decode("utf-8"), declarations.decode("utf-8"),
                             names, packet_prim_lines)
    # 2A：主流程显式开启六个查询入口的 shim；若调用方只做旧阶段转换，
    # 保持默认 False，不把六个 glGet body 提前换掉。
    if query_shims:
        text = patch_gl_query_shims(text)
    after = text.encode("utf-8")
    if before != after:
        with open(bindings_path, "wb") as target:
            target.write(after)


# 2A：六个 scalar/vector 查询入口改成原生 gl_query，不再调用 glGet*。
# 纯文本转换：不读写文件，调用方负责备份/写入。
GL_QUERY_SHIM_NAMES = (
    "lime_gl_get_boolean",
    "lime_gl_get_booleanv",
    "lime_gl_get_float",
    "lime_gl_get_floatv",
    "lime_gl_get_integer",
    "lime_gl_get_integerv",
)
GL_QUERY_SHIM_MARKER = "// STAGE2A_GLQUERY_SHIM v1"


def _gl_query_shim_body(name):
    # 生成到 namespace lime 内部，glquery::* 可直接解析。
    # 注意：向量入口的 maxCount 是 16，但 native 只按 pname 的真实分量数写，
    # 调用方缓冲区按 GL 语义至少是真实分量数。
    bodies = {
        "lime_gl_get_boolean": """// STAGE2A_GLQUERY_SHIM v1
bool lime_gl_get_boolean (int pname) {
    unsigned char out[4] = {0, 0, 0, 0};
    int n = glquery::QueryBooleanv (pname, out, 4);
    if (n < 0) { glquery::Unhandled (pname); return false; }
    return n > 0 && out[0] != 0;
}""",
        "lime_gl_get_booleanv": """// STAGE2A_GLQUERY_SHIM v1
void lime_gl_get_booleanv (int pname, double params) {
    unsigned char* out = (unsigned char*)(uintptr_t)params;
    if (!out) { glquery::Unhandled (pname); return; }
    int n = glquery::QueryBooleanv (pname, out, 16);
    if (n < 0) glquery::Unhandled (pname);
}""",
        "lime_gl_get_float": """// STAGE2A_GLQUERY_SHIM v1
float lime_gl_get_float (int pname) {
    float out[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int n = glquery::QueryFloat (pname, out, 4);
    if (n < 0) { glquery::Unhandled (pname); return 0.0f; }
    return n > 0 ? out[0] : 0.0f;
}""",
        "lime_gl_get_floatv": """// STAGE2A_GLQUERY_SHIM v1
void lime_gl_get_floatv (int pname, double params) {
    float* out = (float*)(uintptr_t)params;
    if (!out) { glquery::Unhandled (pname); return; }
    int n = glquery::QueryFloat (pname, out, 16);
    if (n < 0) glquery::Unhandled (pname);
}""",
        "lime_gl_get_integer": """// STAGE2A_GLQUERY_SHIM v1
int lime_gl_get_integer (int pname) {
    int out[4] = {0, 0, 0, 0};
    int n = glquery::QueryInteger (pname, out, 4);
    if (n < 0) { glquery::Unhandled (pname); return 0; }
    return n > 0 ? out[0] : 0;
}""",
        "lime_gl_get_integerv": """// STAGE2A_GLQUERY_SHIM v1
void lime_gl_get_integerv (int pname, double params) {
    int* out = (int*)(uintptr_t)params;
    if (!out) { glquery::Unhandled (pname); return; }
    int n = glquery::QueryInteger (pname, out, 16);
    if (n < 0) glquery::Unhandled (pname);
}""",
    }
    return bodies[name]


def patch_gl_query_shims(text):
    # 纯转换：全部核查通过才返回；调用方负责备份/写文件。
    text, newline = _gl_normalize_newlines(text, "OpenGLBindings")
    marker_count = text.count(GL_QUERY_SHIM_MARKER)
    if marker_count == len(GL_QUERY_SHIM_NAMES):
        return text.replace("\n", newline)
    if marker_count:
        raise ValueError("OpenGLBindings: partial glquery shim markers")

    compat = _gl_include_entries(text, "../vulkan/gl_compat.h")
    if len(compat) != 1:
        raise ValueError("OpenGLBindings: gl_compat.h include expected once (found %d)" % len(compat))
    query_include = _gl_include_entries(text, "../vulkan/gl_query.h")
    if len(query_include) > 1:
        raise ValueError("OpenGLBindings: duplicate gl_query.h include")

    functions = _gl_function_entries(text, "OpenGLBindings")
    edits = []
    for name in GL_QUERY_SHIM_NAMES:
        definitions = [entry for entry in functions.get(name, ()) if entry[2] == "definition"]
        if len(definitions) != 1:
            raise ValueError("OpenGLBindings: CFFI definition expected once for " + name)
        start, end, _ = definitions[0]
        edits.append((start, end, _gl_query_shim_body(name)))
    if not query_include:
        pos = compat[0].end()
        edits.append((pos, pos, '\n#include "../vulkan/gl_query.h"'))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return text.replace("\n", newline)


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
bindings = os.path.join(LIME, "project", "src", "graphics", "opengl", "OpenGLBindings.cpp")
declarations = os.path.join(VULKAN_DIR, "gl_compat.h")
patch_gl_bindings_files(bindings, declarations, CORE, PKT_PRIM_LINES, query_shims=True)
print("OpenGLBindings CFFI takeovers verified:", len(CORE))

# 3) Build.xml: 按平台注入源文件；不要加 gl_stubs.cpp（会重复符号）


def assert_sources_present():
    # 准备阶段结束后，树里必须真的有这些文件。否则不要把它拖到链接阶段变成
    # 上百行 LNK2019（run 42395291038 就是这样浪费了 87 秒）。
    need = sources + ["gl_compat.h", "vk_packet.h", "gl_packet.h", "vk_tuning.h",
                      "vk_device_caps.h", "gl_query.h", "gl_query_state.h"]
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

# 4) Vulkan 主窗与 Windows 隐藏 GL 兼容窗；只支持原始源、旧脚本输出及本版输出。
def _sdl_replace(text, old, new, label):
    # 新片段通常包含旧锚点：扣除完整新片段后再计数，重跑也必须逐项校验。
    new_count = text.count(new)
    old_count = text.replace(new, "").count(old)
    if (old_count, new_count) == (0, 1):
        return text
    if (old_count, new_count) != (1, 0):
        raise ValueError("SDLWindow %s: expected one old or patched anchor (old=%d, patched=%d)"
                         % (label, old_count, new_count))
    return text.replace(old, new, 1)


def _sdl_normalize_newlines(text, label):
    crlf = text.count("\r\n")
    if "\r" in text.replace("\r\n", "") or (crlf and crlf != text.count("\n")):
        raise ValueError("SDLWindow %s: unsupported mixed newlines" % label)
    return text.replace("\r\n", "\n"), "\r\n" if crlf else "\n"


def patch_sdl_window_sources(cpp, header):
    # 纯字符串转换；UTF-8 BOM 作为字符原样保留，各文件独立恢复 LF/CRLF。
    cpp, cpp_newline = _sdl_normalize_newlines(cpp, "cpp")
    header, header_newline = _sdl_normalize_newlines(header, "h")
    include = '#include "../../graphics/opengl/OpenGLBindings.h"'
    cpp = _sdl_replace(cpp, include, include + '\n#include "../../graphics/vulkan/gl_vk_c.h"\n'
                       '#if defined(HX_ANDROID)\n#include <SDL_syswm.h>\n#endif', "include")

    null_check = (
        "\t\tif (!sdlWindow) {\n\n"
        "\t\t\tprintf (\"Could not create SDL window: %s.\\n\", SDL_GetError ());\n"
        "\t\t\treturn;\n\n"
        "\t\t}\n\n"
    )
    vk_init = (
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
    # 旧脚本把 Vk 放在空判断前；新版和干净源统一放在成功创建主窗之后。
    old_init = vk_init + null_check if "gl_vk_init" in cpp else null_check
    cpp = _sdl_replace(cpp, old_init, null_check + vk_init, "Vulkan init after null check")
    cpp = _sdl_replace(cpp, "\t\tcontext = 0;\n\n\t\tcontextWidth = 0;",
                       "\t\tcontext = 0;\n\t\tglWindow = 0;\n\n\t\tcontextWidth = 0;", "glWindow init")
    cpp = _sdl_replace(cpp, "\t\t\tsdlWindowFlags |= SDL_WINDOW_OPENGL;",
                       "\t\t\t#ifndef HX_WINDOWS\n\t\t\tsdlWindowFlags |= SDL_WINDOW_OPENGL;\n\t\t\t#endif",
                       "main window flags")
    cpp = _sdl_replace(cpp,
        "\t\t\tcontext = SDL_GL_CreateContext (sdlWindow);\n\n"
        "\t\t\tif (context && SDL_GL_MakeCurrent (sdlWindow, context) == 0) {",
        "\t\t\t#ifdef HX_WINDOWS\n"
        "\t\t\t// Keep the Vulkan HWND free of WGL; only this unregistered window hosts GL.\n"
        "\t\t\tint displayIndex = SDL_GetWindowDisplayIndex (sdlWindow);\n"
        "\t\t\tint glWindowPosition = SDL_WINDOWPOS_UNDEFINED_DISPLAY (displayIndex >= 0 ? displayIndex : 0);\n"
        "\t\t\tglWindow = SDL_CreateWindow (\"Lime GL compatibility\", glWindowPosition, glWindowPosition, 1, 1, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);\n"
        "\t\t\tSDL_Window* contextWindow = glWindow;\n"
        "\t\t\t#else\n"
        "\t\t\tSDL_Window* contextWindow = sdlWindow;\n"
        "\t\t\t#endif\n\n"
        "\t\t\tcontext = contextWindow ? SDL_GL_CreateContext (contextWindow) : 0;\n\n"
        "\t\t\tif (context && SDL_GL_MakeCurrent (contextWindow, context) == 0) {", "GL context window")
    cpp = _sdl_replace(cpp, "\t\t\t\tif (flags & WINDOW_FLAG_VSYNC) {",
                       "\t\t\t\tif (!glWindow && (flags & WINDOW_FLAG_VSYNC)) {", "hidden GL interval")
    cpp = _sdl_replace(cpp, "\t\t\t\tSDL_GL_DeleteContext (context);\n\t\t\t\tcontext = NULL;",
                       "\t\t\t\tif (context) SDL_GL_DeleteContext (context);\n\t\t\t\tcontext = NULL;",
                       "failed GL context")
    cpp = _sdl_replace(cpp, "\t\tif (!context) {\n\n\t\t\tsdlRendererFlags &= ~SDL_RENDERER_ACCELERATED;",
        "\t\tif (!context) {\n\n"
        "\t\t\tif (glWindow) {\n\n"
        "\t\t\t\tSDL_DestroyWindow (glWindow);\n"
        "\t\t\t\tglWindow = 0;\n\n"
        "\t\t\t}\n\n"
        "\t\t\tsdlRendererFlags &= ~SDL_RENDERER_ACCELERATED;", "failed GL window")

    destroy_main = (
        "\t\tif (sdlWindow) {\n\n"
        "\t\t\tSDL_DestroyWindow (sdlWindow);\n"
        "\t\t\tsdlWindow = 0;\n\n"
        "\t\t}\n"
    )
    cpp = _sdl_replace(cpp,
        "\tSDLWindow::~SDLWindow () {\n\n" + destroy_main + "\n"
        "\t\tif (sdlRenderer) {\n\n"
        "\t\t\tSDL_DestroyRenderer (sdlRenderer);\n\n"
        "\t\t} else if (context) {\n\n"
        "\t\t\tSDL_GL_DeleteContext (context);\n\n"
        "\t\t}\n\n\t}",
        "\tSDLWindow::~SDLWindow () {\n\n\t\tClose ();\n\n\t}", "destructor")
    cpp = _sdl_replace(cpp, "\tvoid SDLWindow::Close () {\n\n" + destroy_main + "\n\t}",
        "\tvoid SDLWindow::Close () {\n\n"
        "\t\tif (context) {\n\n"
        "\t\t\tSDL_GL_DeleteContext (context);\n"
        "\t\t\tcontext = 0;\n\n"
        "\t\t}\n\n"
        "\t\tif (glWindow) {\n\n"
        "\t\t\tSDL_DestroyWindow (glWindow);\n"
        "\t\t\tglWindow = 0;\n\n"
        "\t\t}\n\n"
        "\t\tif (sdlRenderer) {\n\n"
        "\t\t\tSDL_DestroyRenderer (sdlRenderer);\n"
        "\t\t\tsdlRenderer = 0;\n"
        "\t\t\tsdlTexture = 0; // SDL_DestroyRenderer also destroys its textures.\n\n"
        "\t\t}\n\n" + destroy_main + "\n\t}", "Close cleanup")
    flip = "\tvoid SDLWindow::ContextFlip () {"
    cpp = _sdl_replace(cpp, flip, flip + "\n\n"
        "\t\tif (gl_vk_active ()) {\n\n"
        "\t\t\tgl_vk_end_frame ();\n"
        "\t\t\treturn;\n\n"
        "\t\t}\n", "Vulkan frame")
    cpp = _sdl_replace(cpp, "\t\tif (context && !sdlRenderer) {",
        "\t\tif (sdlWindow && context && !sdlRenderer && !glWindow && (SDL_GetWindowFlags (sdlWindow) & SDL_WINDOW_OPENGL)) {",
        "GL swap guard")
    cpp = _sdl_replace(cpp, "\t\t\tSDL_GL_MakeCurrent (sdlWindow, context);",
                       "\t\t\tSDL_GL_MakeCurrent (glWindow ? glWindow : sdlWindow, context);", "ContextMakeCurrent")

    # 保留旧 scale/size 诊断，但和核心补丁一起检查、写入，不再靠缺失的诊断标记跳过。
    scale = "\tdouble SDLWindow::GetScale () {"
    scale_diag = (
        "\n\n\t\tint _dbgW = 0, _dbgH = 0;\n"
        "\t\tint _dbgWW = 0, _dbgWH = 0;\n"
        "\t\tSDL_GL_GetDrawableSize (sdlWindow, &_dbgW, &_dbgH);\n"
        "\t\tSDL_GetWindowSize (sdlWindow, &_dbgWW, &_dbgWH);\n"
        "\t\tfprintf (stderr, \"[scale] drawable=%dx%d window=%dx%d ratio=%.4f\\n\",\n"
        "\t\t\t_dbgW, _dbgH, _dbgWW, _dbgWH, _dbgWW > 0 ? double(_dbgW)/_dbgWW : 0.0);\n"
    )
    old_scale = scale + scale_diag if "_dbgW" in cpp else scale
    cpp = _sdl_replace(cpp, old_scale,
        scale + "\n\n\t\tif (glWindow) {\n\n\t\t\treturn 1;\n\n\t\t}" + scale_diag,
        "GetScale hidden GL guard")
    cpp = _sdl_replace(cpp,
        "\tint SDLWindow::GetWidth () {\n\n\t\tint width;\n\t\tint height;\n\n"
        "\t\tSDL_GetWindowSize (sdlWindow, &width, &height);\n\n\t\treturn width;",
        "\tint SDLWindow::GetWidth () {\n\n"
        "\t\tint width;\n\t\tint height;\n"
        "\t\tint dw = 0, dh = 0;\n"
        "\t\tSDL_GetWindowSize (sdlWindow, &width, &height);\n"
        "\t\tSDL_GL_GetDrawableSize (sdlWindow, &dw, &dh);\n"
        "\t\tfprintf (stderr, \"[size] GetWidth: SDL_GetWindowSize=%dx%d drawable=%dx%d\\n\", width, height, dw, dh);\n"
        "\t\treturn width;", "GetWidth diagnostic")
    header = _sdl_replace(header, "\t\tprivate:\n\n\t\t\tSDL_GLContext context;\n\t\t\tint contextHeight;",
                         "\t\tprivate:\n\n\t\t\tSDL_GLContext context;\n\t\t\tSDL_Window* glWindow;\n\t\t\tint contextHeight;",
                         "private glWindow declaration")
    return cpp.replace("\n", cpp_newline), header.replace("\n", header_newline)


def patch_sdl_window_files(cpp_path, header_path):
    # 先读、转换并编码两份完整结果；任何缺失/重复锚点都在写入前抛错。
    with open(cpp_path, "rb") as source:
        cpp = source.read()
    with open(header_path, "rb") as source:
        header = source.read()
    new_cpp, new_header = patch_sdl_window_sources(cpp.decode("utf-8"), header.decode("utf-8"))
    results = (new_cpp.encode("utf-8"), new_header.encode("utf-8"))
    for path, before, after in zip((cpp_path, header_path), (cpp, header), results):
        if before != after:
            with open(path, "wb") as target:
                target.write(after)


sdl = os.path.join(LIME, "project", "src", "backend", "sdl", "SDLWindow.cpp")
sdl_header = os.path.join(LIME, "project", "src", "backend", "sdl", "SDLWindow.h")
patch_sdl_window_files(sdl, sdl_header)
print("SDLWindow.cpp/.h Vulkan and hidden GL patches verified")

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
