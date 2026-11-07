# 推之前在本机跑：precheck.ps1
# 固定顺序，任一步失败立即退出（不继续往下走），只为挡住"推上去才发现路径/符号写错"。
#
#   1) check_tu.ps1        Windows + 安卓两组单 TU 编译
#   2) 分叉源在位           vendor/openfl 5 个关键文件 + Context3D.hx 接线
#   3) 符号闭环             lime_vk_pkt_emit 定义/声明、vk_tuning 已进构建清单
#   4) 开关接线             Project.xml 或 hxml 里必须有 openfl_vk_pkt
#
# 用法： powershell -ExecutionPolicy Bypass -File native_vulkan\precheck.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot   # temp_20261106_01
$vk   = $PSScriptRoot

function Fail($msg) { Write-Host "FAIL: $msg"; exit 1 }
function Ok($msg)   { Write-Host "  ok   $msg" }

Write-Host "== 1/4 check_tu（Windows + 安卓） =="
& (Join-Path $vk 'check_tu.ps1') | Out-Host
if ($LASTEXITCODE -ne 0) { Fail "check_tu.ps1 未通过（上面有具体文件与行列）" }

Write-Host "== 2/4 分叉源在位 =="
$fork = Join-Path $root 'vendor\openfl'
$need = @(
    'haxelib.json',
    'src\openfl\display3D\Context3D.hx',
    'src\openfl\display\OpenGLRenderer.hx',
    'src\openfl\display3D\_internal\VKPktNative.hx',
    'src\openfl\display3D\_internal\VKDrawSink.hx'
)
foreach ($f in $need) {
    if (-not (Test-Path (Join-Path $fork $f))) { Fail "缺 $f（haxelib dev openfl 会指到空目录）" }
}
$ctx = Get-Content (Join-Path $fork 'src\openfl\display3D\Context3D.hx') -Raw
if ($ctx -notmatch 'VKPktNative') { Fail "Context3D.hx 没有 VKPktNative 接线（记录侧不会发包）" }
Ok "vendor/openfl 关键文件与接线齐"

Write-Host "== 3/4 native 符号闭环 =="
$gap = Join-Path $vk 'gl_packet.cpp'
$gah = Join-Path $vk 'gl_packet.h'
$gpt = Get-Content $gap -Raw
$gph = Get-Content $gah -Raw
if ($gpt -notmatch 'lime_vk_pkt_emit') { Fail "gl_packet.cpp 没有 lime_vk_pkt_emit 定义" }
if ($gph -notmatch 'lime_vk_pkt_emit') { Fail "gl_packet.h 没有 lime_vk_pkt_emit 声明" }
$patch = Get-Content (Join-Path $vk 'patch_lime.py') -Raw
foreach ($s in @('gl_packet.cpp', 'vk_tuning.cpp')) {
    if ($patch -notmatch [regex]::Escape($s)) { Fail "patch_lime.py 的 COMMON_SOURCES 漏了 $s（CI 会缺符号）" }
}
foreach ($s in @('vk_tuning.h', 'gl_packet.h')) {
    if ($patch -notmatch [regex]::Escape($s)) { Fail "patch_lime.py 的 assert 清单漏了 $s" }
}
Ok "lime_vk_pkt_emit 定义/声明一致，构建清单含 vk_tuning"

Write-Host "== 4/4 编译开关接线 =="
$proj = Join-Path $root 'fix_src\Project.xml'
$hxmls = @(Get-ChildItem (Join-Path $root 'angle_full\haxe') -Filter '*.hxml' -File -ErrorAction SilentlyContinue)
$hasProj = (Test-Path $proj) -and ((Get-Content $proj -Raw) -match 'openfl_vk_pkt')
$hasHxml = $false
foreach ($h in $hxmls) { if ((Get-Content $h.FullName -Raw) -match 'openfl_vk_pkt') { $hasHxml = $true } }
if (-not ($hasProj -or $hasHxml)) { Fail "Project.xml 与本地 hxml 都没有 openfl_vk_pkt（记录侧不会被编译进去）" }
$forkInHxml = $false
foreach ($h in $hxmls) { if ((Get-Content $h.FullName -Raw) -match 'vendor[\\/]openfl') { $forkInHxml = $true } }
if (-not $forkInHxml) { Write-Host "  warn 本地 hxml 未出现 vendor/openfl（若走 CI 的 Project.xml + haxelib dev 则可忽略）" }
Ok "openfl_vk_pkt 已接线"

Write-Host "precheck: 全部通过（只证明单 TU + 接线，不等于链接/运行通过）"
