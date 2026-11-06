// glslang 官方 16.6.0 Windows 静态库是 /MD（动态 CRT）编译的，而 lime.ndll 由 hxcpp
// 以 /MT（静态 CRT）链接，直接把 .lib 塞进 ndll 会得到 LNK2038 RuntimeLibrary 冲突。
//
// 所以这里把官方静态库包成一个独立 DLL（仍然 /MD），由 ndll 通过 import lib 调用：
// ndll 侧只剩 __imp_ 引用，不再引入任何 MD CRT 目标文件。
// 导出清单见 glslang_shim.def。
extern "C" int lime_glslang_shim_anchor (void) { return 1; }
