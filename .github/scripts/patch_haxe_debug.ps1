param(
    [string]$HaxelibRoot = "$env:USERPROFILE\haxelib"
)

# Temporary diagnostic patch: add trace() calls along the openfl/lime size chain.
# Logging only, no behavior change. Idempotent: skip if marker string already present.
# Targets:
#   openfl Stage.__resize / __onLimeWindowResize / __setLogicalSize
#   openfl OpenGLRenderer.__resize
#   lime NativeWindow.create
#   lime Window.resize

$ErrorActionPreference = 'Stop'

function Save-Lines([string]$path, [string[]]$lines) {
    [System.IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"))
}

function Find-Line([string[]]$lines, [string]$pattern, [int]$from = 0, [int]$limit = 0) {
    $end = $lines.Count
    if ($limit -gt 0) { $end = [Math]::Min($from + $limit, $lines.Count) }
    for ($i = $from; $i -lt $end; $i++) {
        if ($lines[$i] -match $pattern) { return $i }
    }
    return -1
}

# ---------- 1) openfl Stage.hx ----------
$p = Join-Path $HaxelibRoot 'openfl\9,2,0\src\openfl\display\Stage.hx'
if (-not (Test-Path $p)) { throw "missing file: $p" }
$lines = [System.IO.File]::ReadAllLines($p)
$all = $lines -join "`n"

if ($all.Contains('[dbgStage] resize win=')) {
    Write-Host "Stage.hx: already patched"
} else {
    # 1a) __resize entry: window.width / window.scale / logical / windowWidth
    $ri = Find-Line $lines 'private function __resize\(\):Void'
    if ($ri -lt 0) { throw 'Stage.__resize anchor missing' }
    $wh = Find-Line $lines 'var windowHeight = Std\.int\(window\.height \* window\.scale\);' $ri 12
    if ($wh -lt 0) { throw 'Stage windowHeight anchor missing' }
    $lines = [string[]]($lines[0..$wh] + @(
        '        var _dbgDpi:String = #if openfl_dpi_aware "1" #else "0" #end;',
        '        trace("[dbgStage] resize win=" + window.width + "x" + window.height + " scale=" + window.scale + " logical=" + __logicalWidth + "x" + __logicalHeight + " sm=" + scaleMode + " stage=" + cacheWidth + "x" + cacheHeight + " winW=" + windowWidth + "x" + windowHeight + " dpiAware=" + _dbgDpi);'
    ) + $lines[($wh + 1)..($lines.Count - 1)])

    # 1b) __resize exit: stageWidth/stageHeight + displayMatrix
    $rd = Find-Line $lines '^\s*__renderDirty = true;\s*$' $ri 220
    if ($rd -lt 0) { throw 'Stage __renderDirty anchor missing' }
    $lines = [string[]]($lines[0..($rd - 1)] + @(
        '        trace("[dbgStage] resize done stage=" + stageWidth + "x" + stageHeight + " dm=" + __displayMatrix.a + "," + __displayMatrix.d + " t=" + __displayMatrix.tx + "," + __displayMatrix.ty + " winW=" + windowWidth + "x" + windowHeight);'
    ) + $lines[$rd..($lines.Count - 1)])

    # 1c) __onLimeWindowResize: width/height carried by the event vs cached window values
    $ei = Find-Line $lines 'private function __onLimeWindowResize\(window:Window, width:Int, height:Int\):Void'
    if ($ei -lt 0) { throw '__onLimeWindowResize anchor missing' }
    $ni = Find-Line $lines 'this\.window != window' $ei 8
    if ($ni -lt 0) { throw '__onLimeWindowResize null-check anchor missing' }
    $lines = [string[]]($lines[0..$ni] + @(
        '        trace("[dbgStage] limeResize event=" + width + "x" + height + " win=" + window.width + "x" + window.height + " scale=" + window.scale);'
    ) + $lines[($ni + 1)..($lines.Count - 1)])

    # 1d) __setLogicalSize: who sets the logical size, and to what
    $li = Find-Line $lines 'private function __setLogicalSize\(width:Int, height:Int\):Void'
    if ($li -lt 0) { throw '__setLogicalSize anchor missing' }
    $lw = Find-Line $lines '^\s*__logicalWidth = width;\s*$' $li 8
    if ($lw -lt 0) { throw '__setLogicalSize body anchor missing' }
    $lines = [string[]]($lines[0..($lw - 1)] + @(
        '        trace("[dbgStage] setLogicalSize " + width + "x" + height + " prev=" + __logicalWidth + "x" + __logicalHeight);'
    ) + $lines[$lw..($lines.Count - 1)])

    Save-Lines $p $lines
    Write-Host "patched Stage.hx: 4 debug traces"
}

# ---------- 2) openfl OpenGLRenderer.hx ----------
$p = Join-Path $HaxelibRoot 'openfl\9,2,0\src\openfl\display\OpenGLRenderer.hx'
if (-not (Test-Path $p)) { throw "missing file: $p" }
$lines = [System.IO.File]::ReadAllLines($p)
$all = $lines -join "`n"

if ($all.Contains('[dbgGL] resize')) {
    Write-Host "OpenGLRenderer.hx: already patched"
} else {
    $gi = Find-Line $lines 'private override function __resize\(width:Int, height:Int\):Void'
    if ($gi -lt 0) { throw 'OpenGLRenderer.__resize anchor missing' }
    $pi = Find-Line $lines '__projectionFlipped\.createOrtho' $gi 30
    if ($pi -lt 0) { throw 'projection createOrtho anchor missing' }
    $lines = [string[]]($lines[0..$pi] + @(
        '        trace("[dbgGL] resize in=" + width + "x" + height + " stage=" + __stage.stageWidth + "x" + __stage.stageHeight + " wt=" + __worldTransform.a + "," + __worldTransform.d + " t=" + __worldTransform.tx + "," + __worldTransform.ty + " off=" + __offsetX + "," + __offsetY + " disp=" + __displayWidth + "x" + __displayHeight + " den=" + (__displayWidth + __offsetX * 2));'
    ) + $lines[($pi + 1)..($lines.Count - 1)])
    Save-Lines $p $lines
    Write-Host "patched OpenGLRenderer.hx: 1 debug trace"
}

# ---------- 3) lime NativeWindow.hx (before __scale assignment in create) ----------
$p = Join-Path $HaxelibRoot 'lime\8,0,2\src\lime\_internal\backend\native\NativeWindow.hx'
if (-not (Test-Path $p)) { throw "missing file: $p" }
$lines = [System.IO.File]::ReadAllLines($p)
$all = $lines -join "`n"

if ($all.Contains('[dbgLime] NativeWindow.create')) {
    Write-Host "NativeWindow.hx: already patched"
} else {
    $ci = Find-Line $lines 'parent\.__scale = NativeCFFI\.lime_window_get_scale\(handle\);'
    if ($ci -lt 0) { throw 'NativeWindow __scale anchor missing' }
    $lines = [string[]]($lines[0..($ci - 1)] + @(
        '        trace("[dbgLime] NativeWindow.create rawScale=" + NativeCFFI.lime_window_get_scale(handle) + " w=" + parent.__width + " h=" + parent.__height);'
    ) + $lines[$ci..($lines.Count - 1)])
    Save-Lines $p $lines
    Write-Host "patched NativeWindow.hx: create trace"
}

# ---------- 4) lime Window.hx (after resize) ----------
$p = Join-Path $HaxelibRoot 'lime\8,0,2\src\lime\ui\Window.hx'
if (-not (Test-Path $p)) { throw "missing file: $p" }
$lines = [System.IO.File]::ReadAllLines($p)
$all = $lines -join "`n"

if ($all.Contains('[dbgLime] Window.resize')) {
    Write-Host "Window.hx: already patched"
} else {
    $wi = Find-Line $lines 'public function resize\(width:Int, height:Int\):Void'
    if ($wi -lt 0) { throw 'Window.resize anchor missing' }
    $hi = Find-Line $lines '^\s*__height = height;\s*$' $wi 12
    if ($hi -lt 0) { throw 'Window.resize body anchor missing' }
    $lines = [string[]]($lines[0..$hi] + @(
        '        trace("[dbgLime] Window.resize " + width + "x" + height + " -> " + __width + "x" + __height + " scale=" + __scale);'
    ) + $lines[($hi + 1)..($lines.Count - 1)])
    Save-Lines $p $lines
    Write-Host "patched Window.hx: resize trace"
}

Write-Host "size-chain debug patch done (root=$HaxelibRoot)"
