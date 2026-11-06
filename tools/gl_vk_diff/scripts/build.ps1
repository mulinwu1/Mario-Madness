# Build gl_vk_diff. Compiles only tools/gl_vk_diff plus the native_vulkan translation layer
# (the latter is compiled as-is, never modified).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean
param(
    [switch]$Clean,
    [string]$SpvDir = ""
)

$ErrorActionPreference = "Stop"

$Root      = Split-Path -Parent $PSScriptRoot                  # tools\gl_vk_diff
$RepoRoot  = Split-Path -Parent (Split-Path -Parent $Root)     # repo root (contains native_vulkan)
$NativeVk  = Join-Path $RepoRoot "native_vulkan"
$BuildDir  = Join-Path $Root "build"
$VkInc     = Join-Path $Root "third_party\vkh\include"

if (-not (Test-Path (Join-Path $NativeVk "vk_renderer.cpp"))) {
    throw "native_vulkan\\vk_renderer.cpp not found. Keep this folder at <repo>/tools/gl_vk_diff."
}
if (-not (Test-Path (Join-Path $VkInc "vulkan\vulkan.h"))) {
    throw "Vulkan headers missing. Run scripts\\fetch_deps.ps1 first."
}

if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# ---- locate MSVC ----
$vsw = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$inst = ""
if (Test-Path $vsw) {
    $inst = (& $vsw -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
}
if (-not $inst) {
    $cand = Get-ChildItem -Path "C:\Program Files*\Microsoft Visual Studio\*" -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName "VC\Auxiliary\Build\vcvars64.bat") } |
            Select-Object -First 1
    if ($cand) { $inst = $cand.FullName }
}
if (-not $inst) { throw "Visual Studio Build Tools (VC++ toolchain) not found." }
$vcvars = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

$Sources = @(
    "src\image.cpp",
    "src\perf.cpp",
    "src\scenes.cpp",
    "src\backend_gl.cpp",
    "src\backend_vk.cpp",
    "src\native_gl_fbo.cpp",
    "src\main.cpp",
    (Join-Path $NativeVk "gl_core.cpp"),
    (Join-Path $NativeVk "gl_texture_params.cpp"),
    (Join-Path $NativeVk "gl_state.cpp"),
    (Join-Path $NativeVk "gl_vk_contract.cpp")
)

$clArgs = @(
    "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/MD", "/O2", "/W3", "/D_CRT_SECURE_NO_WARNINGS",
    "/I", (Join-Path $Root "src"),
    "/I", $NativeVk,
    "/I", (Join-Path $Root "shim"),
    "/I", $VkInc,
    "/FI", (Join-Path $Root "shim\gvd_prelude.h"),
    "/Fo$BuildDir\",
    "/Fe$BuildDir\gl_vk_diff.exe"
) + $Sources + @("/link", "user32.lib", "gdi32.lib")

Write-Host "[build] MSVC: $inst"
$line = 'call "' + $vcvars + '" >nul && cl ' + ($clArgs -join ' ')
Push-Location $Root
try {
    cmd /c $line
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit=$LASTEXITCODE)" }
} finally {
    Pop-Location
}

# ---- shaders must sit next to the exe (VkRenderer reads them from the exe directory) ----
function Find-Spv([string]$name) {
    if ($SpvDir) {
        $p = Join-Path $SpvDir $name
        if (Test-Path $p) { return $p }
    }
    $all = Get-ChildItem -Path $RepoRoot -Filter $name -Recurse -ErrorAction SilentlyContinue |
           Sort-Object { if ($_.FullName -match "vulkan-demo10") { 0 } else { 1 } } |
           Select-Object -First 1
    if ($all) { return $all.FullName }
    return ""
}

$vs = Find-Spv "shader.vert.spv"
$fs = Find-Spv "shader.frag.spv"
if ($vs -and $fs) {
    Copy-Item $vs $BuildDir -Force
    Copy-Item $fs $BuildDir -Force
    Write-Host "[build] shaders copied: shader.vert.spv, shader.frag.spv"
} else {
    Write-Warning "shader.vert.spv / shader.frag.spv not found. Pass -SpvDir or copy them into $BuildDir."
    Write-Warning "Do NOT place crt_test.spv next to the exe: VkRenderer prefers it."
}

Write-Host "[build] done: $BuildDir\gl_vk_diff.exe"
