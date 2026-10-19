# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# The PowerShell version of tools/perf/bench_libretro.sh (read its header): speed of a libretro core, unthrottled.
#
#   powershell -File tools\perf\bench_libretro.ps1 -Core CORE.dll -HostExe libretro_host.exe -Roms a.nes,b.nes `
#       [-Runs 5] [-Frames 3600] [-Label msvc-pgo]
# The last line: "RESULT label fps_median fps_best".
param(
    [Parameter(Mandatory = $true)][string]$Core,
    [Parameter(Mandatory = $true)][string]$HostExe,
    [Parameter(Mandatory = $true)][string[]]$Roms,
    [int]$Runs = 5,
    [int]$Frames = 3600,
    [string]$Label = ""
)
$ErrorActionPreference = "Stop"
if ($Label -eq "") { $Label = Split-Path -Leaf $Core }
Write-Host "bench_libretro.ps1: $Label  core $Core  $Frames frames, best of $Runs"
$sumMed = 0.0; $sumBest = 0.0; $total = 0
foreach ($rom in $Roms) {
    & $HostExe $Core $rom --train 300 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warning "$rom did not run; skipped"; continue }
    $times = @()
    for ($i = 0; $i -lt $Runs; $i++) {
        $t = Measure-Command { & $HostExe $Core $rom --train $Frames | Out-Null }
        $times += $t.TotalSeconds
    }
    $sorted = $times | Sort-Object
    $best = $sorted[0]
    $mid = [int][math]::Floor($sorted.Count / 2)
    $med = if ($sorted.Count % 2) { $sorted[$mid] } else { ($sorted[$mid - 1] + $sorted[$mid]) / 2 }
    "{0,-44} best {1,7:N3} s   median {2,7:N3} s   {3,6:N0} fps" -f (Split-Path -Leaf $rom), $best, $med, ($Frames / $med)
    $sumMed += $med; $sumBest += $best; $total += $Frames
}
if ($total -eq 0) { throw "no ROM ran" }
"RESULT {0} {1:N0} {2:N0}" -f $Label, ($total / $sumMed), ($total / $sumBest)
