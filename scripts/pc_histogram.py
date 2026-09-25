#!/usr/bin/env python3
"""Stage 1: boot lk-perf under QEMU, drive the profiler shell command over
the serial console, dump the PC-sample ring buffer over QMP, and
symbolize it into a flat self-time histogram.

This is deliberately NOT a flame graph -- a bare PC sample carries no
calling-context information (see docs/DESIGN.md). It's a function-level
"where does time go" breakdown, and a sanity check that the sampling
pipeline captures real, varying PCs rather than a stuck constant.

Two things reused directly from bolt-aarch32's dump-bolt-counters.py,
proven working there many times:
  - QMP extraction via `memsave`/`pmemsave` (human-monitor-command) --
    writes a plain raw byte dump of one address range, no ELF wrapping.
  - The Qmp/wait_for_marker plumbing, verbatim.

New here, since bolt_bench never needed it (triggered via kernel cmdline,
not interactive shell): the console runs on `-serial stdio` with the
subprocess's own stdin/stdout as pipes, so this script can *type* shell
commands (start/bench/stop) the same way the Stage 0 verification did
manually via a bash `(sleep; printf ...) | qemu-system-arm` pipe -- this
is that same technique, driven from Python instead.

Usage:
    python3 pc_histogram.py --elf build/lk/build-profiler/lk.elf \
        --nm arm-none-eabi-nm --qemu qemu-system-arm
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def nm_symbol_addr(nm: str, elf: str, symbol: str) -> int:
    out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == symbol:
            return int(parts[0], 16)
    sys.exit(f"error: symbol {symbol!r} not found in {elf} (is app/profiler linked in?)")


def find_symbol_table(elf: str, nm: str) -> list[tuple[int, str]]:
    out = subprocess.run(
        [nm, "--defined-only", elf], capture_output=True, text=True, check=True
    ).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        addr_s, kind, name = parts[0], parts[1], parts[2]
        if kind.lower() != "t":  # code symbols only (t/T = text)
            continue
        try:
            syms.append((int(addr_s, 16), name))
        except ValueError:
            continue
    syms.sort()
    return syms


def symbolize(pc: int, syms: list[tuple[int, str]]) -> str:
    # Thumb symbols carry the low bit in the symbol table; a sampled
    # frame->pc does not (it's already a real instruction address), so
    # mask both sides before comparing.
    lo, hi = 0, len(syms) - 1
    best = None
    target = pc & ~1
    while lo <= hi:
        mid = (lo + hi) // 2
        addr = syms[mid][0] & ~1
        if addr <= target:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best[1] if best else f"0x{pc:08x} (no symbol)"


class Qmp:
    def __init__(self, path: str, timeout: float) -> None:
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX)
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit(f"could not connect to QMP socket {path}")
                time.sleep(0.1)
        self.f = self.sock.makefile("rw", encoding="utf-8", newline="\n")
        self._read()  # greeting
        self.execute("qmp_capabilities")

    def _read(self) -> dict:
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("QMP connection closed")
            msg = json.loads(line)
            if "event" not in msg:
                return msg

    def execute(self, command: str, **arguments) -> dict:
        payload = {"execute": command}
        if arguments:
            payload["arguments"] = arguments
        self.f.write(json.dumps(payload) + "\n")
        self.f.flush()
        reply = self._read()
        if "error" in reply:
            raise SystemExit(f"{command} failed: {reply['error']}")
        return reply

    def monitor(self, command_line: str) -> str:
        return self.execute("human-monitor-command", **{"command-line": command_line}).get(
            "return", ""
        )


def qmp_read_mem(qmp: Qmp, addr: int, size: int, tmp_dir: str, tag: str) -> bytes:
    """memsave (virtual, via CPU0's translation) with pmemsave (physical)
    fallback -- same two-step dump-bolt-counters.py already relies on."""
    out = os.path.join(tmp_dir, f"{tag}.bin")
    qmp.monitor(f'memsave 0x{addr:x} {size} "{out}"')
    if not os.path.exists(out) or os.path.getsize(out) != size:
        qmp.monitor(f'pmemsave 0x{addr:x} {size} "{out}"')
    with open(out, "rb") as fh:
        blob = fh.read()
    if len(blob) != size:
        sys.exit(f"error: read {len(blob)} bytes for {tag}, expected {size}")
    return blob


class SerialConsole:
    """Drives QEMU's stdio-backed serial console: write shell commands in,
    tail the combined output for markers. Same technique as the manual
    `(sleep N; printf "cmd\\r"; sleep M) | qemu-system-arm ...` pipe used
    to verify Stage 0, just interactive instead of a fixed script."""

    def __init__(self, proc: subprocess.Popen):
        self.proc = proc
        self.lines: list[str] = []
        self._lock = threading.Lock()
        self._t = threading.Thread(target=self._pump, daemon=True)
        self._t.start()

    def _pump(self) -> None:
        for raw in iter(self.proc.stdout.readline, ""):
            with self._lock:
                self.lines.append(raw)

    def snapshot(self) -> str:
        with self._lock:
            return "".join(self.lines)

    def wait_for(self, marker: str, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if marker in self.snapshot():
                return True
            time.sleep(0.2)
        return False

    def send(self, line: str) -> None:
        self.proc.stdin.write(line + "\r")
        self.proc.stdin.flush()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--qemu", default="qemu-system-arm")
    ap.add_argument("--cpu", default="cortex-a15")
    ap.add_argument("--machine", default="virt")
    ap.add_argument("--mem", default="512")
    ap.add_argument("--smp", default="1")
    ap.add_argument("--boot-marker", default="entering main console loop")
    ap.add_argument("--boot-timeout", type=float, default=60.0)
    ap.add_argument(
        "--bench-iters",
        type=int,
        default=20000000,
        help="iters/function passed to the on-target 'profiler bench'/'nest' command",
    )
    ap.add_argument("--bench-timeout", type=float, default=60.0)
    ap.add_argument(
        "--nest",
        action="store_true",
        help="drive 'profiler nest' (3-level call chain) instead of 'profiler bench'",
    )
    args = ap.parse_args()

    buf_size = 4096  # PROFILER_BUF_SIZE -- keep in sync with profiler.c
    head_addr = nm_symbol_addr(args.nm, args.elf, "profiler_head")
    total_addr = nm_symbol_addr(args.nm, args.elf, "profiler_total")
    buf_addr = nm_symbol_addr(args.nm, args.elf, "profiler_pc_buf")
    lr_buf_addr = nm_symbol_addr(args.nm, args.elf, "profiler_lr_buf")
    print(
        f"profiler_pc_buf @ 0x{buf_addr:x}  profiler_lr_buf @ 0x{lr_buf_addr:x}  "
        f"profiler_head @ 0x{head_addr:x}  profiler_total @ 0x{total_addr:x}",
        file=sys.stderr,
    )

    tmp = tempfile.mkdtemp(prefix="lk-perf-qemu-")
    qmp_path = os.path.join(tmp, "qmp.sock")

    cmd = [
        args.qemu,
        "-machine", args.machine,
        "-cpu", args.cpu,
        "-m", args.mem,
        "-smp", args.smp,
        "-display", "none",
        "-monitor", "none",
        "-serial", "stdio",
        "-qmp", f"unix:{qmp_path},server=on,wait=off",
        "-kernel", args.elf,
    ]
    print("launching:", " ".join(cmd), file=sys.stderr)
    qemu = subprocess.Popen(
        cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )
    console = SerialConsole(qemu)

    try:
        qmp = Qmp(qmp_path, timeout=30.0)

        if not console.wait_for(args.boot_marker, args.boot_timeout):
            print(console.snapshot(), file=sys.stderr)
            sys.exit(f"error: never saw {args.boot_marker!r}")
        print(f"booted: saw {args.boot_marker!r}", file=sys.stderr)
        time.sleep(0.5)  # let the shell prompt settle before typing

        console.send("profiler clear")
        time.sleep(0.3)
        console.send("profiler start")
        time.sleep(0.3)
        before = len(console.snapshot())
        sub = "nest" if args.nest else "bench"
        console.send(f"profiler {sub} {args.bench_iters}")
        if not console.wait_for(f"{sub} done", args.bench_timeout):
            print(console.snapshot()[before:], file=sys.stderr)
            sys.exit(f"error: 'profiler {sub}' never reported done")
        console.send("profiler stop")
        time.sleep(0.3)
        console.send("profiler status")
        time.sleep(0.3)
        print(console.snapshot()[before:], file=sys.stderr)

        total_bytes = qmp_read_mem(qmp, total_addr, 4, tmp, "total")
        head_bytes = qmp_read_mem(qmp, head_addr, 4, tmp, "head")
        total = struct.unpack("<I", total_bytes)[0]
        head = struct.unpack("<I", head_bytes)[0]
        print(f"total samples: {total}  head: {head}", file=sys.stderr)
        if total == 0:
            sys.exit("error: zero samples captured -- sampling pipeline did not fire")

        n = min(total, buf_size)
        buf_bytes = qmp_read_mem(qmp, buf_addr, buf_size * 4, tmp, "buf")
        lr_bytes = qmp_read_mem(qmp, lr_buf_addr, buf_size * 4, tmp, "lr_buf")
        pcs = list(struct.unpack(f"<{buf_size}I", buf_bytes))[:n]
        lrs = list(struct.unpack(f"<{buf_size}I", lr_bytes))[:n]
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()

    syms = find_symbol_table(args.elf, args.nm)
    hist: dict[str, int] = {}
    callers: dict[str, dict[str, int]] = {}
    for pc, lr in zip(pcs, lrs):
        name = symbolize(pc, syms)
        hist[name] = hist.get(name, 0) + 1
        # LR is a caller's return address (post-call instruction), not the
        # call site itself, and isn't ground truth once `name` has made its
        # own calls (AAPCS lets it reuse LR as scratch) -- an approximate,
        # cheap upgrade over Stage 1, not a real stack (see docs/DESIGN.md).
        caller = symbolize(lr, syms)
        callers.setdefault(name, {})
        callers[name][caller] = callers[name].get(caller, 0) + 1

    print(f"\n{'samples':>8}  {'%':>6}  function")
    print("-" * 50)
    for name, count in sorted(hist.items(), key=lambda kv: -kv[1]):
        pct = 100.0 * count / n
        print(f"{count:>8}  {pct:>5.1f}%  {name}")
        for caller, ccount in sorted(callers[name].items(), key=lambda kv: -kv[1]):
            print(f"{'':>8}  {'':>6}    <- {caller}  ({ccount}/{count})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
