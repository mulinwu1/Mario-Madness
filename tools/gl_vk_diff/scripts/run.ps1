# Run the harness. Extra args are forwarded to gl_vk_diff.exe.
#
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 --mode=diff
#   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 --mode=bench --frames=200
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$PassThru
)

$Root = Split-Path -Parent $PSScriptRoot
$RepoRoot = Split-Path -Parent (Split-Path -Parent $Root)
$exe = Join-Path $Root "build\gl_vk_diff.exe"
if (-not (Test-Path $exe)) { throw "Not built yet. Run scripts\build.ps1 first." }

# Auto-detect ANGLE unless the caller passes --angle-dir themselves.
$extra = @()
if (-not ($PassThru -match "--angle-dir=")) {
    $egl = Get-ChildItem -Path $RepoRoot -Filter libEGL.dll -Recurse -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($egl) { $extra += "--angle-dir=$($egl.DirectoryName)" }
    else { Write-Warning "libEGL.dll not found; GL reference will be skipped." }
}

if (-not ($PassThru -match "--out=")) { $extra += "--out=$(Join-Path $Root 'out')" }

& $exe @extra @PassThru
exit $LASTEXITCODE
