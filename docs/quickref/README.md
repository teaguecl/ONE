# one-debugger quick reference

`one-debugger-quickref.pdf` is a two-page landscape fold-out in the style of the SoftICE 3 quick reference card:
the basics and an annotated screen on the outside, every command, key and setting on the inside.

It is generated, mostly from the debugger's source code, so it stays in step with the debugger:

| File | What it is |
|---|---|
| `quickref.md` | The text (Pandoc Markdown). Placeholders in HTML comments mark where generated parts go: `@include NAME` and `@screen FILE N@ROW,COL ...`. The `@essential` line picks the commands explained on page 1. |
| `extract.sh` | Pulls the facts out of the source with grep/sed/awk: commands, their help text and groups (`debugger/commands.cpp`, grouped by its `// @group` comments), function keys, status line and window numbers (`debugger/imgui/app.cpp`, `app.hpp`), command-line options (`debugger/main.cpp`). |
| `images/screen-*.ansi` | Screen captures of the debugger: plain text with ANSI color codes, made by `capture.sh` (tmux). |
| `ansi2html.py` | Turns a capture into HTML, so the PDF shows real text instead of a bitmap, and adds the numbered callouts. |
| `quickref.css` | The layout: page size, panels, columns, colors. |
| `build.sh` | Runs all of the above: Markdown -> HTML (pandoc) -> PDF (Chrome/Chromium, headless). |

## Building

```sh
sudo apt install pandoc            # plus Chrome or Chromium (set CHROME=/path/to/chrome if it isn't on the PATH)
docs/quickref/build.sh             # writes docs/quickref/one-debugger-quickref.pdf
cmake --build build/linux-debugger --target quickref     # the same, written into the build directory
```

After the debugger's look changes (layout, colors), re-make the screen captures; they need tmux and a built
debugger: `docs/quickref/capture.sh build/linux-debugger/debugger/one-debugger tests/data/nestest.nes`.
The callout positions (`N@ROW,COL`, character cells counted from 0) are on the `@screen` line in `quickref.md`.

`KEEP=1 build.sh` keeps the intermediate files (generated Markdown and HTML) and prints where they are.
