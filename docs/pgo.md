# Profile-guided builds of the libretro core

A profile-guided (PGO) build of `one_libretro` is compiled twice: once with instrumentation, which is run on typical
games to record what is hot, then again using that record. The emulation does not change (the same pictures, the same
sound); only the compiler's choices do. For `one` it is 7-10% faster than a plain Release build; this page is how to
make and measure the libretro core's builds on every platform.

| Platform | Compiler | How |
|---|---|---|
| Linux | GCC (or Clang) | `bash tools/pgo.sh --libretro` -> `build/pgo-libretro/one_libretro.so` |
| Windows, cross-built on Linux | MinGW-w64 GCC, trained under Wine | `ONE_QUICKREF=0 bash tools/dev.sh bash tools/pgo.sh --libretro --windows` -> `build/pgo-libretro-windows/one_libretro.dll` |
| Windows, MSYS2 | MinGW-w64 GCC | `bash tools/pgo.sh --libretro` in an MSYS2 UCRT64 shell (below) |
| Windows, Visual Studio | MSVC | `powershell -File tools\pgo.ps1` in a Developer PowerShell (below) |

The training ROMs are the repository's test ROMs and every unzipped `commercial_games/*.nes` (tools/pgo.sh: 20
emulated seconds each; `PGO_SECONDS`, `PGO_ROMS`). The more varied the games, the better the profile. A profile
from one compiler can't be used by another: each build trains itself.

## MSYS2 GCC (Windows)

