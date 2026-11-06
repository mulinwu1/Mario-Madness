# Performance baseline. Prints one grep-able GVD_PERF line per backend and
# writes out/perf.log. No per-frame readback in the loop (render cost only).
#
#   powershell -ExecutionPolicy Bypass -File scripts\perf.ps1
param(
    [int]$Frames = 200,
    [int]$Quads = 512,
    [int]$W = 512,
    [int]$H = 512,
    [string]$Vk = ""
)

$here = $PSScriptRoot
$args = @("--mode=bench", "--frames=$Frames", "--quads=$Quads", "--w=$W", "--h=$H")
if ($Vk) { $args += "--vk=$Vk" }
& (Join-Path $here "run.ps1") @args
exit $LASTEXITCODE
