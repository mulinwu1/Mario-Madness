<#
GL -> Vulkan 覆盖统计（可重复运行）

扫描 native_vulkan/gl_compat.h 中的全部 lime_gl_* 声明，再扫描 native_vulkan/*.cpp
里的函数定义，按模块统计「真实现 / 空桩 / 未覆盖」，并列出缺口清单。

用法：
  powershell -ExecutionPolicy Bypass -File tools\coverage\coverage.ps1
  powershell -ExecutionPolicy Bypass -File tools\coverage\coverage.ps1 -Gaps
  powershell -ExecutionPolicy Bypass -File tools\coverage\coverage.ps1 -List
  powershell -ExecutionPolicy Bypass -File tools\coverage\coverage.ps1 -Markdown

参数：
  -RepoRoot   仓库根目录，默认按脚本位置上溯两级
  -Header     头文件（相对 RepoRoot），默认 native_vulkan\gl_compat.h
  -Sources    源文件目录（相对 RepoRoot）或绝对路径，默认 native_vulkan
  -Gaps       输出缺口清单（未覆盖 + 空桩，按模块分组）
  -List       输出逐条明细（入口 / 模块 / 状态 / 文件）
  -Markdown   用 markdown 表格输出，便于贴进 docs
#>

[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$Header   = 'native_vulkan\gl_compat.h',
    [string[]]$Sources = @('native_vulkan\*.cpp', 'native_vulkan\*.cc'),
    [switch]$Gaps,
    [switch]$List,
    [switch]$Markdown
)

$ErrorActionPreference = 'Stop'

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
} else {
    $RepoRoot = (Resolve-Path $RepoRoot).Path
}

$headerPath = if ([System.IO.Path]::IsPathRooted($Header)) { $Header } else { Join-Path $RepoRoot $Header }
if (-not (Test-Path -LiteralPath $headerPath)) {
    Write-Host "找不到头文件：$headerPath" -ForegroundColor Red
    exit 2
}

# ---------- 1. 收集声明 ----------
$headerText = Get-Content -LiteralPath $headerPath -Raw -Encoding UTF8
$declRegex  = [regex]'^\s*(?:void|int|bool|float|double|value)\s+(lime_gl_[A-Za-z0-9_]+)\s*\('

$decls = New-Object System.Collections.Generic.List[object]
foreach ($line in ($headerText -split "`r?`n")) {
    $m = $declRegex.Match($line)
    if ($m.Success) { $decls.Add([pscustomobject]@{ Name = $m.Groups[1].Value }) }
}

# ---------- 2. 收集定义 ----------
# 从 '{' 开始做括号匹配，跳过字符串/字符/注释，避免 GLSL 源码字符串里的花括号干扰
function Get-Body {
    param([string]$text, [int]$openIdx)
    $depth = 0
    $i = $openIdx
    $n = $text.Length
    $inString = $false
    $inLineComment = $false
    $inBlockComment = $false
    while ($i -lt $n) {
        $c = $text[$i]
        $next = if ($i + 1 -lt $n) { $text[$i + 1] } else { '' }

        if ($inLineComment) {
            if ($c -eq "`n") { $inLineComment = $false }
            $i++; continue
        }
        if ($inBlockComment) {
            if ($c -eq '*' -and $next -eq '/') { $inBlockComment = $false; $i += 2; continue }
            $i++; continue
        }
        if ($inString) {
            if ($c -eq '\') { $i += 2; continue }
            if ($c -eq '"') { $inString = $false }
            $i++; continue
        }

        if ($c -eq '"') { $inString = $true; $i++; continue }
        if ($c -eq '/' -and $next -eq '/') { $inLineComment = $true; $i += 2; continue }
        if ($c -eq '/' -and $next -eq '*') { $inBlockComment = $true; $i += 2; continue }

        if ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') {
            $depth--
            if ($depth -eq 0) { return $text.Substring($openIdx + 1, $i - $openIdx - 1) }
        }
        $i++
    }
    return $null
}

function Strip-Comments {
    param([string]$text)
    $text = [regex]::Replace($text, '/\*[\s\S]*?\*/', '')
    $text = [regex]::Replace($text, '(?m)//.*$', '')
    return $text
}

$defRegex = [regex]'(?m)^\s*(?:void|int|bool|float|double|value)\s+(lime_gl_[A-Za-z0-9_]+)\s*\('

