# Reference logs from Mesen

Two kinds: CPU trace logs for the regression suite (`regression/tests.txt`, below first), and reference audio.

## CPU trace logs (regression/)

A trace log must start at power-on and keep running for as long as the test needs. Three things make it come out
wrong:

- The file only gets the lines from the moment file logging starts (the Trace Logger's window also shows its
  in-memory history).
- A power cycle (or any reload of the ROM) **ends file logging without saying so**: Mesen replaces its debugger,
  closes the file after the current frame, and the Trace Logger keeps showing "logging" and scrolling new
  instructions. Such a log is one or two frames long. (Seen with Mesen 2.2.1, checked with strace.)
- Mesen pauses at the end of a frame when its window goes to the background (if that preference is on).

So: power cycle first, paused, and only then start logging:

1. *File → Open* the ROM, and pause (Esc).
2. Open *Debug → Trace Logger*.
3. *Game → Power Cycle*. With the Trace Logger open, Mesen stops at power-on (the debugger's "break on power
   cycle/reset", on by default); if it does not, pause straight away.
4. In the Trace Logger: set the file name and start logging to the file.
5. Resume (Esc) and let it run as long as the test does (most of blargg's tests: until the result is on the
   screen). Keep Mesen's game window in front, or turn off its pausing in the background in the preferences.
6. Pause, then stop logging.
7. Check the file before adding it:

       regression/check_log.py ROM ROM_mesen.txt

   It says whether it starts at power-on (at the reset vector), how many frames it covers, and whether it ended at a
   pause (scanline 239). A log that does not start at power-on cannot be fixed: record it again.

Pressing buttons while logging is fine: `regression/input_from_log.py ROM_mesen.txt -o ROM_input.txt` recovers what
the game read from the controllers, and the test then runs with `--input ROM_input.txt` in its `regression/tests.txt`
arguments (`one --input FILE` presses the same buttons at the same CPU cycles).

Turn off register labels in the Trace Logger's options: ONE writes `STA $2000 = $00`, Mesen with labels
`STA PpuControl_2000 = $00`. A log recorded with them can be fixed: `tools/mesen/strip_labels.py LOG...`
(rewrites it in place).

The menu names are from Mesen 2 and may differ in your version.

# Reference audio from Mesen

ONE's audio is checked against Mesen in two ways:

1. **APU state logs** (`apu_log.lua`): every frame, every value Mesen's APU exposes (each channel's timer,
   duty, envelope, sweep, length and linear counters, output level ...). ONE writes the same kind of log, and
   the two are compared line by line, like the CPU traces.
2. **WAV recordings**: what Mesen plays. Not compared byte for byte (Mesen filters and resamples in its own
   way), but frequencies, volumes and timing must match, and they are what to listen to side by side.

The menu names below are from Mesen 2.

## Once: settings and test ROMs

1. **Which Mesen**: *Help → About*. Note the version.
2. **Audio settings**, so the recordings are clean (*Settings → Audio*):
   - Sample rate: **48,000 Hz**
   - Master volume **100**, every channel's volume **100**, every channel's panning **centred**
   - **Off**: reverb, crossfeed, equalizer, and any other effect
3. **Lua file access** (*Debug → Script Window*, then in that window *Settings*): tick
   **Allow access to I/O and OS functions**. Without it the script cannot write its log file (it says so).
   It lets every script run commands and change files as you: turn it off again afterwards, and don't run scripts
   you haven't read while it is on.
4. **The `apu_mixer` test ROMs** (blargg): from <https://github.com/christopherpow/nes-test-roms>, folder
   `apu_mixer`: `square.nes`, `triangle.nes`, `noise.nes`, `dmc.nes`. Put them in `apu_mixer/` at the top of
   the ONE folder (next to `apu_test/`).

## For each ROM

Each recording starts at power-on, so that ONE can do exactly the same. Any Mesen 2 works; 2.2.1 has been checked.

1. *File → Open* the ROM. Let it run.
2. *Debug → Script Window*. In it: *File → Open*, choose `tools/mesen/apu_log.lua`, then **Run** (the ▶
   button or F5). The log at the bottom of the window says how many APU fields it found and where it writes.
3. *Tools → Sound Recorder → Record...* (or *Tools → Record Audio*): save as `<rom name>.wav`.
4. Straight away: **power cycle** (*Game → Power Cycle*, or *System → Power Cycle*). The script and the recorder
   keep running; both now start from power-on.
5. Wait about 35 seconds. The script stops by itself after 30 seconds of frames ("done, 1800 frames in ...").
6. Stop the recording (*Tools → Sound Recorder → Stop*).
7. Move both files (`apu_<rom name>.txt` and the `.wav`) into `test_roms/apu_reference/` at the top of the ONE folder.

To log a different length, change `FRAMES` at the top of `apu_log.lua` (60 frames = 1 second).

## Which ROMs

| ROM | Why | Length | Status |
|---|---|---|---|
| `apu_mixer/square.nes`, `triangle.nes`, `noise.nes`, `dmc.nes` | the mixer: a correct one plays near silence | the default 30 s | recorded |
| `apu_test/apu_test.nes`, `apu_test/rom_singles/1-len_ctr.nes` ... `8-dmc_rates.nes` | length counters, frame counter, DMC | 30 s | recorded |
| a game whose **title screen plays music by itself**, on a mapper ONE has (NROM, CNROM, MMC1), e.g. The Legend of Zelda, Metroid or Mega Man 2 (all MMC1) | real music: sweeps, envelopes and the triangle together | 30 s, no buttons pressed | wanted |
| a game with sampled drums or voices (DMC) on its title screen | the DMC playing real samples | 30 s, no buttons pressed | wanted |

Super Mario Bros. (and the Super Mario Bros. / Duck Hunt cartridge) is silent on its title screen, so it is no
use here unless buttons are pressed, which ONE cannot replay at the same moments. Punch-Out!! is fine but uses
the MMC2 mapper, which ONE does not have yet: its recording waits in `test_roms/apu_reference/` until it does.

The game ROMs and recordings stay on your computer (`test_roms/apu_reference/` is read by the tests but need not be
committed).
