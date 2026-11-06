import os
import sys

LIME = r"C:\lime-src"
if not os.path.isdir(LIME):
    LIME = os.environ.get("LIME_SRC", LIME)

# 1) rename GL implementations that we are replacing
p = os.path.join(LIME, "project", "src", "graphics", "opengl", "OpenGLBindings.cpp")
text = open(p, encoding="utf-8").read()

renames = [
    ("void lime_gl_clear (int mask) {", "void lime_gl_clear_gl (int mask) {"),
    ("void lime_gl_clear_color (float red, float green, float blue, float alpha) {",
     "void lime_gl_clear_color_gl (float red, float green, float blue, float alpha) {"),
    ("void lime_gl_viewport (int x, int y, int width, int height) {",
     "void lime_gl_viewport_gl (int x, int y, int width, int height) {"),
]
for a, b in renames:
    if a not in text:
        print("WARN: pattern not found:", a)
    text = text.replace(a, b)
open(p, "w", encoding="utf-8").write(text)
print("patched OpenGLBindings.cpp")

# 2) inject vulkan sources into Build.xml
b = os.path.join(LIME, "project", "Build.xml")
xml = open(b, encoding="utf-8").read()
inject = """		<file name="src/graphics/vulkan/vk_renderer.cpp" />
		<file name="src/graphics/vulkan/gl_core.cpp" />
		<file name="src/graphics/vulkan/gl_stubs.cpp" />
"""
anchor = '<file name="src/graphics/Image.cpp" />'
if "vk_renderer.cpp" in xml:
    print("build.xml already patched")
elif anchor in xml:
    xml = xml.replace(anchor, anchor + "\n" + inject)
    open(b, "w", encoding="utf-8").write(xml)
    print("patched Build.xml (sources added)")
else:
    print("ERROR: anchor not found in Build.xml")
    sys.exit(1)

print("done")