$defs = @{}   # name -> @{ File; Body; IsReal }
$sourceFiles = New-Object System.Collections.Generic.List[string]
foreach ($glob in $Sources) {
    $full = if ([System.IO.Path]::IsPathRooted($glob)) { $glob } else { Join-Path $RepoRoot $glob }
    foreach ($f in (Get-ChildItem -Path $full -File -ErrorAction SilentlyContinue)) {
        $sourceFiles.Add($f.FullName)
    }
}

foreach ($file in $sourceFiles) {
    $text = Get-Content -LiteralPath $file -Raw -Encoding UTF8
    if (-not $text) { continue }
    $lineStarts = New-Object System.Collections.Generic.List[int]
    $lineStarts.Add(0)
    for ($i = 0; $i -lt $text.Length; $i++) { if ($text[$i] -eq "`n") { $lineStarts.Add($i + 1) } }

    foreach ($m in $defRegex.Matches($text)) {
        $name = $m.Groups[1].Value
        # 找到参数列表的右括号
        $openParen = $text.IndexOf('(', $m.Index + $m.Length - 1)
        if ($openParen -lt 0) { continue }
        $depth = 0
        $closeParen = -1
        for ($i = $openParen; $i -lt $text.Length; $i++) {
            if ($text[$i] -eq '(') { $depth++ }
            elseif ($text[$i] -eq ')') { $depth--; if ($depth -eq 0) { $closeParen = $i; break } }
        }
        if ($closeParen -lt 0) { continue }

        # 右括号之后允许空白 / const / noexcept，必须是 '{' 才算定义
        $j = $closeParen + 1
        while ($j -lt $text.Length -and [char]::IsWhiteSpace($text[$j])) { $j++ }
        if ($j -ge $text.Length -or $text[$j] -ne '{') { continue }   # 声明 / 调用

        $body = Get-Body $text $j
        if ($null -eq $body) { continue }
        $clean = (Strip-Comments $body).Trim()

        $isStub = $false
        if ($clean -eq '') {
            $isStub = $true
        } elseif ($clean -match '^(?:return\s+(?:0|0\.0|false|true|nullptr|NULL)\s*;?)+$') {
            $isStub = $true
        }

        if ($defs.ContainsKey($name)) { continue }   # 先扫到的优先（gl_core 命名靠前）
        $defs[$name] = [pscustomobject]@{
            File   = (Split-Path $file -Leaf)
            Body   = $clean
            IsReal = (-not $isStub)
        }
    }
}

# ---------- 3. 模块归类 ----------
# 有序规则，先匹配者生效。新增文件里的入口也会被自动归类。
$rules = @(
    @{ Module = 'FBO/renderbuffer'; Pattern = 'framebuffer|renderbuffer|draw_buffers|read_buffer|blit_framebuffer|read_pixels|invalidate_' },
    @{ Module = 'shader/program';   Pattern = 'shader|program|uniform|attrib_location|bind_attrib|frag_data_location' },
    @{ Module = '纹理';             Pattern = 'texture|_tex_|tex_image|tex_sub_image|tex_storage|tex_parameter|compressed_tex|copy_tex|pixel_store|internalformat|generate_mipmap' },
    @{ Module = '缓冲/顶点';        Pattern = 'buffer|vertex_attrib|vertex_array|draw_arrays|draw_elements|draw_range_elements|enable_vertex|map_buffer|unmap_buffer' },
    @{ Module = '状态';             Pattern = 'enable|disable|blend|color_mask|depth_|stencil_|cull_face|front_face|scissor|viewport|clear|polygon_offset|line_width|hint|sample_coverage' },
    @{ Module = '其他';             Pattern = '.' }
)

function Get-Module {
    param([string]$name)
    foreach ($r in $rules) {
        if ($name -match $r.Pattern) { return $r.Module }
    }
    return '其他'
}

$entries = New-Object System.Collections.Generic.List[object]
foreach ($d in $decls) {
    $def = $defs[$d.Name]
    $status = if (-not $def) { '未覆盖' } elseif ($def.IsReal) { '真实现' } else { '空桩' }
    $entries.Add([pscustomobject]@{
        Name   = $d.Name
        Module = Get-Module $d.Name
        Status = $status
        File   = if ($def) { $def.File } else { '-' }
    })
}

# ---------- 4. 统计 ----------
$moduleOrder = @('状态', '纹理', '缓冲/顶点', 'shader/program', 'FBO/renderbuffer', '其他')
$stats = @{}
foreach ($mod in $moduleOrder) {
    $stats[$mod] = [pscustomobject]@{ Total = 0; Real = 0; Stub = 0; Missing = 0 }
}
foreach ($e in $entries) {
    $s = $stats[$e.Module]
    $s.Total++
    switch ($e.Status) {
        '真实现' { $s.Real++ }
        '空桩'   { $s.Stub++ }
        default  { $s.Missing++ }
    }
}

