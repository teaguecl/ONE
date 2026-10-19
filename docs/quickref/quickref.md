---
title: one-debugger Quick Reference
lang: en
---

<!--
  Source of the one-debugger quick reference.  build.sh turns it into one-debugger-quickref.pdf:
    a line "@include NAME" in an HTML comment is replaced by a fragment that extract.sh pulled out of the debugger
    source; a line "@screen FILE N@ROW,COL ..." by a screen capture (capture.sh) with numbered callouts.
  Everything else is ordinary Pandoc Markdown; the ::: blocks are the panels of the fold-out (quickref.css).
  The @essential line chooses which commands get their full help text on the first page:
  @essential x t p g bpx bl bc d r
-->

::::::::: {.page .outside}

::::: {.panel .basics}

::: {.bar}
one-debugger Basics
:::

:::: {.twocol}

::: {.col}
## Starting the debugger

1. Install the tools (Debian, Ubuntu, Linux Mint):\
   `sudo apt install cmake libsdl2-dev libncurses-dev`
2. Build the emulator and the debugger:\
   `cmake --preset linux-debugger`\
   `cmake --build --preset linux-debugger`
3. Run it: `one-debugger game.nes` (or with no game: **F3** / File > Open game, or `load game.nes`)\
   The debugger stops at the first instruction after RESET. The game runs in its own window; the debugger
   uses the terminal.
4. Press **F5** to run, **Ctrl+D** to stop again.
:::

::: {.col}
## Debugging your own program

1. Link with debug info: `ld65 ... --dbgfile game.dbg`
2. Load the labels: `one-debugger -l game.dbg game.nes`\
   Labels show up in the code window and work wherever an address does: `bpx nmi`, `g main`.
   `lsave` saves them, with yours from `n`, in `game.mlb`, which loads by itself.
3. Put commands you always type in `game.init` next to the ROM: it runs at start-up (or `-s setup.txt`).
4. Pick the colors: `--theme borland`, or a config file (`one-debugger --write-config FILE`).
:::

::::

## Essential Commands

Type commands in the command window; they are not case sensitive. Addresses are hexadecimal, with or without
`$`, or a label. While the game runs, **Ctrl+D** (or **F5**) breaks back into the debugger.

::: {.essential}
<!-- @include essential -->
:::

:::::

::::: {.panel .cover}

::: {.title}
[ONE]{.brand} [one-debugger]{.product}\
[Quick Reference]{.subtitle}
:::

<!-- @screen images/screen-softice.ansi 1@1,35 2@1,79 3@5,33 4@5,83 5@15,54 6@25,58 A@21,72 B@19,72 C@18,22 D@4,10 E@33,98 -->

:::: {.legend}

::: {.col}
<!-- @include windows -->
:::

::: {.col}
[A]{.badge} The current instruction (PC), with what it will do\
[B]{.badge} A breakpoint (F9 toggles one at the cursor)\
[C]{.badge} A label from `-l` or `n`\
[D]{.badge} Values that changed since the last stop\
[E]{.badge} Status line: the function keys
:::

::::

::: {.group}
### Config Files
The debugger's settings (color theme, colors, window shadows): `--config FILE`, else `./one-debugger.cfg`,
else `~/.config/one/debugger.cfg`. The emulator's settings (controllers, sound, hotkeys): `--one-config FILE`,
else `./one.cfg`, else `~/.config/one/one.cfg`. `--write-config FILE` writes every debugger setting with its
default and a description.
:::

::: {.footer}
ONE NES emulator --- generated from the debugger source by `docs/quickref/build.sh`, <!-- @include version -->
:::

:::::

:::::::::

::::::::: {.page .inside}

::: {.bar}
one-debugger Commands
:::

::: {.subtitle-line}
Complete list. In the debugger, `help` lists the commands and `help command` explains one.
:::

::::: {.cmdcols}

<!-- @include commands -->

::: {.group}
### Function Keys
<!-- @include keys -->
:::

::: {.group}
### Window Numbers
<!-- @include windows -->
:::

::: {.group}
### In a Code or Memory Window
<div class="kv"><span class="key">F6, ec</span><span>Code window: enter / exit (SoftICE EC)</span></div>
<div class="kv"><span class="key">Up, Down</span><span>Move the cursor / scroll one line</span></div>
<div class="kv"><span class="key">PgUp, PgDn</span><span>Scroll one page</span></div>
<div class="kv"><span class="key">Home</span><span>Code: back to PC. Memory: to $0000</span></div>
<div class="kv"><span class="key">Wheel</span><span>Scroll</span></div>
<div class="kv"><span class="key">Dbl-click</span><span>Code: toggle a breakpoint</span></div>
<div class="kv"><span class="key">;</span><span>Code: comment the cursor line (edit, Enter)</span></div>
<div class="kv"><span class="key">Up, Down</span><span>Command window: command history</span></div>
<div class="kv"><span class="key">Left, Right</span><span>Command line: move the cursor (Ctrl: by word; Home, End)</span></div>
<div class="kv"><span class="key">Typing</span><span>Always goes to the command line</span></div>
<div class="kv"><span class="key">Tab</span><span>Complete a command or cs word</span></div>
:::

::: {.group}
### In the Watch Window
<div class="kv"><span class="key">watch, ww</span><span>Opens when a watch is added (or ww); closes when the last goes</span></div>
<div class="kv"><span class="key">Up, Down</span><span>Select a watch</span></div>
<div class="kv"><span class="key">Del</span><span>Remove it (wc)</span></div>
<div class="kv"><span class="key">Enter</span><span>Edit its value (eb on the command line)</span></div>
<div class="kv"><span class="key">Ins</span><span>Add a watch (watch on the command line)</span></div>
<div class="kv"><span class="key">Ctrl+F</span><span>Freeze / unfreeze it (F)</span></div>
:::

::: {.group}
### Addresses and Numbers
<div class="kv"><span class="key">C000, $C000</span><span>Addresses and values are hexadecimal</span></div>
<div class="kv"><span class="key">nmi_handler</span><span>A label works wherever an address does</span></div>
<div class="kv"><span class="key">t 10, t $10</span><span>Counts are decimal unless they start with $</span></div>
<div class="kv"><span class="key">gsl 241</span><span>Scanlines are decimal (0-261)</span></div>
<div class="kv"><span class="key">eb 300 = 1 2</span><span>'=' and ',' are optional separators</span></div>
:::

::: {.group}
### In the OAM Window
<div class="kv"><span class="key">A</span><span>All 64 sprites / the visible ones</span></div>
<div class="kv"><span class="key">H</span><span>The bytes / decoded</span></div>
<div class="kv"><span class="key">Enter</span><span>Edit the sprite (eo on the command line)</span></div>
:::

::: {.group}
### Scripting
<div class="kv"><span class="key">bpx a if cond</span><span>Stop when the Lua condition is true</span></div>
<div class="kv"><span class="key">bpx a do "cmds"</span><span>Commands run when it stops</span></div>
<div class="kv"><span class="key">source file</span><span>Commands from a file; game.init runs at start-up</span></div>
<div class="kv"><span class="key">debugger.command</span><span>In Lua: run a command, returns its output</span></div>
:::

::: {.group}
### Script Commands
Only in scripts (`-s`, `--batch`); a failed `!assert` makes the exit code 1.
<!-- @include script -->
:::

::: {.group}
### Command Line
`one-debugger [options] [ROM]`
<!-- @include options -->
:::


:::::

::: {.keybar}
<!-- @include keybar -->
:::

:::::::::
