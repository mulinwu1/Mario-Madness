<#
  deploy_vk_test.ps1 —— 把 Actions 上的 mario-vulkan-win64 产物装到本地测试目录

  用法（在仓库根目录执行，一条命令）：
    powershell -ExecutionPolicy Bypass -File tools\deploy_vk_test.ps1

  可选参数：
    -ArtifactZip <path>   指定下载好的 zip；不传则自动找 Downloads 里最新的 mario-vulkan-win64*.zip
    -TestDir <path>       测试目录，默认 <仓库根目录>\vk_test
    -BaseDir <path>       基础运行目录，默认 D:\mygame\Marios Madness
                          游戏 assets(1.75GB) 没进 CI 产物，从这里补
    -SkipBase             只解压产物，不补 assets（测试目录已有 assets 时用）
    -Fresh                先清空测试目录再装（会删 vk_test 下的东西，别指向别的目录）

  产物只有 exe + Vulkan 版 lime.ndll + lime 附带的 dll；
  assets/mods/manifest/libvlc 从 -BaseDir 复制，已存在则跳过（不重复拷 1.75GB）。
#>
[CmdletBinding()]
param(
    [string]$ArtifactZip,
    [string]$TestDir,
    [string]$BaseDir = 'D:\mygame\Marios Madness',
    [switch]$SkipBase,
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'

function Say($msg) { Write-Host "== $msg" -ForegroundColor Cyan }
function Fail($msg) { throw $msg }

# 仓库根目录 = 本脚本所在目录的上一级
$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $TestDir) { $TestDir = Join-Path $RepoRoot 'vk_test' }
$TestDir = [IO.Path]::GetFullPath($TestDir)

# ---------- 1. 找到构建产物 ----------
if (-not $ArtifactZip) {
    $searchDirs = @((Join-Path $env:USERPROFILE 'Downloads'), $RepoRoot)
    $cands = @()
    foreach ($d in $searchDirs) {
        if (-not (Test-Path $d)) { continue }
        $cands += Get-ChildItem $d -Filter 'mario-vulkan-win64*.zip' -File -ErrorAction SilentlyContinue
    }
    $cands = $cands | Sort-Object LastWriteTime -Descending
    if ($cands.Count -eq 0) {
        Fail "没找到构建包。请先在 Actions 页面下载产物 mario-vulkan-win64.zip（会自动落到 下载 目录），或用 -ArtifactZip 指定路径。"
    }
    $ArtifactZip = $cands[0].FullName
}
if (-not (Test-Path $ArtifactZip)) { Fail "构建包不存在: $ArtifactZip" }
$ArtifactZip = (Resolve-Path $ArtifactZip).Path
$zip = Get-Item $ArtifactZip
Say "构建包: $($zip.FullName)  ($([math]::Round($zip.Length/1MB,1)) MB, $($zip.LastWriteTime))"

# ---------- 2. 准备测试目录 ----------
if ($Fresh -and (Test-Path $TestDir)) {
    Say "清空测试目录: $TestDir"
    Remove-Item $TestDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $TestDir | Out-Null
Say "测试目录: $TestDir"

# ---------- 3. 从基础目录补运行必需文件 ----------
if (-not $SkipBase) {
    if (-not (Test-Path $BaseDir)) {
        Fail "基础目录不存在: $BaseDir。用 -BaseDir 指向完整游戏目录，或加 -SkipBase 只解压产物。"
    }
    $need = @('assets', 'mods', 'manifest', 'plugins', 'libvlc.dll', 'libvlccore.dll', 'icon.ico')

    # 先算一下要占多少空间（只算还缺的）
    $needBytes = 0
    foreach ($n in $need) {
        $src = Join-Path $BaseDir $n
        if (-not (Test-Path $src)) { continue }
        if (Test-Path (Join-Path $TestDir $n)) { continue }
        if ((Get-Item $src).PSIsContainer) {
            $needBytes += (Get-ChildItem $src -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        } else {
            $needBytes += (Get-Item $src).Length
        }
    }
    if ($needBytes -gt 0) {
        $drive = (Get-Item $TestDir).PSDrive
        $freeGB = [math]::Round((Get-PSDrive $drive.Name).Free / 1GB, 1)
        Say ("需要复制 {0:N1} GB，目标盘还剩 {1} GB" -f ($needBytes/1GB), $freeGB)
        if ($needBytes -gt ((Get-PSDrive $drive.Name).Free - 1GB)) {
            Fail "目标盘空间不够，至少再腾出 $([math]::Round($needBytes/1GB,1)) GB"
        }
    }

    foreach ($n in $need) {
        $src = Join-Path $BaseDir $n
        $dst = Join-Path $TestDir $n
        if (-not (Test-Path $src)) { continue }
        if (Test-Path $dst) { Write-Host "  跳过(已存在): $n"; continue }
        if ((Get-Item $src).PSIsContainer) {
            robocopy $src $dst /E /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { Fail "robocopy 失败($LASTEXITCODE): $n" }
        } else {
            Copy-Item $src $dst -Force
        }
        Write-Host "  已复制: $n"
    }
}

# ---------- 4. 解压产物（覆盖 exe / lime.ndll） ----------
Expand-Archive -Path $ArtifactZip -DestinationPath $TestDir -Force
Say "解压完成"

# ---------- 5. 校验 ----------
$exe  = Join-Path $TestDir 'Mario.exe'
$ndll = Join-Path $TestDir 'lime.ndll'
if (-not (Test-Path $exe))  { Fail "产物里没有 Mario.exe: $exe" }
if (-not (Test-Path $ndll)) { Fail "产物里没有 lime.ndll: $ndll" }

$txt = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($ndll))
$vkCount = ([regex]::Matches($txt, 'vkCreateInstance')).Count
if ($vkCount -lt 1) { Fail "lime.ndll 里没有 vkCreateInstance，说明拿到的不是 Vulkan 版构建" }
Say "lime.ndll 校验通过 (vkCreateInstance x$vkCount, sha256=$((Get-FileHash $ndll -Algorithm SHA256).Hash.Substring(0,12))...)"

# ---------- 6. 生成启动脚本（带日志重定向） ----------
$batLines = @(
    '@echo off',
    'cd /d "%~dp0"',
    'title Mario Madness - Vulkan lime test',
    'if exist vk_stdout.log del /q vk_stdout.log',
    'if exist vk_stderr.log del /q vk_stderr.log',
    'echo Launching Mario.exe (Vulkan lime)...',
    'Mario.exe 1> "vk_stdout.log" 2> "vk_stderr.log"',
    'echo.',
    'echo Exited. Exit code: %errorlevel%',
    'echo --- stderr tail ---',
    'if exist vk_stderr.log type "vk_stderr.log"',
    'echo.',
    'pause'
)
Set-Content -Path (Join-Path $TestDir 'launch-vk.bat') -Value $batLines -Encoding ASCII

# ---------- 7. 汇总 ----------
Write-Host ''
Say '部署完成'
Write-Host "  测试目录 : $TestDir"
Write-Host "  启动     : $exe   （或 $TestDir\launch-vk.bat，会自动抓日志）"
Write-Host "  日志     : $TestDir\vk_stdout.log"
Write-Host "             $TestDir\vk_stderr.log"
Write-Host "  说明     : $TestDir\RUNNING.txt"
