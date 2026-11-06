# Print the manual checklist for checks that need a real window.
# Optionally launch the game exe so the operator can eyeball the listed items.
#
#   powershell -ExecutionPolicy Bypass -File scripts\manual_window_check.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\manual_window_check.ps1 -Exe D:\path\game.exe
param(
    [string]$Exe = ""
)

$Root = Split-Path -Parent $PSScriptRoot
$doc = Join-Path $Root "docs\manual_window_check.md"

Write-Host "====================================================="
Write-Host " Manual window checks (details: docs\manual_window_check.md)"
Write-Host "====================================================="
Write-Host ""
Write-Host "1) Per-frame drawing ....... until fixed: frame 1 only, then stale/black"
Write-Host "2) Vertical orientation .... until fixed: whole frame upside down"
Write-Host "3) Scissor clipping ........ until fixed: no clipping at all"
Write-Host "4) Blend + draw order ...... until fixed: wrong compositing order / no blend"
Write-Host "5) Offscreen RT ............ until fixed: offscreen content lost"
Write-Host "6) Resize / fullscreen ..... record present continuity and LastError"
Write-Host ""
Write-Host "After each run, attach: screenshot + out\perf.log (GVD_PERF line) + out\report.md"
Write-Host ""
Write-Host "Full checklist: $doc"

if ($Exe) {
    if (-not (Test-Path $Exe)) { throw "Not found: $Exe" }
    Write-Host ""
    Write-Host "Launching: $Exe"
    Start-Process -FilePath $Exe
} else {
    Write-Host ""
    Write-Host "Pass -Exe <path> to launch the game automatically."
}
