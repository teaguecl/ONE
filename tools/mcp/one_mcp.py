#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""ONE's MCP server: lets an AI agent drive one-debugger.

    python3 tools/mcp/one_mcp.py [--debugger PATH] [--headless] [--attach [PID]] [--rom-dir DIR]...

It talks MCP on stdin/stdout and to one-debugger over its remote channel (one-debugger --remote: a Unix socket in
$XDG_RUNTIME_DIR/one-debugger/ and a token file next to it; on Windows TCP on 127.0.0.1, with PID.port and PID.token
in %LOCALAPPDATA%\\one-debugger\\; debugger/remote.hpp). Two ways to meet a debugger:
  - launch: load_rom(path) starts one-debugger --remote with the game (a window to watch and play in; --headless:
    none, the agent alone);
  - attach: the user started one-debugger --remote; the adapter finds it (the only one running, or --attach PID)
    and both use it: the user can play in the game window while the agent watches.

Claude Code:  claude mcp add one-debugger -- python3 /path/to/ONE/tools/mcp/one_mcp.py
Needs the official MCP SDK: pip install mcp   (version 1 or 2).

Only what the remote channel allows can be done: no Lua, no files the agent names (debugger/remote.cpp).
"""
import argparse
import asyncio
import contextvars
import copy
import functools
import inspect
import json
import os
import re
import shutil
import socket
import subprocess
import threading
import time

try:  # mcp 2
    from mcp.server.mcpserver import Image, MCPServer
except ImportError:
    try:  # mcp 1
        from mcp.server.fastmcp import FastMCP as MCPServer, Image  # type: ignore
    except ImportError:
        import sys
        sys.exit(f"one_mcp.py: {sys.executable} has no MCP SDK. Install it in a virtual environment:\n"
                 "  python3 -m venv ~/.local/share/one/mcp-venv && ~/.local/share/one/mcp-venv/bin/pip install mcp\n"
                 "and register the server with that Python (tools/mcp/README.md).")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FRAME_CYCLES = 29781  # CPU cycles in an NTSC frame, about


class DebuggerError(Exception):
    pass


def runtime_dir():
    if os.name == "nt":
        return os.path.join(os.environ.get("LOCALAPPDATA") or os.environ.get("TEMP", ""), "one-debugger")
    base = os.environ.get("XDG_RUNTIME_DIR", "")
    return os.path.join(base, "one-debugger") if base.startswith("/") else f"/tmp/one-debugger-{os.getuid()}"


def process_runs(pid):
    if os.name == "nt":  # (os.kill(pid, 0) would end the process here)
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.OpenProcess.restype = ctypes.c_void_p
        handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.GetLastError() != 87  # ERROR_INVALID_PARAMETER: no such process
        code = ctypes.c_ulong()
        running = not kernel32.GetExitCodeProcess(ctypes.c_void_p(handle), ctypes.byref(code)) or code.value == 259
        kernel32.CloseHandle(ctypes.c_void_p(handle))
        return running
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass
    return True


def running_debuggers():
    """{pid: where} of the one-debugger --remote processes that are listening: where is the Unix socket's path
    (PID.sock), or, for TCP, the file PID.port (Windows) that says 127.0.0.1:PORT"""
    found = {}
    directory = runtime_dir()
    try:
        names = os.listdir(directory)
    except OSError:
        return found
    for name in names:
        stem, extension = os.path.splitext(name)
        if extension in (".sock", ".port") and stem.isdigit() and process_runs(int(stem)):
            found[int(stem)] = os.path.join(directory, name)
    return found


LABEL = re.compile(r"^\.?[A-Za-z_][A-Za-z0-9_.]*$")


def parse_address(value):
    """768, "$0300", "0x300", "0300" (hex, as in the debugger) -> int; a label such as "energy" -> the name (the
    debugger resolves it: hex first, then labels, as on its command line)"""
    if isinstance(value, int):
        return value
    text = str(value).strip()
    lower = text.lower()
    for prefix in ("$", "0x"):
        if lower.startswith(prefix):
            lower = lower[len(prefix):]
    try:
        return int(lower, 16)
    except ValueError:
        if LABEL.match(text):
            return text
        raise DebuggerError(f"not an address: {value!r} (a number, hex like $0300, or a label)")


def address_text(value):
    """An address for a debugger command line: hex digits, or the label"""
    where = parse_address(value)
    return f"{where:04x}" if isinstance(where, int) else where


class Debugger:
    """The connection to one-debugger's remote channel (one request at a time)"""

    def __init__(self, options):
        self.options = options
        self.sock = None
        self.buffer = b""
        self.next_id = 1
        self.process = None  # (launched by us)
        self.pid = None  # the debugger we are connected to
        self.lock = threading.Lock()

    # --- meeting a debugger ---

    def connected(self):
        return self.sock is not None

    def connect(self, where, token_path, pid=None):
        """WHERE: a Unix socket's path, HOST:PORT, or a PID.port file (which has HOST:PORT)"""
        if where.endswith(".port"):
            with open(where) as f:
                where = f.read().strip()
        host_port = re.fullmatch(r"([0-9.]+):(\d+)", where)
        sock = socket.socket(socket.AF_INET if host_port else socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect((host_port.group(1), int(host_port.group(2))) if host_port else where)
        with open(token_path) as f:
            token = f.read().strip()
        self.sock, self.buffer = sock, b""
        stem = os.path.splitext(os.path.basename(token_path))[0]
        self.pid = pid or (int(stem) if stem.isdigit() else None)
        reply = self._exchange({"auth": token}, 10)
        if not reply.get("authorized"):
            self.close()
            raise DebuggerError(f"the debugger refused the token: {reply.get('error')}")

    def attach(self, pid=None):
        debuggers = running_debuggers()
        if pid is None:
            if len(debuggers) != 1:
                raise DebuggerError(
                    "no debugger to talk to: load_rom(path) starts one, or start one-debugger --remote yourself"
                    if not debuggers else
                    f"several debuggers are running (pids {sorted(debuggers)}): pass pid=PID to the tool, or start the adapter with --attach PID")
            pid = next(iter(debuggers))
        if pid not in debuggers:
            raise DebuggerError(f"no one-debugger --remote with pid {pid}")
        path = debuggers[pid]
        self.connect(path, os.path.splitext(path)[0] + ".token", pid)

    @staticmethod
    def detached():
        """Popen options: the debugger does not end with the adapter's terminal"""
        if os.name == "nt":
            return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        return {"start_new_session": True}

    def launch(self, rom):
        program = self.options.debugger
        dirs = self.options.rom_dir or [os.path.dirname(os.path.abspath(rom))]
        command = [program, "--remote", "--no-init"]
        for d in dirs:
            command += ["--remote-roms", d]
        if self.options.headless:
            command += ["--batch", "--log", os.devnull]
        command.append(rom)
        before = set(running_debuggers())
        started = time.monotonic()
        self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, text=True, **self.detached())
        line = []
        reader = threading.Thread(target=lambda: line.append(self.process.stdout.readline()), daemon=True)
        reader.start()
        reader.join(15)
        text = line[0].strip() if line else ""
        # the rest of its output (--batch prints every command) is drained, so that it never blocks
        threading.Thread(target=lambda: [None for _ in self.process.stdout], daemon=True).start()
        if text.startswith("remote: listening on ") and ", token " in text:
            where, token = text[len("remote: listening on "):].split(", token ", 1)
            self.connect(where, token, self.process.pid)
            return
        if self.process.poll() is not None:
            raise DebuggerError(f"one-debugger did not start: {text or 'exit code ' + str(self.process.returncode)}")
        # (the text UI may have started itself again in a terminal window: its own socket appears)
        while time.monotonic() - started < 15:
            new = set(running_debuggers()) - before
            if new:
                self.attach(max(new))
                return
            time.sleep(0.2)
        raise DebuggerError(f"one-debugger did not say where it listens ({text!r})")

    def ensure(self):
        if self.sock is None:
            self.attach(self.options.attach if self.options.attach else None)

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    # --- requests ---

    def _exchange(self, request, timeout):
        try:
            self.sock.settimeout(timeout)
            self.sock.sendall(json.dumps(request).encode() + b"\n")
            while b"\n" not in self.buffer:
                data = self.sock.recv(1 << 16)
                if not data:
                    raise ConnectionError("closed")
                self.buffer += data
        except (OSError, ConnectionError) as error:
            self.close()
            raise DebuggerError(f"lost the debugger ({error}); load_rom starts a new one, or attach again")
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def request(self, wait=None, **fields):
        """Send a request, return its reply (WAIT: seconds the debugger may take); DebuggerError if it was refused"""
        with self.lock:
            self.ensure()
            fields["id"] = self.next_id
            self.next_id += 1
            reply = self._exchange(fields, (wait if wait is not None else 10) + 15)
        if not reply.get("ok"):
            raise DebuggerError(reply.get("error", "refused"))
        return reply

    def command(self, line, timeout=None):
        fields = {"cmd": line}
        if timeout is not None:
            fields["timeout"] = timeout
        return self.request(wait=timeout, **fields)


