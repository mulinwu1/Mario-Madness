# Fetch Vulkan headers only (no SDK install).
# Result layout: third_party/vkh/include/{vulkan,vk_video}
param(
    [string]$Ref = "vulkan-sdk-1.4.313.0"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $Root "third_party\vkh"
$Zip  = Join-Path $Root "third_party\_vkh_t.zip"

if (Test-Path (Join-Path $Dest "include\vulkan\vulkan.h")) {
    Write-Host "[deps] Vulkan headers already present, skipping."
    exit 0
}

New-Item -ItemType Directory -Force -Path (Join-Path $Root "third_party") | Out-Null
$url = "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$Ref.zip"
Write-Host "[deps] downloading $url"
Invoke-WebRequest -Uri $url -OutFile $Zip -UseBasicParsing

Add-Type -AssemblyName System.IO.Compression.FileSystem
$tmp = Join-Path $Root "third_party\_vkh_t"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
[System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $tmp)
$inner = Get-ChildItem -Path $tmp -Directory | Select-Object -First 1
Copy-Item -Recurse -Force (Join-Path $inner.FullName "include") $Dest
Remove-Item -Recurse -Force $tmp
Remove-Item -Force $Zip
Write-Host "[deps] done: $Dest\include"
