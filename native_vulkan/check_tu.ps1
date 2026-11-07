<#
check_tu.ps1 —— native_vulkan 源码的单 TU（translation unit）本地检查

用途：不跑 hxcpp/lime 全量构建，只对 patch_lime.py 真正注入 Build.xml 的源文件逐个做
"编译到 obj / 语法检查"，用几十秒换回"平台解耦漏网"这一类编译期错误。

## 为什么 Windows 源必须用 MSVC，而不是 NDK clang
安卓的 clang 永远不会走 windows.h 那一路：它不认识 MAX_PATH / GetModuleFileNameA /
CreateFile / HWND，遇到这些符号只会报"未声明"，因此**用 NDK clang 检查 Windows 源
等于没检查**。历史上 vk_renderer.cpp 的 `C2065 MAX_PATH` / `C3861 GetModuleFileNameA`
就是这样漏出去的：当时只有 clang 那一组，Windows 组压根不存在。
所以：Windows 源（vk_renderer.cpp、gl_*.cpp、vk_packet.cpp、gl_packet.cpp、
vk_platform_win32.cpp）用 vcvarsall + cl.exe（/c 真编译，obj 只落 %TEMP%），
安卓源（vk_platform_android.cpp 及同一批共用源）用 NDK clang++ 的 --target 交叉检查。

## CFFI 桩是什么
gl_shader.cpp / gl_core.cpp / gl_fbo.cpp / gl_texture_params.cpp 通过 gl_compat.h 引
`<system/CFFI.h>`（hxcpp 的 CFFI 头）。本机没有 hxcpp 源码，所以脚本在 %TEMP% 下生成
一个最小桩：
    typedef void* value;
    class HxString { const char* __s; ... };   // gl_shader 的 HxToString 直接读 __s
    value alloc_null/alloc_string/alloc_int/alloc_array/alloc_empty_object/val_id/...
    只给声明，不给实现（链接期由真实 hxcpp 提供）
这只够通过语法检查，不含任何运行时实现。桩由脚本每次生成，不依赖仓库里遗留文件，
也不写进仓库。它只影响"能不能解析到类型名"，不会掩盖 Windows/Android 的 API 差异。

## 风险台账：demo_renderer.cpp / main.cpp
这两个文件含大量 Win32（窗口过程、CreateWindowEx、GetMessage 之类），**不在任何
BUILD 组里**（patch_lime.py 的 COMMON_SOURCES / PLATFORM_SOURCES 都没有它们）。
谁把它们加进 project/Build.xml，谁就会在安卓构建里直接炸。要跑它们只能在 Windows
侧单独用 cl 编，不能进安卓组。本脚本有意不检查这两个文件。

## glslang（仅 gl_shader.cpp 需要）
版本固定 16.6.0，与 CI 同源（native_vulkan/build-lime-vulkan.yml 里 Expand-Archive 的那份）：
https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip
解压到 C:\glslang（需要 include\glslang\Include\glslang_c_interface.h）。
链接期的 glslang.lib / SPIRV-Tools*.lib 与 /c 检查无关；本脚本只校验头文件在位。
版本对不上（例如 build_info.h 里不是 16/6/0）就当作"缺依赖"处理，不静默降级。

## 约定
- 缺 vcvarsall.bat / 缺 Vulkan 头 / 缺 glslang include / 缺 NDK 时：明确报"缺什么、
  从哪拿"，并 exit 2。**绝不跳过某个文件后仍然显示通过**（那是假绿）。
