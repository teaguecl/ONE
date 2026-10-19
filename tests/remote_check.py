#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 1999-2026 Chris Teague
# Part of ONE, a NES emulator: GPL, version 2 or later (see LICENSE).
"""The remote channel (debugger/remote.hpp) against nestest.

    remote_check.py [--wine] ONE_DEBUGGER ROM OTHER_ROM [BANKED_ROM]

Runs one-debugger --batch --remote (no window) with a private XDG_RUNTIME_DIR and checks: the token and socket files
(modes, gone after q); refused without the token, with a wrong one, a second client while one is connected, an HTTP
request on TCP; commands, a run that stops (fs), one that times out (x) and a pause; the allowlist (lua, conditions,
breakpoint actions, load outside the ROM directories); a screenshot; PRG-ROM reads by bank (BANKED_ROM: MMC1,
tests/health/health.nes). Prints "remote channel ok" at the end.

--wine: ONE_DEBUGGER is one-debugger.exe, run under Wine. Windows has no Unix socket: the channel is TCP on 127.0.0.1
(and the token file is in %LOCALAPPDATA%\\one-debugger, which winepath turns into the host's path), so the checks of
the socket file and of file modes are left out.
"""
import base64
import json
import os
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time

failures = []
WINE = False  # --wine


def check(condition, what):
    if not condition:
        failures.append(what)
        print("FAILED:", what)


class Client:
    def __init__(self, address):
        if isinstance(address, str):
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        else:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(20)
        self.sock.connect(address)
        self.buffer = b""
        self.next_id = 1

    def send_line(self, text):
        self.sock.sendall(text.encode() + b"\n")

    def read_line(self):
        """A reply, or None when the debugger closed the connection"""
        while b"\n" not in self.buffer:
            try:
                data = self.sock.recv(65536)
            except (ConnectionResetError, socket.timeout):
                return None
            if not data:
                return None
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def request(self, **fields):
        fields["id"] = self.next_id
        self.next_id += 1
        self.send_line(json.dumps(fields))
        reply = self.read_line()
        check(reply is not None and reply.get("id") == fields["id"], f"a reply to {fields}: {reply}")
        return reply or {}

    def closed(self):
        return self.read_line() is None

    def close(self):
        self.sock.close()


def host_path(path):
    """A path the debugger printed, as this system names it (under Wine: C:\\users\\... to /home/...)"""
    if WINE:
        return subprocess.run(["winepath", "-u", path], capture_output=True, text=True, check=True).stdout.strip()
    return path


def start(debugger, rom, runtime, *remote_args):
    """The debugger with --remote: (process, where, token file); WHERE is a socket's path or (host, port)"""
    log = os.path.join(runtime, "debugger.log")
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    command = ["wine"] if WINE else []
    process = subprocess.Popen([*command, debugger, "--batch", "--log", log, "--remote", *remote_args, rom],
                               stdout=subprocess.PIPE, stdin=subprocess.DEVNULL, env=env, text=True)
    line = process.stdout.readline().strip()  # remote: listening on WHERE, token FILE
    check(line.startswith("remote: listening on "), f"the listening line: {line!r}")
    where, token_file = line[len("remote: listening on "):].split(", token ")
    if ":" in where and not where.startswith("/"):
        host, port = where.rsplit(":", 1)
        where = (host, int(port))
    return process, where, host_path(token_file)