$total = $entries.Count
$realCount = @($entries | Where-Object { $_.Status -eq '真实现' }).Count
$stubCount = @($entries | Where-Object { $_.Status -eq '空桩' }).Count
$missCount = @($entries | Where-Object { $_.Status -eq '未覆盖' }).Count

function Format-Pct {
    param([int]$part, [int]$whole)
    if ($whole -eq 0) { return '-' }
    return ('{0:N1}%' -f (100.0 * $part / $whole))
}

Write-Host ''
Write-Host "GL -> Vulkan 覆盖统计（源目录：native_vulkan，扫描 $($sourceFiles.Count) 个 cpp，声明 $total 个入口）"
Write-Host ('=' * 78)

if ($Markdown) {
    Write-Output '| 模块 | 入口数 | 真实现 | 空桩 | 未覆盖 | 真实现覆盖率 | 有落点覆盖率 |'
    Write-Output '| --- | --- | --- | --- | --- | --- | --- |'
    foreach ($mod in $moduleOrder) {
        $s = $stats[$mod]
        if ($s.Total -eq 0) { continue }
        $hit = $s.Real + $s.Stub
        Write-Output ("| {0} | {1} | {2} | {3} | {4} | {5} | {6} |" -f $mod, $s.Total, $s.Real, $s.Stub, $s.Missing,
            (Format-Pct $s.Real $s.Total), (Format-Pct $hit $s.Total))
    }
    Write-Output ("| **合计** | {0} | {1} | {2} | {3} | {4} | {5} |" -f $total, $realCount, $stubCount, $missCount,
        (Format-Pct $realCount $total), (Format-Pct ($realCount + $stubCount) $total))
} else {
    $fmt = '{0,-16} {1,7} {2,7} {3,7} {4,8} {5,10} {6,12}'
    Write-Host ($fmt -f '模块', '入口', '真实现', '空桩', '未覆盖', '真实现率', '有落点率')
    Write-Host ('-' * 78)
    foreach ($mod in $moduleOrder) {
        $s = $stats[$mod]
        if ($s.Total -eq 0) { continue }
        Write-Host ($fmt -f $mod, $s.Total, $s.Real, $s.Stub, $s.Missing,
            (Format-Pct $s.Real $s.Total), (Format-Pct ($s.Real + $s.Stub) $s.Total))
    }
    Write-Host ('-' * 78)
    Write-Host ($fmt -f '合计', $total, $realCount, $stubCount, $missCount,
        (Format-Pct $realCount $total), (Format-Pct ($realCount + $stubCount) $total))
}

if ($List) {
    Write-Host ''
    Write-Host '逐条明细'
    Write-Host ('-' * 78)
    foreach ($mod in $moduleOrder) {
        $items = @($entries | Where-Object { $_.Module -eq $mod })
        if ($items.Count -eq 0) { continue }
        Write-Host "[$mod] $($items.Count) 个"
        foreach ($e in $items) {
            Write-Host ('  {0,-42} {1,-8} {2}' -f $e.Name, $e.Status, $e.File)
        }
    }
}

if ($Gaps) {
    Write-Host ''
    Write-Host '缺口清单'
    Write-Host ('-' * 78)
    foreach ($mod in $moduleOrder) {
        $missing = @($entries | Where-Object { $_.Module -eq $mod -and $_.Status -eq '未覆盖' })
        $stubbed = @($entries | Where-Object { $_.Module -eq $mod -and $_.Status -eq '空桩' })
        if (($missing.Count + $stubbed.Count) -eq 0) { continue }
        Write-Host "[$mod] 未覆盖 $($missing.Count)，空桩 $($stubbed.Count)"
        foreach ($e in $missing) { Write-Host "  MISSING  $($e.Name)" }
        foreach ($e in $stubbed) { Write-Host "  STUB     $($e.Name)  ($($e.File))" }
    }
}

# 覆盖率过低时给出显式告警（退出码仍为 0，方便 CI/本地反复运行）
$weak = @()
foreach ($mod in $moduleOrder) {
    $s = $stats[$mod]
    if ($s.Total -gt 0 -and ($s.Real / $s.Total) -lt 0.10) {
        $weak += ("{0}({1}/{2})" -f $mod, $s.Real, $s.Total)
    }
}
if ($weak.Count -gt 0) {
    Write-Host ''
    Write-Host ("告警：以下模块真实现率低于 10%： " + ($weak -join '、')) -ForegroundColor Yellow
}
