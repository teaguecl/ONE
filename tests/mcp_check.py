#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""ONE's MCP server (tools/mcp/one_mcp.py) end to end, through an MCP client, against nestest.

    python3 mcp_check.py ONE_DEBUGGER ROM          (the Python must have the MCP SDK: pip install mcp)

Starts the adapter (--headless: it launches one-debugger --batch --remote), calls its tools as an agent would, and
prints "mcp ok" at the end. The ROM is copied to a temporary directory (save states go next to it).
"""
import asyncio
import base64
import os
import shutil
import sys
import tempfile

try:
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
except ImportError:
    print("skipped: this Python has no MCP SDK (pip install mcp)")
    sys.exit(77)

HERE = os.path.dirname(os.path.abspath(__file__))
ADAPTER = os.path.join(os.path.dirname(HERE), "tools", "mcp", "one_mcp.py")
failures = []


def check(condition, what):
    if not condition:
        failures.append(what)
        print("FAILED:", what)


def text_of(result):
    return "\n".join(getattr(c, "text", "") for c in result.content)


async def run(debugger, rom):
    # (a short directory: on Linux the socket's path is in it)
    work = tempfile.mkdtemp(prefix="one-mcp-", dir="/tmp" if os.path.isdir("/tmp") else None)
    runtime = os.path.join(work, "run")
    os.mkdir(runtime, 0o700)
    game = os.path.join(work, "nestest.nes")
    shutil.copy(rom, game)
    # (the debugger and the adapter keep the token and socket files in XDG_RUNTIME_DIR; on Windows, in LOCALAPPDATA)
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime, LOCALAPPDATA=runtime, SDL_VIDEODRIVER="dummy",
               SDL_AUDIODRIVER="dummy")
    params = StdioServerParameters(command=sys.executable, args=[ADAPTER, "--debugger", debugger, "--headless"],
                                   env=env)
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            async def tool(tool_name, /, **arguments):
                result = await session.call_tool(tool_name, arguments)
                return result, text_of(result)

            names = {t.name for t in (await session.list_tools()).tools}
            for name in ["load_rom", "run_frames", "read_memory", "write_memory", "disassemble", "set_breakpoint",
                         "wait_for_breakpoint", "wait_for_user", "screenshot", "cheat_search", "game_genie",
                         "debugger_command", "set_controller", "set_zapper", "save_state", "load_state"]:
                check(name in names, f"the tool {name}")

            _, text = await tool("get_status")
            check("no debugger" in text, f"a tool before load_rom says what to do: {text!r}")
            _, text = await tool("load_rom", path=game)
            check("started one-debugger" in text and "stopped" in text, f"load_rom launches: {text!r}")
            _, text = await tool("run_frames", count=3)
            check("Frame" in text and "scanline 241" in text, f"run_frames: {text!r}")
            await tool("write_memory", address="$0300", data="5a 5b")
            _, text = await tool("read_memory", address="0x300", length=2)
            check("$0300: 5A 5B" in text, f"write_memory / read_memory: {text!r}")
            _, text = await tool("disassemble", address="$C000", count=2)
            check("JMP" in text, f"disassemble: {text!r}")

            # labels work wherever an address does (the debugger resolves them: hex first, then labels)
            await tool("label", address="$0300", name="scratch")
            _, text = await tool("read_memory", address="scratch", length=2)
            check("$0300: 5A 5B" in text, f"read_memory by label: {text!r}")
            await tool("write_memory", address="scratch", data="11")
            _, text = await tool("read_memory", address="$0300", length=2)
            check("$0300: 11 5B" in text, f"write_memory by label: {text!r}")
            _, text = await tool("set_breakpoint", kind="write", address="scratch")
            _, text = await tool("list_breakpoints")
            check("0300" in text, f"set_breakpoint by label: {text!r}")
            await tool("remove_breakpoint", number="*")
            _, text = await tool("read_memory", address="nosuchlabel")
            check("Unknown symbol" in text, f"an unknown label says so: {text!r}")
            await tool("label", address="$0300")  # (no name: removed again)

            _, text = await tool("set_breakpoint", kind="exec", address="$C28F")
            _, text = await tool("wait_for_breakpoint", timeout=20)
            check("BPX" in text and "PC=$C28F" in text, f"wait_for_breakpoint: {text!r}")
            _, text = await tool("set_breakpoint", kind="write", address="$0300", condition="os.exit()")
            check("not over the remote channel" in text, f"a Lua condition is refused: {text!r}")
            await tool("remove_breakpoint")
            _, text = await tool("list_breakpoints")
            check("BPX" not in text, f"remove_breakpoint: {text!r}")

            _, text = await tool("set_controller", buttons="start", frames=2)
            check("stopped" in text, f"set_controller runs the frames: {text!r}")
            _, text = await tool("set_zapper", x=100, y=90)
            check("Zapper: pointing at 100,90" in text, f"set_zapper aims: {text!r}")
            _, text = await tool("set_zapper", unplug=True)
            check("unplugged" in text, f"set_zapper unplugs: {text!r}")
            result, _ = await tool("screenshot")
            check(any(getattr(c, "type", "") == "image" and base64.b64decode(c.data)[:8] == b"\x89PNG\r\n\x1a\n"
                      for c in result.content), "screenshot is a PNG image")
            _, text = await tool("cdl", action="on")
            check("on" in text, f"cdl on: {text!r}")
            _, text = await tool("backtrace")
            check("#0" in text, f"backtrace: {text!r}")
            _, text = await tool("cdl", action="save /tmp/x.cdl")
            check("with a file name" in text, f"cdl save FILE is refused over the channel: {text!r}")
            _, text = await tool("cheat_search", action="start")
            check("Searching" in text, f"cheat_search: {text!r}")
            _, text = await tool("game_genie", args="SXIOPO")
            check("$91D9" in text, f"game_genie: {text!r}")
            _, text = await tool("debugger_command", line="lua x.lua")
            check("not over the remote channel" in text, f"debugger_command refuses lua: {text!r}")

            _, text = await tool("save_state", slot=2)
            check("Saved to slot 2" in text, f"save_state: {text!r}")
            _, text = await tool("load_state", slot=2)
            check("Loaded slot 2" in text, f"load_state: {text!r}")

            await tool("resume")  # running: wait_for_user must stop it and leave it stopped until the user starts it
            _, text = await tool("wait_for_user", message="Please beat the boss", timeout=1)
            check("timed out" in text and "never started" in text and "stopped" in text,
                  f"wait_for_user stops the game and times out without a user: {text!r}")
            _, text = await tool("pause")
            check("stopped" in text, f"pause: {text!r}")

            guide = await session.read_resource("one://guide/game-genie")
            check("Game Genie" in guide.contents[0].text, "the guide resource")
            help_text = await session.read_resource("one://help")
            check("bpx" in help_text.contents[0].text, "the help resource")
            await tool("debugger_command", line="q")
    shutil.rmtree(work, ignore_errors=True)


def main():
    asyncio.run(run(sys.argv[1], sys.argv[2]))
    if failures:
        print(f"{len(failures)} MCP checks failed")
        sys.exit(1)
    print("mcp ok")


if __name__ == "__main__":
    main()