class Debuggers:
    """Several debuggers at once: the tool's pid argument picks one (0: the default one, as before: the only debugger
    running, --attach PID, or the one load_rom started). Debugger's methods are reached through the selected one."""

    def __init__(self, options):
        self.options = options
        self.default = Debugger(options)
        self.others = {}
        self.chosen = contextvars.ContextVar("debugger", default=self.default)

    def select(self, pid):
        if not pid or self.default.pid == pid:
            return self.chosen.set(self.default)
        if pid not in self.others:
            self.others[pid] = Debugger(copy.copy(self.options))
            self.others[pid].options.attach = pid
        return self.chosen.set(self.others[pid])

    def __getattr__(self, name):
        return getattr(self.chosen.get(), name)

    def all(self):
        return [self.default, *self.others.values()]


def state_text(reply):
    s = reply.get("state", {})
    run = "running" if s.get("running") else "stopped"
    return (f"[{run}; PC=${s.get('pc')}; frame {s.get('frame')} (lag frames {s.get('lag_count')}), scanline {s.get('scanline')}; "
            f"cycle {s.get('cycle')}; {s.get('rom') or 'no game'}]")


def result(reply, extra=""):
    """A tool's text: what the debugger printed, then the state"""
    out = (reply.get("output") or "").rstrip("\n")
    if reply.get("timed_out"):
        out += ("\n" if out else "") + "(timed out: the game is still running; wait, pause or wait_for_breakpoint)"
    if extra:
        out += ("\n" if out else "") + extra
    return (out + "\n" if out else "") + state_text(reply)