1. Install MSYS2 (https://www.msys2.org), open **MSYS2 UCRT64**, and run
   `pacman -S --needed git make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja`.
2. Get the source: `git clone ... one && cd one` (or copy the folder). Put your games, unzipped, in `commercial_games/`.
3. The profile-guided build (about the time of two builds plus the training):
   ```sh
   bash tools/pgo.sh --libretro build/msys2-pgo
   ```
   It prints `pgo.sh: done: .../one_libretro.dll`. The test host is `build/msys2-pgo/tests/libretro_host.exe`.
4. The plain build for comparison (same compiler, same options, no profile):
   ```sh
   cmake -S . -B build/msys2-plain -G Ninja -DCMAKE_BUILD_TYPE=Release -DONE_BUILD_LIBRETRO=ON -DONE_ENABLE_SDL2=OFF
   cmake --build build/msys2-plain --target one_libretro libretro_host
   ```

## MSVC (Visual Studio)

Tested with Visual Studio 2026 (MSVC 19.51) and CMake 4.3; if a step fails, send the whole output.

1. Visual Studio 2022 with the "Desktop development with C++" workload; CMake 3.21 or newer (the one that comes with
   Visual Studio is enough).
2. Open **Developer PowerShell for VS 2022** (so that `cl`, `link` and `pgort140.dll` are found), `cd` into the source.
3. The profile-guided build:
   ```powershell
   powershell -File tools\pgo.ps1 -BuildDir build\msvc-pgo
   ```
   It builds with `/GL`, links with `/LTCG /GENPROFILE`, runs the ROMs through `libretro_host.exe` (that writes
   `one_libretro!*.pgc` files), and links again with `/USEPROFILE`. The result: `build\msvc-pgo\Release\one_libretro.dll`;
   the host: `build\msvc-pgo\tests\Release\libretro_host.exe`.
   If the final link says it found no profile, run `pgomgr /merge build\msvc-pgo\pgo-profile\one_libretro.pgd` in the
   folder that holds the `.pgc` files and `pgd`, and run the last two lines of the script again.
4. Plain builds for comparison:
   ```powershell
   cmake -S . -B build\msvc-plain -A x64 -DONE_BUILD_LIBRETRO=ON -DONE_ENABLE_SDL2=OFF -DONE_BUILD_GUI=OFF
   cmake --build build\msvc-plain --config Release --target one_libretro libretro_host
   cmake -S . -B build\msvc-ltcg -A x64 -DONE_BUILD_LIBRETRO=ON -DONE_ENABLE_SDL2=OFF -DONE_BUILD_GUI=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
   cmake --build build\msvc-ltcg --config Release --target one_libretro libretro_host
   ```
   `msvc-ltcg` is the fair baseline for the PGO build (the same code generation without the profile).

## The cross-built DLL (Linux + Wine)

Build it on Linux as above. Copy `one_libretro.dll` and `tests/libretro_host.exe` from `build/pgo-libretro-windows/`
(and, for comparison, a plain cross build: `cmake --preset windows-cross-libretro && cmake --build --preset
windows-cross-libretro --target one_libretro libretro_host`) to the Windows machine. Wine is slow, but the profile only
records which code is hot, so the build is as good as one trained on Windows, if a little less representative.

## Comparing the builds

Variants to compare (any subset): `msys2-plain`, `msys2-pgo`, `msvc-plain`, `msvc-ltcg`, `msvc-pgo`,
`cross-plain`, `cross-pgo-wine`.

**Preparation.** Close other programs, plug in the machine, use the "High performance" power plan, and don't touch the
machine while it measures. Use the same ROM list for every variant (at least six games, with different mappers:
for example SMB, SMB3, Karnov, a MMC5 and a VRC game, plus `tests\data\nestest.nes`). Keep the unzipped ROMs in
one folder, say `C:\roms`.

**1. Check that the emulation did not change** (once per variant; a speedup that changes the picture is a bug).
With `nestest.nes` the host's own checks must pass:
```
libretro_host CORE tests\data\nestest.nes          ->  libretro_host: all passed
```
(the checks are written for nestest; on other games ignore them). Then, for at least three games, write the picture
after 600 frames and compare the files of the variants, which must be identical:
```
libretro_host.exe build\msvc-pgo\Release\one_libretro.dll C:\roms\smb.nes --train 600 smb_msvc-pgo.ppm
certutil -hashfile smb_msvc-pgo.ppm SHA256        (Windows)        sha256sum *.ppm        (MSYS2, Linux)
```

**2. Measure speed**, one variant after the other, and repeat the whole round three times, in this order:
`A B C D` `D C B A` `A B C D` (so slow drift of the machine, like heat, shows up instead of favoring a build):
```
bash tools/perf/bench_libretro.sh -n 5 -f 3600 -l msys2-pgo build/msys2-pgo/one_libretro.dll build/msys2-pgo/tests/libretro_host.exe /c/roms/*.nes
powershell -File tools\perf\bench_libretro.ps1 -Core build\msvc-pgo\Release\one_libretro.dll -HostExe build\msvc-pgo\tests\Release\libretro_host.exe -Roms (Get-ChildItem C:\roms\*.nes) -Label msvc-pgo
```
Each run is 5 timed runs of 3600 frames (one emulated minute) per ROM after a warm-up; it prints best and median
per ROM and ends with `RESULT label fps_median fps_best` (frames per second over all the ROMs).
Use the same script (the `.sh` in MSYS2, the `.ps1` for the MSVC builds) for the builds of one compiler; both measure
the same thing (wall time of `libretro_host --train`), so the numbers can be compared across the two.

**Both steps at once:** `tools\perf\compare_windows.ps1` runs steps 1 and 2 for any set of variants (all DLLs, whichever
compiler built them) and writes `result.txt`; see the header of the script.

**3. Report** (copy the table, fill it in, and send it with the output of the `bench` runs):

| variant | compiler (`gcc --version` / `cl`) | `all passed`? | picture hash equal? | RESULT fps (round 1 / 2 / 3) | fps best |
|---|---|---|---|---|---|
| msys2-plain | | | | | |
| msys2-pgo | | | | | |
| msvc-plain | | | | | |
| msvc-ltcg | | | | | |
| msvc-pgo | | | | | |
| cross-plain | | | | | |
| cross-pgo-wine | | | | | |

Also: the CPU (`Get-CimInstance Win32_Processor | Select Name`), Windows version, the ROM list, the commit of ONE
(`git rev-parse --short HEAD`), and the size of each DLL. The interesting numbers are the ratios: `pgo / plain` for
each compiler (7-10% is the expectation from `one`), and `msvc-pgo / msys2-pgo`.

## Results (2026-10-17)

Measured with `tools\perf\compare_windows.ps1`: 8 ROMs (nestest, SMB, SMB3, Kung Fu, Metroid, Batman, Battletoads,
Castlevania III), 5 timed runs of 3600 frames each, 3 rounds in the order A B C D, D C B A, A B C D. CPU: AMD Ryzen 7 5700G.
Windows 11, Visual Studio 2026 (MSVC 19.51), CMake 4.3. The three rounds agree within 1 fps. The picture after 600 frames was
identical in all variants for every ROM. The PGO builds were trained on 47 ROMs (20 emulated seconds each).

| variant | fps (mean of 3 rounds) | vs plain, same compiler | DLL size |
|---|---|---|---|
| msvc-plain | 293 | | 535 KB |
| msvc-ltcg | 315 | +7.6% | 594 KB |
| msvc-pgo | 365 | +24.5% (+15.7% vs LTCG) | 457 KB |
| msys2-plain | 322 | | 939 KB |
| msys2-pgo | 378 | +17.4% | 704 KB |
| cross-plain | 313 | | 1209 KB |
| cross-pgo-wine | 382 | +22.0% | 981 KB |

- PGO helps with every toolchain: +17% to +25% over the plain build, +16% over MSVC with LTCG alone. (Linux, libretro core:
  plain 330 fps, LTO 351, PGO 411.)
- The cross-built DLL trained under Wine is the fastest, about 1% ahead of the MSYS2 build trained natively on Windows:
  training under Wine loses nothing. Its profile came from sources slightly older than the ones that were measured.
- With PGO, MSVC is about 3.5% slower than GCC (365 vs 378 fps); without PGO about 9% (293 vs 322).
- The cross-built plain DLL is about 3% slower than the native MSYS2 plain one (313 vs 322); the cause was not looked into
  (compiler versions or flags).
- One CPU and one ROM set: read the ratios, not the exact numbers.