def main():
    global WINE
    if sys.argv[1] == "--wine":
        WINE = True
        del sys.argv[1]
    debugger, rom, other_rom = sys.argv[1:4]
    runtime = tempfile.mkdtemp(prefix="one-remote-", dir="/tmp")  # (a short path: a socket's is 107 bytes at most)
    os.chmod(runtime, 0o700)

    # --- the Unix socket (Windows: TCP on a free port) ---
    process, socket_path, token_file = start(debugger, rom, runtime)
    directory = os.path.dirname(token_file) if WINE else os.path.join(runtime, "one-debugger")
    if not WINE:
        check(stat.S_IMODE(os.stat(directory).st_mode) == 0o700, "the directory is mode 0700")
        check(stat.S_IMODE(os.stat(token_file).st_mode) == 0o600, "the token file is mode 0600")
        check(os.path.dirname(socket_path) == directory, "the socket is in the directory")
    if WINE:
        port_file = os.path.splitext(token_file)[0] + ".port"
        check(os.path.exists(port_file) and open(port_file).read().strip() == "%s:%d" % socket_path,
              "the port file says where the debugger listens")
    token = open(token_file).read()
    check(len(token) == 64 and all(c in "0123456789abcdef" for c in token), "the token: 64 hex digits")

    c = Client(socket_path)  # no token: refused, closed
    c.send_line(json.dumps({"id": 1, "cmd": "r"}))
    reply = c.read_line()
    check(reply is not None and reply.get("ok") is False and c.closed(), "a command without the token is refused")
    c.close()
    c = Client(socket_path)  # a wrong token
    c.send_line(json.dumps({"auth": "0" * 64}))
    reply = c.read_line()
    check(reply is not None and reply.get("ok") is False and c.closed(), "a wrong token is refused")
    c.close()

    c = Client(socket_path)
    c.send_line(json.dumps({"auth": token}))
    reply = c.read_line()
    check(reply is not None and reply.get("ok") and reply.get("authorized"), f"the token is accepted: {reply}")
    second = Client(socket_path)  # one client at a time
    reply = second.read_line()
    check(reply is not None and "busy" in reply.get("error", "") and second.closed(), "a second client is refused")
    second.close()

    reply = c.request(cmd="r")
    check(reply.get("ok") and "PC" in reply.get("output", "") and reply["state"]["running"] is False,
          f"r: {reply}")
    reply = c.request(cmd="fs 3")
    check(reply.get("ok") and "Frame" in reply.get("output", "") and reply["state"]["scanline"] == 241 and
          not reply.get("timed_out"), f"fs 3 replies when it stops: {reply}")
    reply = c.request(cmd="eb 0300 = 5a")
    reply = c.request(cmd="!assert mem 0300 5a")
    check(reply.get("ok") and reply.get("output") == "", f"eb, then !assert: {reply}")
    reply = c.request(cmd="!assert mem 0300 00")
    check("ASSERTION FAILED" in reply.get("output", ""), f"a failed !assert is in the output: {reply}")

    # the allowlist
    for command, why in [("lua x.lua", "lua"), ("source x.txt", "source"), ("lb x.bin 300", "lb"),
                         ("screenshot x.png", "screenshot"), ("lsave x.mlb", "lsave with a file")]:
        reply = c.request(cmd=command)
        check(reply.get("ok") is False and "not over the remote channel" in reply.get("error", ""),
              f"refused: {why}: {reply}")
    for command, why in [("bpx c000 if os.execute('true')", "a condition with a function"),
                         ("bpx c000 if a == \"x\"", "a condition with a string"),
                         ("bpx c000 do \"lua x.lua\"", "lua in an action"),
                         ("bpx c000 do \"bpx c004 if io.open('x')\"", "a breakpoint in an action"),
                         (f"load {other_rom}", "load outside the ROM directories")]:
        reply = c.request(cmd=command)
        check("not over the remote channel" in reply.get("output", ""), f"refused: {why}: {reply}")
    reply = c.request(cmd="bl")
    check("C000" not in reply.get("output", "").upper(), f"no breakpoint was set: {reply.get('output')!r}")
    reply = c.request(cmd="bpx c000 if a == 0x10 and mem[0x300] ~= 0")
    reply = c.request(cmd="bl")
    check("C000" in reply.get("output", "").upper(), f"an allowed condition: {reply.get('output')!r}")
    c.request(cmd="bc *")
    reply = c.request(cmd=f"load {rom}")
    check("not over" not in reply.get("output", ""), f"load from the ROM's directory: {reply}")

    # a run that does not stop by itself: the reply after the timeout, then a pause
    c.request(cmd="fs 2")
    reply = c.request(cmd="x", timeout=0.5)
    check(reply.get("ok") and reply.get("timed_out") and reply["state"]["running"], f"x times out: {reply}")
    reply = c.request(cmd="r")
    check(reply.get("ok") is False and "running" in reply.get("error", ""), f"no command while running: {reply}")
    reply = c.request(op="pause")
    check(reply.get("ok") and reply["state"]["running"] is False and "Stopped" in reply.get("output", ""),
          f"pause: {reply}")
    reply = c.request(op="wait", timeout=1)
    check(reply.get("ok") and not reply.get("timed_out"), f"wait while stopped: {reply}")
    reply = c.request(op="state")
    check(reply.get("ok") and reply["state"]["rom"] == os.path.basename(rom), f"state: {reply}")

    reply = c.request(op="screenshot")
    png = base64.b64decode(reply.get("png", ""))
    check(png[:8] == b"\x89PNG\r\n\x1a\n" and struct.unpack(">II", png[16:24]) == (256, 240),
          "a 256x240 PNG screenshot")
    check(not [f for f in os.listdir(directory) if f.endswith(".png")], "no screenshot file is left")
    c.request(cmd="eb 0300 = 5a 01")
    reply = c.request(op="read", space="cpu", address=0x300, length=2)
    check(reply.get("data") == "5A01", f"read: {reply}")
    reply = c.request(op="read", space="oam", address=0x100, length=1)
    check(reply.get("ok") is False, "read outside the memory is refused")
    reply = c.request(op="disassemble", address=0xC000, count=2)
    check("JMP" in reply.get("lines", "") and reply.get("lines", "").count("\n") == 2, f"disassemble: {reply}")
    reply = c.request(op="rominfo")
    rom_data = open(rom, "rb").read()[16:]
    check(reply.get("bank_size") == 0 and reply.get("prg_size") == 0x4000, f"rominfo (NROM): {reply}")
    reply = c.request(op="read", space="prg", address=0, length=3)
    check(reply.get("data") == rom_data[:3].hex().upper(), f"read prg by offset: {reply}")
    reply = c.request(op="read", bank=0, address=0x8000, length=1)
    check(reply.get("ok") is False and "does not switch" in reply.get("error", ""), f"no banks in NROM: {reply}")
    reply = c.request(op="notify", message="hello\nplayer")
    check(reply.get("ok"), f"notify: {reply}")
    reply = c.request(op="dance")
    check(reply.get("ok") is False, "an unknown request is refused")

    c.request(cmd="q")
    process.wait(timeout=20)
    check(process.returncode == 1, f"q ends the debugger (exit code {process.returncode}: 1, an !assert failed)")
    check(WINE or not os.path.exists(socket_path), "the socket file is gone")
    check(not WINE or not os.path.exists(os.path.splitext(token_file)[0] + ".port"), "the port file is gone")
    check(not os.path.exists(token_file), "the token file is gone")
    c.close()

    # --- PRG-ROM banks (MMC1): read and disassemble a bank that is not mapped in ---
    if len(sys.argv) > 4:
        banked = sys.argv[4]
        data = open(banked, "rb").read()[16:]
        process, socket_path, token_file = start(debugger, banked, runtime)
        c = Client(socket_path)
        c.send_line(json.dumps({"auth": open(token_file).read()}))
        c.read_line()
        c.request(cmd="fs 2")
        reply = c.request(op="rominfo")
        check(reply.get("bank_size") == 0x4000 and reply.get("banks") == 4 and reply.get("windows") == "8000=0 C000=3",
              f"rominfo (MMC1): {reply}")
        reply = c.request(op="read", bank=2, address=0x8108, length=3)
        check(reply.get("data") == data[0x8108:0x810B].hex().upper(), f"read bank 2 (not mapped): {reply}")
        reply = c.request(op="disassemble", bank=1, address=0x8108, count=1)
        check(reply.get("lines", "").startswith("01:8108  CE 47 03  DEC $0347"), f"disassemble bank 1: {reply}")
        reply = c.request(op="read", bank=4, address=0x8000, length=1)
        check(reply.get("ok") is False and "0-3" in reply.get("error", ""), f"no bank 4: {reply}")
        reply = c.request(op="read", bank=1, address=0x0300, length=1)
        check(reply.get("ok") is False, f"a bank needs an address $8000-$FFFF: {reply}")
        c.request(cmd="q")
        process.wait(timeout=20)
        c.close()

    # --- TCP: an HTTP request is dropped unanswered; the token works ---
    process, (host, port), token_file = start(debugger, rom, runtime, "0")
    check(host == "127.0.0.1", "TCP on 127.0.0.1 only")
    token = open(token_file).read()
    c = Client((host, port))
    c.send_line("GET / HTTP/1.1\r\nHost: localhost\r")
    check(c.closed(), "an HTTP request gets no answer")
    c.close()
    c = Client((host, port))
    c.send_line(json.dumps({"auth": token}))
    check((c.read_line() or {}).get("authorized"), "the token over TCP")
    c.request(cmd="q")
    process.wait(timeout=20)
    check(not os.path.exists(token_file), "the token file is gone (TCP)")
    c.close()

    shutil.rmtree(runtime, ignore_errors=True)
    if failures:
        print(f"{len(failures)} remote channel checks failed")
        sys.exit(1)
    print("remote channel ok")


if __name__ == "__main__":
    main()