GUIDE = """\
Finding a RAM variable and making a Game Genie code with ONE (example: player 1's health)

1. load_rom(path) (or attach to the user's debugger). Getting to a place where the value can change is often too
   hard to do with set_controller: use wait_for_user for it (see "Handing the game to the user" below).
2. Follow this script, one user action per wait_for_user call:
   a. wait_for_user("Play to where an enemy can hit Ryu. Do NOT get hit. Press F5 when ready"), with the player at full
      value. Then cheat_search("start").
   b. wait_for_user("Take exactly ONE hit, then press F5"). Then cheat_search("down") (it went down).
   c. Let time pass without a change (run_frames), cheat_search("same"). Repeat b and c until cheat_search("list")
      shows a few addresses. "= 3" when you know the value (hex), "up", "diff", "+1", "-1", "undo", "reset" also
      work. The list says which instruction last wrote each one ("written by $8108").
   Handing the game to the user: wait_for_user stops the game and shows the message; the user reads it at their own
   pace, starts the game with F5, plays, and stops it with F5 (or Ctrl+D). Never start the game yourself while
   the user is acting, and never put two steps in one message: keep each message short and say what to stop on.
3. Check: read_memory on the candidates, write_memory one and look at the screen (the health bar changes).
4. set_breakpoint("write", address) and play until it stops: it stops just after the instruction that wrote,
   and says where ("at 05:BAB7 (PRG bank 5 of 8, ...)" in bank-switched games). The writing instruction ends at
   that address: disassemble(address - 3, 1, bank=5), or - 2, - 1, to find it (DEC $65, or SBC then STA $65).
   rom_info shows the banks; read_memory and disassemble take a bank, mapped in or not; find_in_rom("ce 65 00")
   finds an instruction in every bank.
5. A Game Genie code replaces one ROM byte as the CPU reads it: game_genie("ADDRESS VALUE") encodes one. To stop a
   decrement, replace the opcode with one of the same length that does not write. DEC abs (CE) -> BIT abs (2C) and
   DEC zp (C6) -> BIT zp (24) work when the flags after the instruction are not used. When they are (a BEQ or BMI
   follows, as with SBC then STA $65 and a death check) BIT changes them: use LDA instead of STA (STA zp 85 -> LDA zp
   A5, STA abs 8D -> LDA abs AD), which keeps the old value's flags. In bank-switched games (the address shows a bank,
   "05:BAB7") add the compare value, the ROM byte there now: game_genie("BAB7 2C CE") gives an 8-letter code that
   only acts where that bank is mapped. game_genie says whether other banks have the same byte at that address
   (then the code would act there too: use another byte of the instruction, or another instruction).
   game_genie("on CODE description") applies it; test it by playing (and game_genie("db") lists the codes
   already known for the game).
"""