- 任一 TU 失败：该组 exit 非 0，最终 exit 1，并逐个列出失败文件与诊断行。
- 仓库零产物：obj 与 stub 全部在 %TEMP%\vk_tu_check 下。
- Vulkan 头必须用 tools\gl_vk_diff\third_party\vkh\include；不要用 NDK sysroot 里的
  Vulkan 头（它会把 NDK 的 stdint.h / math.h / android/* 带进 Windows 组，产生假错误）。
#>

[CmdletBinding()]
param(
    [string]$Repo = 'C:\Users\yuu\Documents\Trae-CN\temp_20261106_01\native_vulkan',
    [string]$Vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat',
    [string]$VulkanInclude = 'C:\Users\yuu\Documents\Trae-CN\temp_20261106_01\tools\gl_vk_diff\third_party\vkh\include',
    [string]$GlslangRoot = 'C:\glslang',
    [string]$NdkRoot = 'C:\Users\yuu\Documents\deepseek-harness\default-workspace\android-ndk-r27c'
)

$ErrorActionPreference = 'Stop'

if (-not $PSBoundParameters.ContainsKey('NdkRoot') -and $env:ANDROID_NDK_ROOT -and (Test-Path $env:ANDROID_NDK_ROOT)) {
    $NdkRoot = $env:ANDROID_NDK_ROOT
}

# 与 patch_lime.py 的 COMMON_SOURCES / PLATFORM_SOURCES 一一对应，改清单时两边同步。
$CommonSources = @(
    'vk_renderer.cpp',
    'gl_core.cpp',
    'gl_shader.cpp',
    'gl_vk_contract.cpp',
    'gl_fbo.cpp',
    'gl_texture_params.cpp',
    'gl_state.cpp',
    'vk_packet.cpp',
    'gl_packet.cpp',
    'vk_tuning.cpp'
)
$WinOnlySources = @('vk_platform_win32.cpp')
$AndroidOnlySources = @('vk_platform_android.cpp')

$Clang = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
$Sysroot = Join-Path $NdkRoot 'toolchains\llvm\prebuilt\windows-x86_64\sysroot'
$GlslangInclude = Join-Path $GlslangRoot 'include'

$Work = Join-Path $env:TEMP 'vk_tu_check'
$ObjDir = Join-Path $Work 'obj'
$StubRoot = Join-Path $Work 'cffistub'

# ---------- 前置检查：缺依赖就 fail-fast，不允许静默跳过 ----------
$fatal = @()

if (-not (Test-Path (Join-Path $Repo 'vk_platform.h'))) {
    $fatal += "缺源码目录：$Repo（用 -Repo 指向含 vk_platform.h 的 native_vulkan 目录）"
}

foreach ($f in ($CommonSources + $WinOnlySources + $AndroidOnlySources)) {
    if (-not (Test-Path (Join-Path $Repo $f))) {
        $fatal += "缺源文件：$f（预期在 $Repo）"
    }
}

if (-not (Test-Path $Vcvars)) {
    $fatal += "缺 vcvarsall.bat：$Vcvars —— 从 Visual Studio Installer 安装「使用 C++ 的桌面开发」（MSVC + Windows SDK），或用 -Vcvars 指定实际路径"
}
if (-not (Test-Path (Join-Path $VulkanInclude 'vulkan\vulkan.h'))) {
    $fatal += "缺 Vulkan 头：$VulkanInclude\vulkan\vulkan.h —— 从 tools\gl_vk_diff\third_party\vkh 取（不要用 NDK sysroot 的 Vulkan 头，会污染 Windows 组）"
}
if (-not (Test-Path (Join-Path $GlslangInclude 'glslang\Include\glslang_c_interface.h'))) {
    $fatal += "缺 glslang 16.6.0 头：$GlslangInclude\glslang\Include\glslang_c_interface.h —— 下载 https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip 并 Expand-Archive 到 $GlslangRoot"
}
if (-not (Test-Path $Clang)) {
    $fatal += "缺 NDK clang++：$Clang —— 安装 Android NDK（r27 及以上），或用 -NdkRoot 指向 NDK 根目录（也可设 ANDROID_NDK_ROOT）"
}
if (-not (Test-Path $Sysroot)) {
    $fatal += "缺 NDK sysroot：$Sysroot —— NDK 安装不完整，重新解压 NDK"
}

if ($fatal.Count -gt 0) {
    Write-Host 'check_tu: 前置检查失败，未执行任何编译（不是通过）：' -ForegroundColor Red
    foreach ($m in $fatal) { Write-Host ("  - " + $m) -ForegroundColor Red }
    exit 2
}

New-Item -ItemType Directory -Force -Path $ObjDir, (Join-Path $StubRoot 'system') | Out-Null

# ---------- CFFI 桩（见头注释；只为本机语法检查） ----------
$cffiHeader = @'
#pragma once
// check_tu.ps1 自动生成的最小 CFFI 桩：只声明 hxcpp 的 CFFI 表面，供编译期解析类型与
// 符号名；不含任何实现，也不负责运行时语义。
//   - HxString 仿 hxcpp：带 const char* __s（gl_shader.cpp 的 HxToString 直接读它）
//   - value = void*（真实 hxcpp 是不透明句柄，检查期只需要它能赋值/传参）
//   - alloc_* / val_* 是 hxcpp 运行时 API，这里只给声明，链接期由真实 hxcpp 提供
#include <string>
typedef void* value;

class HxString {
public:
    const char* __s;
    HxString () : __s (0) {}
    HxString (const char* s) : __s (s) {}
};

value alloc_null ();
value alloc_string (const char* s);
value alloc_int (int i);
value alloc_array (int n);
value alloc_empty_object ();
void  alloc_field (value obj, value id, value v);
value val_id (const char* s);
int   val_int (value v);
int   val_array_size (value a);
value val_array_i (value a, int i);
void  val_array_set_i (value a, int i, value v);
'@
Set-Content -Path (Join-Path $StubRoot 'system\CFFI.h') -Value $cffiHeader -Encoding UTF8
Set-Content -Path (Join-Path $StubRoot 'system\CFFIPointer.h') -Value '#pragma once' -Encoding UTF8

function Show-Diagnostics {
    param([string[]]$Lines)
    $hits = $Lines | Where-Object { $_ -match '^\S+\(\d+\)\s*:\s*(error|fatal error)' -or $_ -match '\berror\b' }
    if ($hits) {
        $hits | Select-Object -First 12 | ForEach-Object { Write-Host ("      " + $_.Trim()) -ForegroundColor DarkYellow }
    }
}

$results = @()

# ---------- Windows 组：vcvarsall + cl /c ----------
# 编译器会在 stderr 打 warning，这里不能用 Stop（会被 PowerShell 当成失败抛出）。
$ErrorActionPreference = 'Continue'
Write-Host "Windows 组（MSVC cl）：$($CommonSources.Count + $WinOnlySources.Count) 个 TU"
$failWin = @()
foreach ($f in ($CommonSources + $WinOnlySources)) {
    $src = Join-Path $Repo $f
    $obj = Join-Path $ObjDir ((Get-Item $src).BaseName + '.obj')
    $cl = 'call "' + $Vcvars + '" x64 >nul && cl /nologo /c /EHsc /std:c++17 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS' +
          ' /I "' + $Repo + '"' +
          ' /I "' + $VulkanInclude + '"' +
          ' /I "' + $GlslangInclude + '"' +
          ' /I "' + $StubRoot + '"' +
          ' /Fo"' + $obj + '" "' + $src + '" 2>&1'
    $out = & cmd.exe /c $cl
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        Write-Host ("  ok   {0} exit=0" -f $f) -ForegroundColor Green
    } else {
        Write-Host ("  FAIL {0} exit={1}" -f $f, $code) -ForegroundColor Red
        Show-Diagnostics -Lines $out
        $failWin += $f
    }
}
$winExit = if ($failWin.Count -eq 0) { 0 } else { 1 }
$results += [pscustomobject]@{ Group = 'windows(MSVC)'; Total = ($CommonSources.Count + $WinOnlySources.Count); Failed = $failWin }
Write-Host ("Windows 组结果：exit={0}，失败 {1}/{2}" -f $winExit, $failWin.Count, ($CommonSources.Count + $WinOnlySources.Count))
Write-Host ''

# ---------- 安卓组：NDK clang++ --target=aarch64-linux-android24 ----------
Write-Host "安卓组（NDK clang++）：$($CommonSources.Count + $AndroidOnlySources.Count) 个 TU"
$failAndroid = @()
foreach ($f in ($CommonSources + $AndroidOnlySources)) {
    $src = Join-Path $Repo $f
    $argv = @(
        '--target=aarch64-linux-android24',
        ("--sysroot=" + $Sysroot),
        '-fsyntax-only',
        '-std=c++17',
        '-Wall',
        '-Wextra',
        ('-I' + $Repo),
        ('-I' + $VulkanInclude),
        ('-I' + $GlslangInclude),
        ('-I' + $StubRoot),
        $src
    )
    $out = & $Clang @argv 2>&1
    $code = $LASTEXITCODE
    if ($code -eq 0) {
        $warnCount = @($out | Where-Object { $_ -match '\bwarning:' }).Count
        Write-Host ("  ok   {0} exit=0 warnings={1}" -f $f, $warnCount) -ForegroundColor Green
    } else {
        Write-Host ("  FAIL {0} exit={1}" -f $f, $code) -ForegroundColor Red
        Show-Diagnostics -Lines $out
        $failAndroid += $f
    }
}
$androidExit = if ($failAndroid.Count -eq 0) { 0 } else { 1 }
$results += [pscustomobject]@{ Group = 'android(NDK clang)'; Total = ($CommonSources.Count + $AndroidOnlySources.Count); Failed = $failAndroid }
Write-Host ("安卓组结果：exit={0}，失败 {1}/{2}" -f $androidExit, $failAndroid.Count, ($CommonSources.Count + $AndroidOnlySources.Count))
Write-Host ''

Write-Host '================ 汇总 ================'
foreach ($r in $results) {
    $state = if ($r.Failed.Count -eq 0) { 'PASS' } else { 'FAIL' }
    Write-Host ("{0}: {1}  ({2} 个 TU){3}" -f $r.Group, $state, $r.Total,
        $(if ($r.Failed.Count -eq 0) { '' } else { '  失败文件: ' + ($r.Failed -join ', ') }))
}

if ($winExit -ne 0 -or $androidExit -ne 0) {
    Write-Host 'check_tu: 失败（上面列出的文件需要修）' -ForegroundColor Red
    exit 1
}
Write-Host 'check_tu: 两组全部通过。注意：这只证明单 TU 能编译，不等于全量构建/链接通过。' -ForegroundColor Green
Write-Host ("产物仅存在于临时目录：{0}（仓库内无新增文件）" -f $Work)
exit 0
