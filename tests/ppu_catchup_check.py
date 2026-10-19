#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""PPU catch-up (FEATURE_PPU_CATCHUP) must not change anything the game can see: run
each ROM with and without it (ONE_NO_CATCHUP=1) to the same frame and compare the whole machine, as the two save state
files. The Mesen trace regression cannot do this: a trace turns catch-up off.

  ppu_catchup_check.py ONE [--all] [--made-up] [--reset] [--frames N,N,...] [ROM...]   (--reset: run the ROMs with RESET pressed; --made-up: only the made-up games)

With no ROM: the ROMs of the repository's test_roms/ (a selection; --all: every one), and the commercial games in
commercial_games/ if they are there. ROMs are copied to a scratch directory (games with battery RAM write a .sav)."""
import concurrent.futures, glob, os, shutil, subprocess, sys, tempfile

SELECTION = ("nestest.nes", "mmc3bigchrram.nes", "cpu_dummy_reads.nes", "test_cpu_exec_space_ppuio.nes",
             "01-vbl_basics.nes", "05-nmi_timing.nes", "07-nmi_on_timing.nes", "02-vbl_set_time.nes",
             "03-vbl_clear_time.nes", "04-nmi_control.nes", "02-alignment.nes", "03-corners.nes",
             "05-basics.nes", "01-basics.nes", "oam_read.nes", "oam_stress.nes", "ppu_open_bus.nes")
# ROMs that are also run with the RESET button pressed (-r: at CPU cycles counted from the previous reset, not at frame
# starts), so that a reset happens while the CPU is ahead of the PPU: the games reset, and the ROMs that test resets
RESET_PRESSES = ["-r", "599999", "-r", "1234567"]
RESET_ROMS = ("ram_after_reset.nes", "registers.nes", "cpu_reset.nes", "irq_flag_cleared.nes", "nestest.nes")
RESET_GAMES = ("Super Mario Bros. (World).nes", "Super Mario Bros. 3 (USA).nes", "Karnov (USA).nes")
GAMES = ("Super Mario Bros. 3 (USA).nes", "Karnov (USA).nes", "Metroid (USA).nes", "Punch-Out!! (USA).nes")
# MMC5 games, played with the buttons of their Mesen logs (commercial_games/*_input.txt) to frames in the game itself
INPUT_GAMES = (("Castlevania III - Dracula's Curse (USA).nes", "castlevania3_input.txt", (700, 1100)),
               ("Gemfire (USA).nes", "gemfire_input.txt", (600, 1000)),
               ("Bandit Kings of Ancient China (USA).nes", "banditkings_input.txt", (800, 1100)),
               ("Romance of the Three Kingdoms II (USA).nes", "romance3k2_input.txt", (800, 1100)),
               ("Hogan's Alley (World).zip", "hogansalley_input.txt", (560, 1130)),
               ("Gumshoe (USA, Europe).zip", "gumshoe_input.txt", (600, 1200)),
               ("Operation Wolf (USA) (Rev 0A).zip", "operationwolf_input.txt", (600, 1200)),
               ("Duck Hunt (World).nes", "duckhunt_input.txt", (540, 560, 900)))  # (+ the Zapper: shots at frames 525, 570)
# The Zapper test ROM, aimed as Mesen saw it (tests/zapper/zapper_input_mesen.txt): $4017 reads need the PPU where the CPU is
ZAPPER_RUN = ("zapper_free.nes", "zapper_input_mesen.txt", (70, 100, 130, 180))


# Made-up games for the two things catch-up must not get wrong, whatever the real games do: when the CPU takes an
# interrupt. Each main loop is `INC $01 / JMP` (8 cycles); a handler counts in $00 / $02 and adds the loop count at its
# start to a sum in $05 / $06 (a sum: an interrupt that came 8 or more cycles late at any time stays in the state, while
# the last value would not, as a save at a frame has the CPU exact for the frames just before it).
def ines(mapper, prg_banks_16k, prg_code_offset, code, vectors_offset, vectors, flags6=0):
    prg = bytearray(prg_banks_16k * 16384)
    prg[prg_code_offset:prg_code_offset + len(code)] = code
    prg[vectors_offset:vectors_offset + 6] = vectors
    header = bytes([0x4E, 0x45, 0x53, 0x1A, prg_banks_16k, 1, ((mapper & 15) << 4) | flags6, mapper & 0xF0]) + bytes(8)
    return header + bytes(prg) + bytes(8192)


def lo_hi(address):
    return [address & 255, address >> 8]


def spin_game(mapper, base, ppuctrl, ppumask, init, ack=(0x8D, 0x00, 0xE0, 0x8D, 0x01, 0xC0, 0x8D, 0x01, 0xE0)):
    """Code at BASE: reset, wait for two vblanks, INIT bytes, enable what PPUCTRL / PPUMASK say, CLI, loop. The
    interrupt handlers (NMI, IRQ) follow the loop; INIT is run before the loop and may set up an IRQ."""
    code = bytearray([0x78, 0xD8, 0xA2, 0xFF, 0x9A])  # SEI CLD LDX #$FF TXS
    code += bytes([0xA9, 0x40, 0x8D, 0x17, 0x40])  # the APU's frame IRQ off ($4017 = $40)
    code += bytes([0xA9, 0x00, 0x8D, 0x00, 0x20, 0x8D, 0x01, 0x20])  # picture and NMI off
    for _ in range(2):  # BIT $2002 / BPL back
        code += bytes([0x2C, 0x02, 0x20, 0x10, 0xFB])
    code += bytes([0xA9, ppumask, 0x8D, 0x01, 0x20])  # LDA #mask STA $2001
    code += bytes(init)
    code += bytes([0xA9, ppuctrl, 0x8D, 0x00, 0x20, 0x58])  # LDA #ctrl STA $2000, CLI
    loop = base + len(code)
    code += bytes([0xE6, 0x01, 0x4C]) + bytes(lo_hi(loop))  # INC $01 / JMP loop
    nmi = base + len(code)
    code += bytes([0xA5, 0x01, 0x18, 0x65, 0x05, 0x85, 0x05, 0xE6, 0x00, 0x40])  # $05 += $01 (the loop count: when) ...
    irq = base + len(code)
    code += bytes([0xA5, 0x01, 0x18, 0x65, 0x06, 0x85, 0x06])  # $06 += $01 (the loop count: when)
    code += bytes([0xE6, 0x02]) + bytes(ack) + bytes([0x40])  # INC $02, acknowledge (MMC3: $E000 / $C001 / $E001), RTI
    return bytes(code), nmi, irq


def made_up_games(directory):
    games = {}
    # NMI: NROM, NMI on, picture off then on
    code, nmi, irq = spin_game(0, 0xC000, 0x80, 0x18, [])
    games["nmi_spin"] = ines(0, 1, 0, code, 0x3FFA, lo_hi(nmi) + lo_hi(0xC000) + lo_hi(irq))
    # MMC3 IRQ every 8 lines (latch 7), NMI off, picture on: 8x8 sprites at $1000, and 8x16 sprites
    for name, ctrl in (("mmc3_irq_8x8", 0x08), ("mmc3_irq_8x16", 0x28)):
        init = [0xA9, 0x07, 0x8D, 0x00, 0xC0, 0x8D, 0x01, 0xC0, 0x8D, 0x01, 0xE0]  # latch 7, reload, enable
        code, nmi, irq = spin_game(4, 0xE000, ctrl, 0x18, init)
        games[name] = ines(4, 2, 0x6000, code, 0x7FFA, lo_hi(nmi) + lo_hi(0xE000) + lo_hi(irq), 0)
    # MMC5 scanline IRQ (compare value in line 10 / 100 / 239, once a frame), acknowledged by reading $5204; 8x8 and 8x16 sprites.
    # The code is in the last 8 KB bank (the MMC5 maps it at $E000 at power-on) of a 16 KB PRG
    for name, ctrl, target in (("mmc5_irq_10", 0x08, 10), ("mmc5_irq_100_8x16", 0x28, 100), ("mmc5_irq_239", 0x08, 239)):
        init = [0xA9, target, 0x8D, 0x03, 0x52, 0xA9, 0x80, 0x8D, 0x04, 0x52]  # compare value, IRQ on
        code, nmi, irq = spin_game(5, 0xE000, ctrl, 0x18, init, ack=(0xAD, 0x04, 0x52))
        games[name] = ines(5, 1, 0x2000, code, 0x3FFA, lo_hi(nmi) + lo_hi(0xE000) + lo_hi(irq), 0)
    paths = []
    for name, data in games.items():
        path = os.path.join(directory, name + ".nes")
        open(path, "wb").write(data)
        paths.append(path)
    return paths


def run(one, rom, state, frame, no_catchup, extra=()):
    env = dict(os.environ)
    env.pop("ONE_NO_CATCHUP", None)
    if no_catchup:
        env["ONE_NO_CATCHUP"] = "1"
    try:
        subprocess.run([one, "-H", "-u", "-S", "1500000", *extra, "--state-save", state, "--state-frame", str(frame), "-k",
                        "40000000", rom], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    except subprocess.TimeoutExpired:
        return TIMED_OUT
    return open(state, "rb").read() if os.path.exists(state) else None


TIMED_OUT = object()


def check(one, rom, extra, frames):
    """Compare one ROM at each frame in its own scratch directory (so jobs can run in parallel). Returns a list of
    (frame, outcome) with outcome "same", "different", "skipped" or "timeout"."""
    results = []
    with tempfile.TemporaryDirectory() as work:  # (games with battery RAM write a .sav next to the ROM)
        copy = os.path.join(work, "rom" + os.path.splitext(rom)[1])  # (a .zip stays one)
        shutil.copy(rom, copy)
        for frame in frames:
            for leftover in ("rom.sav", "a.ss", "b.ss"):
                if os.path.exists(os.path.join(work, leftover)):
                    os.remove(os.path.join(work, leftover))
            a = run(one, copy, os.path.join(work, "a.ss"), frame, True, extra)
            if os.path.exists(os.path.join(work, "rom.sav")):
                os.remove(os.path.join(work, "rom.sav"))
            b = run(one, copy, os.path.join(work, "b.ss"), frame, False, extra)
            if a is TIMED_OUT or b is TIMED_OUT:
                results.append((frame, "timeout"))
            elif a is None or b is None:
                results.append((frame, "skipped"))  # (the game never reached that frame)
            else:
                results.append((frame, "same" if a == b else "different"))
    return results


def main(argv):
    args = argv[1:]
    if not args:
        print(__doc__)
        return 2
    one = args.pop(0)
    everything = "--all" in args
    frames = [100, 300]
    roms = []
    args_roms = False
    resets = []
    press_reset = False
    only_made_up = False
    i = 0
    while i < len(args):
        if args[i] == "--all":
            pass
        elif args[i] == "--reset":
            press_reset = True
        elif args[i] == "--made-up":
            only_made_up = True
        elif args[i] == "--frames":
            i += 1
            frames = [int(n) for n in args[i].split(",")]
        else:
            roms.append(args[i])
            args_roms = True
        i += 1
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if roms and press_reset:
        resets, roms = roms, []
    if only_made_up:
        roms, resets = [], []
    elif not roms and not resets:
        found = glob.glob(os.path.join(root, "test_roms", "**", "*.nes"), recursive=True)
        roms = sorted(f for f in found if everything or os.path.basename(f) in SELECTION)
        roms += [os.path.join(root, "commercial_games", g) for g in GAMES
                 if os.path.exists(os.path.join(root, "commercial_games", g))]
        resets = sorted(f for f in found if os.path.basename(f) in RESET_ROMS)
        resets += [os.path.join(root, "commercial_games", g) for g in RESET_GAMES
                   if os.path.exists(os.path.join(root, "commercial_games", g))]
    same = bad = skipped = 0
    jobs = [(rom, (), frames) for rom in roms] + [(rom, tuple(RESET_PRESSES), frames) for rom in resets]
    if not args_roms and not only_made_up:
        for game, script, game_frames in INPUT_GAMES:
            rom, keys = os.path.join(root, "commercial_games", game), os.path.join(root, "commercial_games", script)
            if os.path.exists(rom) and os.path.exists(keys):
                jobs.append((rom, ("--input", keys), game_frames))
    if not args_roms and not only_made_up:
        rom, keys = (os.path.join(root, "tests", "zapper", f) for f in ZAPPER_RUN[:2])
        if os.path.exists(rom) and os.path.exists(keys):
            jobs.append((rom, ("--input", keys), ZAPPER_RUN[2]))
    workers = int(os.environ.get("JOBS", min(8, os.cpu_count() or 1)))
    with tempfile.TemporaryDirectory() as made_dir, concurrent.futures.ThreadPoolExecutor(workers) as pool:
        if not args_roms:  # (the default set also has the made-up games, with and without RESET)
            made = made_up_games(made_dir)
            jobs += [(rom, (), frames) for rom in made] + [(rom, tuple(RESET_PRESSES), frames) for rom in made]
        futures = [(rom, extra, pool.submit(check, one, rom, extra, job_frames)) for rom, extra, job_frames in jobs]
        for rom, extra, future in futures:
            for frame, outcome in future.result():
                if outcome == "same":
                    same += 1
                elif outcome == "skipped":
                    skipped += 1
                    print("not reached: %s at frame %d" % (os.path.basename(rom), frame))
                else:
                    bad += 1
                    print("%s: %s at frame %d%s" % ("DIFFERENT" if outcome == "different" else "TIMEOUT",
                                                   os.path.basename(rom), frame, " with RESET" if extra and extra[0] == "-r" else (" with the game's input" if extra else "")))
    print("ppu_catchup_check: %d identical, %d different, %d not reached" % (same, bad, skipped))
    if same == 0:
        print("ppu_catchup_check: nothing was compared")
        return 1
    return 1 if bad else 0


sys.exit(main(sys.argv))
