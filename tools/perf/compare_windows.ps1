# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Compares several builds of the libretro core on Windows (docs/pgo.md, "Comparing the builds"):
#   1. the picture after 600 frames of each ROM must be identical in every build (SHA-256);
#   2. speed, three rounds in the order A B C D, D C B A, A B C D (tools\perf\bench_libretro.ps1).
#
#   powershell -File tools\perf\compare_windows.ps1 -RomDir C:\roms -Out C:\bench `
#       -Variants "msvc-plain=build\msvc-plain\Release\one_libretro.dll|build\msvc-plain\tests\Release\libretro_host.exe", ...
# Each variant is LABEL=CORE.dll|HOST.exe. Every ROM in -RomDir (*.nes) is used. Writes -Out\result.txt.
param(
    [Parameter(Mandatory = $true)][string]$RomDir,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Rounds = 3,
    [int]$PictureFrames = 600,
    [switch]$SkipPictures,
    # last: swallows all the remaining words, so it also works through "powershell -File" (which does not build arrays)
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)][string[]]$Variants
)
$ErrorActionPreference = "Stop"
$bench = Join-Path $PSScriptRoot "bench_libretro.ps1"
$roms = @(Get-ChildItem $RomDir -Filter *.nes | ForEach-Object { $_.FullName })
if ($roms.Count -eq 0) { throw "no .nes files in $RomDir" }
New-Item -ItemType Directory -Force $Out | Out-Null
$vars = @()
foreach ($v in ($Variants -split ",")) {
    if ($v.Trim() -eq "") { continue }
    $label, $rest = $v -split "=", 2
    $core, $hostExe = $rest -split "\|", 2
    if (-not (Test-Path $core) -or -not (Test-Path $hostExe)) { throw "variant ${label}: $core or $hostExe not found" }
    $vars += [pscustomobject]@{ Label = $label; Core = (Resolve-Path $core).Path; Host = (Resolve-Path $hostExe).Path }
}
$log = Join-Path $Out "result.txt"
"compare_windows.ps1  $(Get-Date -Format s)  $($roms.Count) ROMs" | Tee-Object $log
(Get-CimInstance Win32_Processor | Select-Object -First 1).Name | Tee-Object $log -Append
foreach ($v in $vars) { "{0,-16} {1:N0} bytes  {2}" -f $v.Label, (Get-Item $v.Core).Length, $v.Core | Tee-Object $log -Append }

if (-not $SkipPictures) {
    "`n== 1. pictures after $PictureFrames frames (each ROM: all variants must have one hash) ==" | Tee-Object $log -Append
    $bad = 0
    foreach ($rom in $roms) {
        $hashes = @{}
        foreach ($v in $vars) {
            $ppm = Join-Path $Out ("pic_{0}_{1}.ppm" -f $v.Label, [IO.Path]::GetFileNameWithoutExtension($rom))
            & $v.Host $v.Core $rom --train $PictureFrames $ppm | Out-Null
            if ($LASTEXITCODE -ne 0) { $hashes[$v.Label] = "(did not run)"; continue }
            $hashes[$v.Label] = (Get-FileHash $ppm -Algorithm SHA256).Hash.Substring(0, 12)
            Remove-Item $ppm
        }
        $distinct = @($hashes.Values | Sort-Object -Unique)
        $state = if ($distinct.Count -eq 1) { "same" } else { $bad++; "DIFFERENT" }
        "{0,-44} {1}  {2}" -f (Split-Path -Leaf $rom), $state, (($hashes.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join " ") |
            Tee-Object $log -Append
    }
    "pictures: $bad ROM(s) differ" | Tee-Object $log -Append
}

"`n== 2. speed ($Rounds rounds, alternating order) ==" | Tee-Object $log -Append
$results = @{}
for ($r = 1; $r -le $Rounds; $r++) {
    $order = if ($r % 2) { $vars } else { $vars[($vars.Count - 1)..0] }
    foreach ($v in $order) {
        Write-Host "round $r : $($v.Label)"
        $lines = & $bench -Core $v.Core -HostExe $v.Host -Roms $roms -Label $v.Label
        $lines | Out-File (Join-Path $Out ("bench_r{0}_{1}.txt" -f $r, $v.Label))
        $res = ($lines | Where-Object { $_ -like "RESULT *" } | Select-Object -Last 1) -split " "
        "round $r  RESULT $($res[1]) $($res[2]) $($res[3])" | Tee-Object $log -Append
        if (-not $results.ContainsKey($v.Label)) { $results[$v.Label] = @() }
        $results[$v.Label] += [double]($res[2] -replace ",", "")
    }
}
"`n== summary (fps median per round; mean) ==" | Tee-Object $log -Append
$plainMean = $null
foreach ($v in $vars) {
    $vals = $results[$v.Label]
    $mean = ($vals | Measure-Object -Average).Average
    "{0,-16} {1}   mean {2:N1}" -f $v.Label, (($vals | ForEach-Object { "{0:N0}" -f $_ }) -join " / "), $mean | Tee-Object $log -Append
}
"`nSend $log (and the table of docs/pgo.md)." | Tee-Object $log -Append
