# RAMBO-1 (mapper 64) hardware test ROMs, g1 to g6

Six ROMs that check a RAMBO-1 against what was measured on a real chip (a RAMBO-1 on a flash cartridge in a console, and on a jig). Each prints a list of tests on screen
(green when all pass, red otherwise; one beep for a pass, N beeps for failure code N) and puts its result in RAM, so that an emulator without a screen can read it:

| Address | Meaning |
|---|---|
| `$0300` | status: `$80` while running, `$00` all passed, otherwise the code of the first failing test |
| `$0301-$0303` | `DE B0 61` (signature: the ROM has started) |
| `$0304...` | the report text, zero-terminated |

| ROM | Tests |
|---|---|
| `g1_power_on.nes` | the power-on state (needs a power cycle: a reset gives false failures) |
| `g2_banking.nes` | banking, bank widths, R8 / R9, unused indices, mirroring |
| `g3_bus.nes` | no bus conflict, read-modify-write, register aliasing, /ROMSEL, open bus |
| `g4_cpu_irq.nes` | the CPU-cycle IRQ, to one cycle |
| `g5_a12_irq.nes` | the A12-mode IRQ: the filter, latch timing, mode switches |
| `g6_raster.nes` | rendering: the interrupt line, PPU configurations, repeat counts, a raster demo (about 380 frames) |

They were designed to fail in emulators that copy the NESdev description of the chip (Mesen, for one, fails several), so a failure in another emulator is expected where it differs from the
chip. ONE passes all six (against the research as of its commit `544ca1b`: G4's test 13 checks finding F-087, and the mapper has F-087 and F-088): ctest `rambo1hw_g1_power_on` ... `rambo1hw_g6_raster` run each in `one-debugger --batch` for 510 frames and check the signature and that the status is `$00`.
The INFO lines some of them print (G4 `07 17 03`, G5 `10 FC 07`, G6 `00 28 98` and `01 00 00`) are discovery output and are the console's values.

The `.nes` files here are the built ROMs (iNES 2.0, mapper 64, 256 KiB PRG and 256 KiB CHR each; they compress to a few KB). To rebuild them (cc65 and python3 are needed):

    cd tests/rambo1hw && mkdir -p build && make build/g1_power_on.nes build/g2_banking.nes build/g3_bus.nes build/g4_cpu_irq.nes build/g5_a12_irq.nes build/g6_raster.nes

(The Makefile also lists two probe ROMs for the measuring jig, `timing_probe` and `delay_probe`; they are not in this directory, so name the six targets as above instead of running a bare `make`.)
The result is byte-identical to the `.nes` files here.
