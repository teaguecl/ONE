# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
# Profile-guided build of the libretro core (one_libretro.dll) with MSVC: the PowerShell counterpart of
# tools/pgo.sh --libretro. Run it from an "x64 Native Tools Command Prompt for VS 2022" or a Developer PowerShell
# (so that cl, link and pgort140.dll, which the instrumented DLL needs while it runs, are found).
#
#   powershell -File tools\pgo.ps1 [-BuildDir build\pgo-msvc] [-Seconds 20] [-Roms a.nes,b.nes]
#
# 1. an instrumented one_libretro.dll (link-time /GENPROFILE, with /GL + /LTCG);
# 2. tests\libretro_host.exe runs the ROMs through it ("--train"), which writes the .pgc profile files;
# 3. one_libretro.dll is linked again with /USEPROFILE.
# The ROMs: -Roms, else the test ROMs of the repository and every commercial_games\*.nes (not .zip) there is.
# Tested with Visual Studio 2026 (MSVC 19.51), CMake 4.3, 2026-10-17: report problems with the full output.
param(
    [string]$BuildDir = "build\pgo-msvc",
    [int]$Seconds = 20,
    [string[]]$Roms = @(),
    [string]$Generator = ""   # default: the newest Visual Studio CMake finds; e.g. "Visual Studio 17 2022"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ($Roms.Count -eq 0) {
    $candidates = @("tests\data\nestest.nes", "tests\mmc1\mmc1.nes", "tests\cnrom\cnrom.nes",
                    "test_roms\apu_test\apu_test.nes", "test_roms\apu_mixer\square.nes", "test_roms\apu_mixer\dmc.nes",
                    "test_roms\AccuracyCoin\AccuracyCoin.nes")
    $Roms = @($candidates | Where-Object { Test-Path $_ })
    if (Test-Path commercial_games) { $Roms += @(Get-ChildItem commercial_games -Filter *.nes | ForEach-Object { $_.FullName }) }
}
if ($Roms.Count -eq 0) { throw "pgo.ps1: no training ROMs (use -Roms)" }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "pgo.ps1: git not found (CMake fetches Lua with it): install Git for Windows and reopen the shell"
}
function Invoke-Checked { param([scriptblock]$Cmd)
    & $Cmd
    if ($LASTEXITCODE -ne 0) { throw "pgo.ps1: failed (exit $LASTEXITCODE): $Cmd" }
}

$profile = Join-Path $BuildDir "pgo-profile"
$gen = @()
if ($Generator) { $gen += @("-G", $Generator) }
if (-not $Generator -or $Generator -like "Visual Studio*") { $gen += @("-A", "x64", "-T", "host=x64") }   # x64-hosted compiler/linker (a 32-bit host runs out of memory in LTCG)
$common = @("-S", ".", "-B", $BuildDir) + $gen + @("-DONE_BUILD_LIBRETRO=ON",
            "-DONE_ENABLE_SDL2=OFF", "-DONE_BUILD_GUI=OFF", "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON",
            "-DONE_PGO_DIR=$profile")

Write-Host "pgo.ps1: 1/3 instrumented build in $BuildDir"
# start clean: a cache from another generator (or a failed configure) would make CMake refuse or reuse it
if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
Invoke-Checked { cmake @common -DONE_PGO=generate }
if (Test-Path $profile) { Remove-Item -Recurse -Force $profile }
New-Item -ItemType Directory -Force $profile | Out-Null   # the linker writes the .pgd here and needs the directory
Invoke-Checked { cmake --build $BuildDir --config Release --target one_libretro libretro_host --parallel }

$dll = Join-Path (Resolve-Path $BuildDir) "Release\one_libretro.dll"
$host_exe = Join-Path (Resolve-Path $BuildDir) "tests\Release\libretro_host.exe"
# The instrumented DLL needs the PGO run-time (pgort140.dll or similar) next to it; it is in the compiler's bin
# directory, which a plain PowerShell does not have on PATH. Find it and copy it beside the DLL.
# (It must be the x64 one: a Developer PowerShell may be x86-hosted, and loading a 32-bit pgort gives error 193.)
$vcRoots = @()
if ($env:VCToolsInstallDir) { $vcRoots += $env:VCToolsInstallDir.TrimEnd('\') }
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -property installationPath
    if ($vs) { $vcRoots += @(Get-ChildItem (Join-Path $vs "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
                             Sort-Object Name -Descending | ForEach-Object { $_.FullName }) }
}
$rt = $null
foreach ($r in $vcRoots) {
    $rt = Get-ChildItem (Join-Path $r "bin\Host*\x64\pgort*.dll") -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($rt) { break }
}
if (-not $rt) { throw "pgo.ps1: pgort*.dll (the PGO run-time) not found: run from a Developer PowerShell for VS" }
Copy-Item $rt.FullName (Split-Path $dll) -Force
Copy-Item $rt.FullName (Split-Path $host_exe) -Force   # LoadLibrary looks in the program's directory
Write-Host "pgo.ps1: PGO run-time: $($rt.FullName)"
$trained = 0
Write-Host "pgo.ps1: 2/3 training: $($Roms.Count) ROMs, $Seconds emulated seconds each"
$work = Join-Path $env:TEMP "one_pgo_train"
New-Item -ItemType Directory -Force $work | Out-Null
foreach ($rom in $Roms) {
    Write-Host "  $(Split-Path -Leaf $rom)"
    Copy-Item $rom (Join-Path $work "train.nes") -Force
    & $host_exe $dll (Join-Path $work "train.nes") --train ($Seconds * 60) | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warning "$rom did not run (unsupported mapper?); skipped" } else { $trained++ }
}
if ($trained -eq 0) { throw "pgo.ps1: no ROM ran through the instrumented DLL (see the messages above)" }
# The run-time writes one_libretro!N.pgc next to the DLL; the optimising link looks for them next to the .pgd.
$pgc = @(Get-ChildItem (Split-Path $dll) -Filter "one_libretro!*.pgc")
if ($pgc.Count -eq 0) { throw "pgo.ps1: no .pgc profile files were written next to the DLL" }
$pgc | Move-Item -Destination $profile -Force
Write-Host "pgo.ps1: $($pgc.Count) profile files moved to $profile"

Write-Host "pgo.ps1: 3/3 optimised build"
Invoke-Checked { cmake @common -DONE_PGO=use }
Invoke-Checked { cmake --build $BuildDir --config Release --target one_libretro --parallel }
Write-Host "pgo.ps1: done: $dll"