def build_server(debugger):
    server = MCPServer("one-debugger", instructions=(
        "Drive ONE's NES debugger (one-debugger): load a game, run frames, press buttons, read and search memory, set "
        "breakpoints, disassemble, make Game Genie codes. Start with load_rom (or attach to the user's debugger: "
        "any tool connects). With several debuggers running, every tool takes pid=PID (list_debuggers) to pick one. Addresses are hex strings like \"$0300\", numbers, or labels (see the label tool). The resource "
        "one://guide/game-genie explains finding a RAM variable and making a code; one://help lists the "
        "debugger's commands (for debugger_command)."))

    def tool():
        """server.tool(), plus a pid argument on every tool: which one-debugger (list_debuggers) to talk to"""
        register = server.tool()

        def decorate(function):
            @functools.wraps(function)
            async def wrapper(*args, pid: int = 0, **kwargs):
                token = debugger.select(pid)
                try:
                    return await function(*args, **kwargs)
                finally:
                    debugger.chosen.reset(token)
            signature = inspect.signature(function)
            wrapper.__signature__ = signature.replace(parameters=[*signature.parameters.values(), inspect.Parameter(
                "pid", inspect.Parameter.POSITIONAL_OR_KEYWORD, default=0, annotation=int)])
            wrapper.__annotations__ = {**function.__annotations__, "pid": int}
            return register(wrapper)
        return decorate

    async def call(function, *args, **kwargs):
        try:
            return await asyncio.to_thread(function, *args, **kwargs)
        except DebuggerError as error:
            return f"error: {error}"

    def command_text(line, timeout=None):
        return result(debugger.command(line, timeout))

    # --- the game ---

    @tool()
    async def list_debuggers() -> str:
        """The one-debugger --remote processes running (pids). With several, give any tool pid=PID to choose; without
        pid it uses the only one running (or the one the server was started with --attach for, or load_rom started)."""
        found = running_debuggers()
        if not found:
            return "no one-debugger --remote running"
        return "\n".join(f"pid {pid}" for pid in sorted(found))

    @tool()
    async def load_rom(path: str) -> str:
        """Load a NES game (.nes, or a .zip holding one). Starts one-debugger with it if none is running (launch mode); otherwise the
        debugger loads it (only from the ROM directories it was started with). The game stops at its first
        instruction."""
        def run():
            rom = os.path.abspath(os.path.expanduser(path))
            if not os.path.isfile(rom):
                raise DebuggerError(f"no file {rom}")
            if not debugger.connected():
                try:
                    debugger.attach(debugger.options.attach if debugger.options.attach else None)
                except DebuggerError:
                    debugger.launch(rom)
                    return result(debugger.request(op="state"), f"started one-debugger with {os.path.basename(rom)}")
            return command_text(f'load {rom}')
        return await call(run)

    @tool()
    async def reset(hard: bool = False) -> str:
        """Press the NES's RESET button (hard: power cycle). Stops at the reset vector."""
        return await call(command_text, "reset hard" if hard else "reset")

    @tool()
    async def get_status() -> str:
        """Whether the game runs, the CPU registers, frame and scanline."""
        def run():
            state = debugger.request(op="state")
            if state["state"].get("running"):
                return state_text(state)
            return command_text("r")
        return await call(run)

    @tool()
    async def pause() -> str:
        """Stop the game (it can be resumed)."""
        return await call(lambda: result(debugger.request(op="pause")))

    @tool()
    async def resume() -> str:
        """Let the game run (until a breakpoint, pause, or the user stops it). Returns at once."""
        return await call(command_text, "x", 0)

    @tool()
    async def run_frames(count: int = 1, timeout: float = 30) -> str:
        """Run COUNT frames (1 frame = 1/60 s) and stop at the start of vblank. A breakpoint stops it earlier."""
        return await call(command_text, f"fs {int(count)}", timeout)

    @tool()
    async def step(count: int = 1, kind: str = "into") -> str:
        """Step instructions: kind "into" (COUNT instructions), "over" (over a JSR), "out" (to the RTS/RTI)."""
        line = {"into": f"t {int(count)}", "over": "p", "out": "p ret"}.get(kind)
        if line is None:
            return "error: kind is into, over or out"
        return await call(command_text, line, 30)

    @tool()
    async def set_controller(buttons: str, frames: int = 2, port: int = 1, run: bool = True) -> str:
        """Hold controller BUTTONS ("a", "b", "select", "start", "up", "down", "left", "right", joined by "+": "a+right";
        "-" for none) on pad PORT (1 or 2) for FRAMES frames. run: also run those frames now (else the next
        run_frames/resume runs them)."""
        def go():
            pad = "p2 " if int(port) == 2 else ""
            text = command_text(f"press {pad}{buttons} {int(frames)}")
            if run:
                text = command_text(f"fs {int(frames)}", max(10.0, frames / 30))
            return text
        return await call(go)

    @tool()
    async def set_zapper(x: int = -1, y: int = -1, fire: bool = False, frames: int = 2, run: bool = True,
                         unplug: bool = False) -> str:
        """Aim the Zapper (light gun) on controller port 2 at the NES pixel X, Y (0-255, 0-239; leave both -1 to point away
        from the screen) and optionally pull the trigger for FRAMES frames (fire). It stays aimed until the next call.
        run: with fire, also run those frames now. unplug: take the Zapper out again (controller 2 returns). Light gun
        games (Duck Hunt, Hogan's Alley ...) see light only where the picture is bright (white target on black) while the beam
        passes the aim point."""
        def go():
            if unplug:
                return command_text("zapper none")
            where = "off" if x < 0 or y < 0 else f"{int(x)} {int(y)}"
            text = command_text(f"zapper {where}" + (f" fire {int(frames)}" if fire else ""))
            if fire and run:
                text = command_text(f"fs {int(frames)}", max(10.0, frames / 30))
            return text
        return await call(go)

    @tool()
    async def save_state(slot: int = 1) -> str:
        """Save the game in a slot (1-9, the same as the emulator's F1 slots). Runs to the next frame to do it."""
        return await call(command_text, f"savestate save {int(slot)}", 10)

    @tool()
    async def load_state(slot: int = 1) -> str:
        """Load the game from a slot (1-9)."""
        return await call(command_text, f"savestate load {int(slot)}")

    @tool()
    async def screenshot():
        """The game's picture (the last complete frame, 256x240)."""
        def run():
            import base64
            reply = debugger.request(op="screenshot")
            return Image(data=base64.b64decode(reply["png"]), format="png")
        return await call(run)

    # --- memory and code ---

    @tool()
    async def read_memory(address: str, length: int = 16, space: str = "cpu", bank: int = -1) -> str:
        """Read memory (no side effects): space "cpu" (the CPU's view, $0000-$FFFF, Game Genie codes included),
        "ppu" ($0000-$3FFF), "oam" (sprites, 0-$FF), "palette" (0-$1F) or "prg" (PRG-ROM by offset). bank: a PRG-ROM
        bank, mapped in or not, at ADDRESS $8000-$FFFF as it shows when mapped (rom_info gives the banks). Up to
        4096 bytes, as hex rows."""
        def run():
            start = parse_address(address)
            fields = {"op": "read", "space": "prg" if bank >= 0 else space, "address": start, "length": int(length)}
            if bank >= 0:
                fields["bank"] = int(bank)
            reply = debugger.request(**fields)
            data = bytes.fromhex(reply["data"])
            start = reply.get("address", start)  # (resolved by the debugger when ADDRESS was a label)
            rows = []
            for i in range(0, len(data), 16):
                rows.append(f"${start + i:04X}: " + " ".join(f"{b:02X}" for b in data[i:i + 16]))
            return "\n".join(rows) + "\n" + state_text(reply)
        return await call(run)

    @tool()
    async def write_memory(address: str, data: str) -> str:
        """Write bytes to CPU memory: DATA is hex bytes ("05" or "a5 00 ff"). Writes to ROM go to the mapper."""
        def run():
            values = data.replace(",", " ").split()
            if len(values) == 1 and len(values[0]) > 2:
                values = [values[0][i:i + 2] for i in range(0, len(values[0]), 2)]
            return command_text(f"eb {address_text(address)} = {' '.join(values)}")
        return await call(run)

    @tool()
    async def disassemble(address: str = "", count: int = 16, bank: int = -1) -> str:
        """Disassemble COUNT instructions from ADDRESS (default: PC). In bank-switched games the address column shows
        the PRG bank ("05:BAB7"); bank: decode that PRG-ROM bank at ADDRESS ($8000-$FFFF) even when it is not
        mapped in."""
        def run():
            start = parse_address(address) if address else int(debugger.request(op="state")["state"]["pc"], 16)
            fields = {"op": "disassemble", "address": start, "count": int(count)}
            if bank >= 0:
                fields["bank"] = int(bank)
            reply = debugger.request(**fields)
            return reply["lines"] + state_text(reply)
        return await call(run)

    @tool()
    async def rom_info() -> str:
        """PRG-ROM: its size, the mapper's bank size and number of banks, and which bank each window of $8000-$FFFF
        shows now."""
        def run():
            r = debugger.request(op="rominfo")
            if not r["bank_size"]:
                return f"PRG-ROM: {r['prg_size'] // 1024} KB, no PRG bank switching\n" + state_text(r)
            return (f"PRG-ROM: {r['prg_size'] // 1024} KB, {r['banks']} banks of {r['bank_size'] // 1024} KB; "
                    f"mapped now: {r['windows']} (address=bank)\n" + state_text(r))
        return await call(run)

    @tool()
    async def find_in_rom(data: str) -> str:
        """Find a byte sequence (hex: "ce 47 03" = DEC $0347) everywhere in PRG-ROM, every bank: where it is, as
        bank:address (the address where the bank shows when mapped in) and ROM offset."""
        def run():
            pattern = bytes.fromhex(data.replace(",", " "))
            if not pattern:
                raise DebuggerError("data: hex bytes, e.g. \"ce 47 03\"")
            rom = read_rom()
            info = debugger.request(op="rominfo")
            hits = []
            at = rom.find(pattern)
            while at >= 0 and len(hits) < 100:
                hits.append(rom_place(at, info))
                at = rom.find(pattern, at + 1)
            if not hits:
                return "not found"
            return f"{len(hits)} found{' (the first 100)' if len(hits) == 100 else ''}:\n" + "\n".join(hits)
        return await call(run)

    def read_rom():
        size = debugger.request(op="rominfo")["prg_size"]
        data = b""
        while len(data) < size:
            chunk = debugger.request(op="read", space="prg", address=len(data), length=min(4096, size - len(data)))
            data += bytes.fromhex(chunk["data"])
        return data

    def windows_of(info):
        """{window address: bank} from rominfo, and the window size"""
        mapped = {}
        for item in (info.get("windows") or "").split():
            where, bank = item.split("=")
            mapped[int(where, 16)] = int(bank)
        return mapped

    def rom_place(offset, info):
        size = info["bank_size"]
        if not size:
            return f"ROM offset ${offset:05X} (${0x8000 + offset % 0x8000:04X})"
        bank, inside = divmod(offset, size)
        windows = [w for w, b in windows_of(info).items() if b == bank]
        base = windows[0] if windows else 0x8000 + (inside // size) * size
        where = f"{bank:02X}:{base + inside:04X}" if windows else f"bank {bank}, +${inside:04X}"
        return f"{where}  (ROM offset ${offset:05X}{'' if windows else '; not mapped in now'})"

    def code_note(text):
        """What a decoded / encoded code does in a bank-switched game: the banks where it acts"""
        m = re.search(r"\$([0-9A-F]{4}) = ([0-9A-F]{2})(?: if (?:it reads )?([0-9A-F]{2}))?", text)
        if not m:
            return ""
        address, compare = int(m.group(1), 16), m.group(3)
        info = debugger.request(op="rominfo")
        size = info["bank_size"]
        if not size or address < 0x8000:
            return ""
        rom = read_rom()
        inside = address % size
        banks = len(rom) // size
        if compare is None:
            return (f"Note: a 6-letter code changes ${address:04X} whichever of the {banks} PRG banks is mapped there; "
                    "add the compare value (the ROM byte there) for an 8-letter code that acts on one bank only.")
        same = [b for b in range(banks) if rom[b * size + inside] == int(compare, 16)]
        if len(same) <= 1:
            return (f"Safe: only PRG bank {same[0]} has ${compare} at ${address:04X}: the code acts there only."
                    if same else f"Note: no PRG bank has ${compare} at ${address:04X}: the code never acts.")
        return (f"Careful: PRG banks {', '.join(map(str, same))} all have ${compare} at ${address:04X}: the code acts "
                "in each of them when it is mapped there. Pick an address whose byte is unique to the bank (another "
                "byte of the instruction).")

    @tool()
    async def label(address: str, name: str = "") -> str:
        """Name an address (kept with the game's labels; any tool's ADDRESS can then be the name, hex taking priority). No
        name: remove it."""
        return await call(command_text, f"n {address_text(address)} {name}".rstrip())

    # --- breakpoints ---

    @tool()
    async def set_breakpoint(kind: str, address: str, condition: str = "") -> str:
        """Stop the game when: kind "exec" (an instruction at ADDRESS runs), "read", "write", "access" (read or write
        of ADDRESS; stops after the instruction), or "opcode" (ADDRESS is the opcode, e.g. "9E"). condition: only
        when it is true, e.g. "a == 0" or "value < 3" (registers a x y s p pc, scanline, frame, mem[0x300],
        address/value of the access; numbers as 0x..)."""
        def run():
            where = address_text(address)
            line = {"exec": f"bpx {where}", "read": f"bpm {where} r", "write": f"bpm {where} w",
                    "access": f"bpm {where} rw", "opcode": f"bop {where}"}.get(kind)
            if line is None:
                raise DebuggerError("kind is exec, read, write, access or opcode")
            if condition:
                line += f" if {condition}"
            return command_text(line)
        return await call(run)

    @tool()
    async def remove_breakpoint(number: str = "*") -> str:
        """Remove breakpoint NUMBER (as list_breakpoints shows), or all ("*")."""
        return await call(command_text, f"bc {number}")

    @tool()
    async def list_breakpoints() -> str:
        """The breakpoints."""
        return await call(command_text, "bl")

    @tool()
    async def wait_for_breakpoint(timeout: float = 60) -> str:
        """Let the game run (if stopped) until a breakpoint stops it, or TIMEOUT seconds. Says which one and where."""
        def run():
            if not debugger.request(op="state")["state"].get("running"):
                reply = debugger.command("x", timeout)
                if not reply.get("timed_out"):
                    return result(reply)
            return result(debugger.request(wait=timeout, op="wait", timeout=timeout))
        return await call(run)

    # --- the user ---

    @tool()
    async def wait_for_user(message: str, timeout: float = 900) -> str:
        """Ask the user to do something in the game. The game is stopped and stays stopped while the user reads
        MESSAGE (shown in the debugger's command window, and a prompt on the game picture). The user starts it with
        F5, plays, and hands it back with F5 or Ctrl+D (or the gamepad's hand-back button if one is set); this
        returns then, or after TIMEOUT seconds. Ask for one action per call, in a short message, and say what to
        stop on: "Take ONE hit, then press F5". Never ask for several steps at once."""
        def run():
            deadline = time.monotonic() + timeout
            debugger.request(op="pause")  # always stopped: the user is never rushed
            debugger.request(op="notify", message=message, overlay="Agent: read the command window. F5 starts",
                             seconds=timeout)
            while not debugger.request(op="state")["state"].get("running"):
                if time.monotonic() >= deadline:
                    debugger.request(op="notify", message="", overlay="")
                    return result(debugger.request(op="state"), "(timed out: the user never started the game)")
                time.sleep(0.2)
            debugger.request(op="notify", message="", overlay="")  # the prompt goes while the user plays
            reply = debugger.request(wait=max(deadline - time.monotonic(), 1), op="wait",
                                     timeout=max(deadline - time.monotonic(), 1))
            return result(reply, "" if reply.get("timed_out") else "(the user stopped the game)")
        return await call(run)

    # --- ONE's own ---

    @tool()
    async def cheat_search(action: str) -> str:
        """Find a variable by how it changes (the debugger's cs): "start" (cpu RAM; "start ppu", "start oam"), then
        after letting the game run: "= 05", "!= 05", "< 05", "> 05" (hex), "same", "diff", "up", "down", "+1", "-1",
        "written", "notwritten", "read", "notread"; "list" shows the candidates, "undo", "reset", "stop"."""
        return await call(command_text, f"cs {action}")

    @tool()
    async def backtrace(jumps: bool = False) -> str:
        """The call stack: how the CPU got to where it is stopped (the debugger's bt). The routines called and not
        yet returned (JSR, NMI/IRQ), innermost first, with labels and PRG banks, and how the current instruction
        was reached (a branch or JMP). jumps: the last jumps and taken branches instead. Call tracking starts at the
        first backtrace (or cdl on): earlier calls, and calls before a state load, are guessed from the stack."""
        return await call(command_text, "bt jumps" if jumps else "bt")

    @tool()
    async def cdl(action: str = "stats") -> str:
        """The code/data log (the debugger's cdl): which PRG-ROM bytes ran as code, were read as data or jumped to,
        kept per bank. "on" / "off" / "clear"; "stats" (how much is known, per bank); "unseen [BANK [BYTES]]" (the
        ranges never seen: where to steer the game next); "save" / "load [force]" (the game's .cdl, Mesen's
        format). Switch it on, play, then look at stats and unseen."""
        return await call(command_text, f"cdl {action}")

    @tool()
    async def game_genie(args: str) -> str:
        """Game Genie codes (the debugger's gg): "SXIOPO" decodes; "91D9 AD [compare]" encodes (address, value, the
        ROM byte there for an 8-letter code); "on CODE [description]" applies a code; "off CODE|*", "delete CODE|*",
        "list", "enable", "disable"; "db [word]" lists the known codes for the game, "db N" applies one."""
        def run():
            reply = debugger.command(f"gg {args}")
            note = ""
            words = args.split()
            if words and words[0].lower() not in ("off", "delete", "list", "enable", "disable", "db"):
                note = code_note(reply.get("output", ""))
            return result(reply, note)
        return await call(run)

    @tool()
    async def set_cheats(codes: list[str]) -> str:
        """Apply exactly these Game Genie codes (others are unticked): ["SXIOPO", "AEUKZIGA", ...]."""
        def run():
            text = command_text("gg off *")
            notes = []
            for code in codes:
                reply = debugger.command(f"gg on {code}")
                text = result(reply)
                note = code_note(reply.get("output", ""))
                if note:
                    notes.append(f"{code}: {note}")
            return text + ("\n" + "\n".join(notes) if notes else "")
        return await call(run)

    @tool()
    async def debugger_command(line: str, timeout: float = 10) -> str:
        """Any debugger command the remote channel allows (one://help lists them), for what the other tools do not
        cover. Commands that run the game return when it stops, or after TIMEOUT seconds."""
        return await call(command_text, line, timeout)

    @server.resource("one://help", mime_type="text/plain")
    def help_text() -> str:
        """The debugger's commands (its help)."""
        try:
            return debugger.command("help").get("output", "")
        except DebuggerError as error:
            return f"(no debugger: {error})"

    @server.resource("one://guide/game-genie", mime_type="text/plain")
    def guide() -> str:
        """How to find a RAM variable and make a Game Genie code with these tools."""
        return GUIDE

    return server


def main():
    parser = argparse.ArgumentParser(description="ONE's MCP server (one-debugger for AI agents)")
    default = os.environ.get("ONE_DEBUGGER") or next(
        (p for p in [os.path.join(ROOT, "build", "linux-debugger", "debugger", "one-debugger"),
                     shutil.which("one-debugger")] if p and os.path.isfile(p)), "one-debugger")
    parser.add_argument("--debugger", default=default, help=f"one-debugger to launch (default {default})")
    parser.add_argument("--headless", action="store_true", help="launch it without windows (the agent alone)")
    parser.add_argument("--attach", type=int, nargs="?", const=0, default=None,
                        help="use a one-debugger --remote the user started (PID: which one, if several)")
    parser.add_argument("--rom-dir", action="append", help="where load_rom may load games from (repeatable; "
                        "default: the first game's directory)")
    options = parser.parse_args()
    debugger = Debuggers(options)
    server = build_server(debugger)
    try:
        server.run("stdio")
    finally:
        for each in debugger.all():
            each.close()
            if each.process is not None and each.process.poll() is None:
                each.process.terminate()


if __name__ == "__main__":
    main()
