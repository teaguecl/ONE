#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""MCP end to end: a scripted agent finds the health byte of tests/health/health.nes, the
instruction that decrements it and a Game Genie code that stops it, using only the MCP tools (tools/mcp/one_mcp.py).

    python3 health_check.py ONE_DEBUGGER HEALTH_ROM      (the Python must have the MCP SDK: pip install mcp)

Each step checks what an agent would conclude from the tools' text; prints "health ok" at the end. The game: pressing
A is a hit (health $0347 down by one, by bank 1's DEC $0347 at $8108); decoys change on their own (health.s).
"""
import asyncio
import os
import re
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
    return condition


async def run(debugger, rom):
    work = tempfile.mkdtemp(prefix="one-health-", dir="/tmp")
    runtime = os.path.join(work, "run")
    os.mkdir(runtime, 0o700)
    game = os.path.join(work, "health.nes")
    shutil.copy(rom, game)
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=os.path.join(work, "config"),
               SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    params = StdioServerParameters(command=sys.executable, args=[ADAPTER, "--debugger", debugger, "--headless"],
                                   env=env)
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            async def tool(name, **arguments):
                result = await session.call_tool(name, arguments)
                text = "\n".join(getattr(c, "text", "") for c in result.content)
                print(f"> {name}({', '.join(f'{k}={v!r}' for k, v in arguments.items())})\n  " +
                      text.strip().replace("\n", "\n  ")[:400])
                return text

            async def hit():
                await tool("set_controller", buttons="a", frames=2)
                await tool("run_frames", count=10)

            # 1. the health byte: down after a hit, the same while nothing happens
            await tool("load_rom", path=game)
            await tool("run_frames", count=10)
            await tool("cheat_search", action="start")
            for _ in range(3):
                await hit()
                await tool("cheat_search", action="down")
                await tool("run_frames", count=20)  # (the timer decoy goes down in this time: "same" drops it)
                await tool("cheat_search", action="same")
            listing = await tool("cheat_search", action="list")
            found = re.findall(r"^\s*\$([0-9A-F]{4})\s", listing, re.M)
            if not check(len(found) == 1, f"one candidate left: {found}"):
                return
            health = int(found[0], 16)
            check(health == 0x0347, f"the health byte is $0347: ${health:04X}")
            before = await tool("read_memory", address=f"${health:04X}", length=1)
            await hit()
            after = await tool("read_memory", address=f"${health:04X}", length=1)
            value = lambda text: int(re.search(r"\$[0-9A-F]{4}: ([0-9A-F]{2})", text).group(1), 16)
            check(value(after) == value(before) - 1, "a hit takes one")

            # 2. the instruction: a write breakpoint stops after it; it is the one just before PC
            await tool("set_breakpoint", kind="write", address=f"${health:04X}")
            await tool("set_controller", buttons="a", frames=2, run=False)
            stop = await tool("wait_for_breakpoint", timeout=20)
            m = re.search(r"at ([0-9A-F]{2}):([0-9A-F]{4}) \(PRG bank (\d+)", stop)
            if not check(m is not None, f"the stop says where, with the bank: {stop!r}"):
                return
            bank, pc = int(m.group(3)), int(m.group(2), 16)
            instruction = None
            for back in (3, 2, 1):
                lines = await tool("disassemble", address=f"${pc - back:04X}", count=1, bank=bank)
                first = lines.splitlines()[0]
                if f"${health:04X}" in first and int(first.split()[0].split(":")[-1], 16) + back == pc:
                    sizes = len(first.split("  ")[1].split())
                    if sizes == back:
                        instruction = (pc - back, first)
                        break
            if not check(instruction is not None, "the instruction before the stop writes the health byte"):
                return
            address, text = instruction
            check("DEC" in text and address == 0x8108 and bank == 1, f"DEC $0347 at 01:8108: {text!r}, bank {bank}")
            rom_bytes = await tool("read_memory", address=f"${address:04X}", length=3, bank=bank)
            opcode = int(rom_bytes.split(": ")[1].split()[0], 16)
            other = await tool("read_memory", address=f"${address:04X}", length=3, bank=2)
            check("AD 47 03" in other, f"bank 2 has other code there: {other!r}")
            places = await tool("find_in_rom", data="ce 47 03")
            check(places.startswith("1 found") and ("bank 1" in places or "01:8108" in places),
                  f"find_in_rom: one DEC $0347: {places!r}")
            await tool("remove_breakpoint")
            await tool("run_frames", count=5)  # (the stop came in the middle of the A press: let it go)

            # 3. the code: the opcode replaced by BIT abs (2C: reads, writes nothing), only where bank 1's byte is
            encoded = await tool("game_genie", args=f"{address:04X} 2C {opcode:02X}")
            code = re.search(r": ([APZLGITYEOXUKSVN]{8})", encoded)
            if not check(code is not None, f"an 8-letter code: {encoded!r}"):
                return
            check("Safe: only PRG bank 1" in encoded, f"the code acts on bank 1 only: {encoded!r}")
            careless = await tool("game_genie", args="9000 00 00")
            check("Careful" in careless, f"a compare value in several banks is pointed out: {careless!r}")
            six = await tool("game_genie", args=f"{address:04X} 2C")
            check("6-letter" in six, f"a 6-letter code in a banked game is pointed out: {six!r}")

            # 4. it works: hits no longer take health (and the game still counts them)
            applied = await tool("game_genie", args=f"on {code.group(1)} infinite health")
            check("Safe: only PRG bank 1" in applied, f"applying it says the same: {applied!r}")
            before = value(await tool("read_memory", address=f"${health:04X}", length=1))
            hits = int((await tool("read_memory", address="$0351", length=1)).split(": ")[1][:2], 16)
            for _ in range(3):
                await hit()
            after = value(await tool("read_memory", address=f"${health:04X}", length=1))
            hits_after = int((await tool("read_memory", address="$0351", length=1)).split(": ")[1][:2], 16)
            check(after == before and hits_after == hits + 3, f"with the code: health {before} -> {after}, "
                  f"hits {hits} -> {hits_after}")
            await tool("game_genie", args="off *")
            await hit()
            check(value(await tool("read_memory", address=f"${health:04X}", length=1)) == before - 1,
                  "without it a hit takes health again")
            await tool("debugger_command", line="q")
    shutil.rmtree(work, ignore_errors=True)


def main():
    asyncio.run(run(sys.argv[1], sys.argv[2]))
    if failures:
        print(f"{len(failures)} checks failed")
        sys.exit(1)
    print("health ok")


if __name__ == "__main__":
    main()
