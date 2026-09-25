#!/usr/bin/env python3
"""Boot lk-perf under QEMU, drive the profiler shell command over the
serial console, dump the sample ring buffers over QMP, and produce both
a flat self-time histogram (Stage 1) and, per sample, an offline
frame-pointer-chain call stack (Stage 3) -- collapsed-stack output
compatible with standard FlameGraph tooling.

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

The FP-chain walk reads two words at each frame's fp (Thumb: r7,
ARM: r11 -- picked per-sample from the interrupted SPSR's T-bit, see
profiler.c): [fp+0]=caller's saved fp, [fp+4]=the *stack-saved* return
address (reliable, unlike Stage 2's live-LR read -- see profiler.c's own
comment on why GCC can repurpose live lr as scratch mid-function but
can't touch the copy it already pushed to the stack). Walk stops on a
non-increasing fp (stacks grow down, so a legitimate caller frame is
always at a higher address than its callee) -- a cheap, dependency-free
corruption/cycle guard that doesn't need to know the stack's actual
address range.

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


def nm_symbol_size(nm: str, elf: str, symbol: str) -> int:
    """Byte size of a symbol via `nm -S` -- used to derive SMP_MAX_CPUS
    from profiler_total[SMP_MAX_CPUS]'s own array size rather than
    hardcoding a core count that has to be kept in sync by hand."""
    out = subprocess.run(
        [nm, "-S", elf], capture_output=True, text=True, check=True
    ).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[3] == symbol:
            return int(parts[1], 16)
    sys.exit(f"error: symbol {symbol!r} not found (or has no size) in {elf}")


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


def walk_fp_chain(
    qmp: Qmp,
    fp: int,
    tmp_dir: str,
    text_lo: int,
    text_hi: int,
    max_depth: int = 12,
) -> list[int]:
    """Return caller return addresses, immediate caller first, by walking
    the FP chain (see module docstring for the frame layout and the
    non-increasing-fp stop condition).

    Not every -fno-omit-frame-pointer frame has a [fp+4] return-address
    slot: GCC only spills {fp, lr} as a pair when it actually needs to
    (a real call to preserve lr across, or -- empirically confirmed via
    objdump, not assumed -- register pressure in an otherwise-leaf loop).
    A pure leaf with no such pressure pushes {fp} alone and returns via
    the still-live lr register instead, so [fp+4] there is unrelated
    stack content, not a return address. Without unwind-table metadata
    (deliberately not available here -- see docs/DESIGN.md) there's no
    way to know which kind of frame this is from fp alone, so this
    validates [fp+4] against the ELF's actual .text range before
    trusting it and stops the walk rather than fabricate a frame."""
    frames: list[int] = []
    seen: set[int] = set()
    depth = 0
    while fp and (fp & 3) == 0 and depth < max_depth and fp not in seen:
        seen.add(fp)
        blob = qmp_read_mem(qmp, fp, 8, tmp_dir, f"fpwalk_{depth}_{fp:x}")
        saved_fp, ret_addr = struct.unpack("<II", blob)
        if not (text_lo <= (ret_addr & ~1) <= text_hi):
            break
        frames.append(ret_addr)
        if saved_fp <= fp:
            break
        fp = saved_fp
        depth += 1
    return frames


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
    ap.add_argument(
        "--smp-workload",
        action="store_true",
        help="drive 'profiler smp' (concurrent worker per core) -- implies --smp > 1",
    )
    args = ap.parse_args()

    buf_size = 4096  # PROFILER_BUF_SIZE -- keep in sync with profiler.c
    num_cpus = nm_symbol_size(args.nm, args.elf, "profiler_total") // 4
    if args.smp_workload and args.smp == "1":
        args.smp = str(num_cpus)  # boot enough cores for the ELF's own SMP_MAX_CPUS
    head_addr = nm_symbol_addr(args.nm, args.elf, "profiler_head")
    total_addr = nm_symbol_addr(args.nm, args.elf, "profiler_total")
    buf_addr = nm_symbol_addr(args.nm, args.elf, "profiler_pc_buf")
    lr_buf_addr = nm_symbol_addr(args.nm, args.elf, "profiler_lr_buf")
    fp_buf_addr = nm_symbol_addr(args.nm, args.elf, "profiler_fp_buf")
    ts_buf_addr = nm_symbol_addr(args.nm, args.elf, "profiler_ts_buf")
    print(
        f"SMP_MAX_CPUS={num_cpus}  profiler_pc_buf @ 0x{buf_addr:x}  "
        f"profiler_lr_buf @ 0x{lr_buf_addr:x}  profiler_fp_buf @ 0x{fp_buf_addr:x}  "
        f"profiler_ts_buf @ 0x{ts_buf_addr:x}  profiler_head @ 0x{head_addr:x}  "
        f"profiler_total @ 0x{total_addr:x}",
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
        sub = "smp" if args.smp_workload else ("nest" if args.nest else "bench")
        console.send(f"profiler {sub} {args.bench_iters}")

        # Deliberately do NOT wait for '{sub} done' before reading memory.
        # By the time that text is even visible, the sampled function has
        # already returned -- profiler_workload_inner's own epilogue pops
        # its {fp, lr} pair the instant profiler_workload_outer() returns,
        # and the *following* printf call (same -fno-omit-frame-pointer
        # build) immediately reuses that exact stack slot for its own
        # frame, synchronously inside the guest, before any host-side
        # reaction -- however fast -- is possible. Empirically confirmed:
        # even qmp.execute("stop") issued the instant 'done' was detected
        # still read zeros there. Instead poll the ever-increasing
        # profiler_total counter (a stable BSS word, not reused stack
        # memory, safe to read while running) until enough samples exist,
        # then pause via QMP mid-workload, while the sampled call chain's
        # stack frames are still genuinely live. This is what makes the
        # captured fp values point at memory the walk can still trust.
        # Poll the SUM across all cores' profiler_total[] (still one small
        # QMP read, still stable BSS, not reused stack memory). Fixed,
        # small threshold, deliberately NOT scaled by num_cpus: this is a
        # SUM, so a multi-core workload reaches it faster, not slower --
        # scaling it up regression-tested badly (single-core --nest,
        # where only cpu0 ever contributes, took 4x longer than the
        # workload's own ~280ms runtime to reach a *4 threshold, so the
        # pause fired *after* completion, reproducing the exact Stage 3
        # stack-reuse bug this whole mechanism exists to avoid -- fixed
        # by keeping the threshold small and workload-shape-independent).
        min_samples = 8
        deadline = time.time() + args.bench_timeout
        totals = [0] * num_cpus
        while time.time() < deadline:
            totals_bytes = qmp_read_mem(qmp, total_addr, num_cpus * 4, tmp, "poll_totals")
            totals = list(struct.unpack(f"<{num_cpus}I", totals_bytes))
            if sum(totals) >= min_samples:
                break
            time.sleep(0.05)
        qmp.execute("stop")
        print(f"paused mid-workload after {totals} samples/core", file=sys.stderr)
        if sum(totals) == 0:
            print(console.snapshot()[before:], file=sys.stderr)
            sys.exit("error: zero samples captured before pause -- sampling pipeline did not fire")

        heads_bytes = qmp_read_mem(qmp, head_addr, num_cpus * 4, tmp, "heads")
        totals_bytes = qmp_read_mem(qmp, total_addr, num_cpus * 4, tmp, "totals")
        totals = list(struct.unpack(f"<{num_cpus}I", totals_bytes))
        heads = list(struct.unpack(f"<{num_cpus}I", heads_bytes))
        print(f"total samples: {sum(totals)}  per-core: {totals}", file=sys.stderr)

        # profiler_pc_buf etc. are C row-major [cpu][index] arrays -- one
        # read of the whole 2D array, sliced per-core in Python, instead
        # of num_cpus separate small QMP round-trips.
        buf_bytes = qmp_read_mem(qmp, buf_addr, num_cpus * buf_size * 4, tmp, "buf")
        lr_bytes = qmp_read_mem(qmp, lr_buf_addr, num_cpus * buf_size * 4, tmp, "lr_buf")
        fp_bytes = qmp_read_mem(qmp, fp_buf_addr, num_cpus * buf_size * 4, tmp, "fp_buf")
        ts_bytes = qmp_read_mem(qmp, ts_buf_addr, num_cpus * buf_size * 8, tmp, "ts_buf")
        all_pcs = struct.unpack(f"<{num_cpus * buf_size}I", buf_bytes)
        all_lrs = struct.unpack(f"<{num_cpus * buf_size}I", lr_bytes)
        all_fps = struct.unpack(f"<{num_cpus * buf_size}I", fp_bytes)
        all_ts = struct.unpack(f"<{num_cpus * buf_size}Q", ts_bytes)

        syms = find_symbol_table(args.elf, args.nm)
        text_lo, text_hi = syms[0][0] & ~1, syms[-1][0] & ~1

        # Per-core sample lists: (pc, lr, fp, ts, cpu) tuples, n = min(total, capacity)
        # per core since each core's ring buffer wraps independently.
        samples: list[tuple[int, int, int, int, int]] = []
        for cpu in range(num_cpus):
            n_cpu = min(totals[cpu], buf_size)
            base = cpu * buf_size
            for i in range(n_cpu):
                samples.append(
                    (all_pcs[base + i], all_lrs[base + i], all_fps[base + i],
                     all_ts[base + i], cpu)
                )
        n = len(samples)

        print(f"walking FP chains for {n} samples across {num_cpus} cores ...", file=sys.stderr)
        stacks = [walk_fp_chain(qmp, fp, tmp, text_lo, text_hi) for _, _, fp, _, _ in samples]
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()

    hist: dict[str, int] = {}
    callers: dict[str, dict[str, int]] = {}
    folded_merged: dict[str, int] = {}
    folded_per_cpu: list[dict[str, int]] = [dict() for _ in range(num_cpus)]
    depths: list[int] = []
    for (pc, lr, _fp, ts, cpu), frames in zip(samples, stacks):
        name = symbolize(pc, syms)
        hist[name] = hist.get(name, 0) + 1
        # LR is a caller's return address (post-call instruction), not the
        # call site itself, and isn't ground truth once `name` has made its
        # own calls (AAPCS lets it reuse LR as scratch) -- an approximate,
        # cheap upgrade over Stage 1, not a real stack (see docs/DESIGN.md).
        caller = symbolize(lr, syms)
        callers.setdefault(name, {})
        callers[name][caller] = callers[name].get(caller, 0) + 1

        # Real call stack (Stage 3): outermost caller first, sample's own
        # PC last -- standard folded-stack order for FlameGraph tooling.
        # Merged view is what BOLT's own function/block reordering
        # optimizes for (a global layout decision independent of which
        # core executes it); per-core is the diagnostic "which core is
        # the bottleneck" view. CNTPCT-derived ts (not used for ordering
        # here -- folded format is unordered/aggregated by design -- but
        # carried through per-sample since a future timeline view needs
        # cross-core-comparable timestamps, which is exactly what CNTPCT,
        # not PMCCNTR, guarantees; see docs/DESIGN.md and profiler.c).
        del ts
        stack_syms = [symbolize(addr, syms) for addr in reversed(frames)] + [name]
        key = ";".join(stack_syms)
        folded_merged[key] = folded_merged.get(key, 0) + 1
        folded_per_cpu[cpu][key] = folded_per_cpu[cpu].get(key, 0) + 1
        depths.append(len(stack_syms))

    print(f"\n{'samples':>8}  {'%':>6}  function")
    print("-" * 50)
    for name, count in sorted(hist.items(), key=lambda kv: -kv[1]):
        pct = 100.0 * count / n
        print(f"{count:>8}  {pct:>5.1f}%  {name}")
        for caller, ccount in sorted(callers[name].items(), key=lambda kv: -kv[1]):
            print(f"{'':>8}  {'':>6}    <- {caller}  ({ccount}/{count})")

    avg_depth = sum(depths) / len(depths) if depths else 0.0
    print(
        f"\nFP-chain unwind: {n} samples across {num_cpus} cores, "
        f"avg stack depth {avg_depth:.1f} frames (1 = leaf-only, chain didn't extend)",
        file=sys.stderr,
    )
    print(f"\n{'samples':>8}  call stack, all cores merged (outermost -> innermost, folded)")
    print("-" * 70)
    for key, count in sorted(folded_merged.items(), key=lambda kv: -kv[1]):
        print(f"{count:>8}  {key}")

    base_path = os.path.splitext(args.elf)[0]
    merged_path = base_path + ".folded"
    with open(merged_path, "w") as fh:
        for key, count in sorted(folded_merged.items(), key=lambda kv: -kv[1]):
            fh.write(f"{key} {count}\n")
    print(f"\nmerged folded-stack output (FlameGraph-compatible): {merged_path}", file=sys.stderr)

    for cpu in range(num_cpus):
        if not folded_per_cpu[cpu]:
            continue
        cpu_path = f"{base_path}.cpu{cpu}.folded"
        with open(cpu_path, "w") as fh:
            for key, count in sorted(folded_per_cpu[cpu].items(), key=lambda kv: -kv[1]):
                fh.write(f"{key} {count}\n")
        print(f"cpu{cpu} folded-stack output: {cpu_path}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
