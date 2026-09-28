#!/usr/bin/env python3
"""Parse `profiler dump` output (captured via pi4_serial_boot.py --log
or scripts/pi4_run.py --log) into a symbolized, multi-frame sample
histogram, using DwarfCFIUnwinder against the per-sample stack-memory
capture app/profiler.c's `dump` command now includes.

Usage:
    python scripts/pi4_pc_histogram.py pi4.log build/lk/build-rpi4-test/lk.elf \
        [--folded out.folded] [--annotate N]

`pi4.log` just needs to contain the "SAMPLE ..." lines `profiler dump`
prints -- everything else in the log (shell echoes, other output) is
ignored. Each sample's `stack=<hex>` field is PROFILER_STACK_CAPTURE_BYTES
raw bytes starting at `sp`; reads outside that window return 0, which
DwarfCFIUnwinder.unwind() already treats as a clean stop (same as a
real, legitimately-zero saved LR) -- so an unwind that runs past the
captured window just truncates there rather than erroring.

`--annotate N` (M5 `profiler annotate`): disassembles the N hottest
functions via arm-none-eabi-objdump (needed on PATH -- same dependency
this project's setup.sh already installs) over each function's real
address range, and prefixes every instruction with how many samples
landed exactly there -- a `perf annotate`-style, per-instruction
hotspot view, not just "which function". objdump itself already
handles ARM/Thumb-correct disassembly from the ELF's own `$t`/`$a`
mapping symbols, so this doesn't need to track instruction sets itself.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).parent))
from dwarf_unwind import (
    DwarfCFIUnwinder, SP_REG, LR_REG, find_function, resolve_lines, symbolize,
    strip_isa_bit,
)

SAMPLE_RE = re.compile(
    r"SAMPLE cpu=(?P<cpu>\d+) pc=(?P<pc>[0-9a-fA-F]+) lr=(?P<lr>[0-9a-fA-F]+) "
    r"fp=(?P<fp>[0-9a-fA-F]+) sp=(?P<sp>[0-9a-fA-F]+) spsr=(?P<spsr>[0-9a-fA-F]+) "
    r"tid=(?P<tid>[0-9a-fA-F]+) ts=(?P<ts>[0-9a-fA-F]+)(?: stack=(?P<stack>[0-9a-fA-F]+))?"
)

SPSR_T_BIT = 1 << 5  # Thumb state -- same bit app/profiler.c reads


def parse_samples(log_path: str) -> list[dict]:
    samples = []
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            m = SAMPLE_RE.search(line)
            if not m:
                continue
            samples.append({
                "cpu": int(m["cpu"]),
                "pc": int(m["pc"], 16),
                "lr": int(m["lr"], 16),
                "fp": int(m["fp"], 16),
                "sp": int(m["sp"], 16),
                "spsr": int(m["spsr"], 16),
                "tid": int(m["tid"], 16),
                "ts": int(m["ts"], 16),
                "stack": bytes.fromhex(m["stack"]) if m["stack"] else b"",
            })
    return samples


def make_read_memory(stack_bytes: bytes, sp: int) -> Callable[[int, int], int]:
    def read_memory(addr: int, size: int) -> int:
        offset = addr - sp
        if offset < 0 or offset + size > len(stack_bytes):
            return 0  # out of the captured window -- unwind() already
                       # treats a resolved 0 as a clean stop, same as a
                       # real, legitimately-zero saved LR
        return int.from_bytes(stack_bytes[offset:offset + size], "little")
    return read_memory


ADDR_RE = re.compile(r"^\s*([0-9a-f]+):\s")


def annotate_function(elf_path: str, name: str, start: int, size: int,
                       pc_counts: Counter, total_for_func: int) -> list[str]:
    """Real disassembly of one function's exact address range (via
    objdump -- see this module's own docstring for why), one line per
    instruction, prefixed with that instruction's own sample count and
    percentage of the function's total. A source-line marker is
    inserted whenever resolve_lines() says the line changed, the same
    interleaving `perf annotate` itself does.
    """
    end = start + max(size, 1)
    try:
        result = subprocess.run(
            ["arm-none-eabi-objdump", "-d",
             f"--start-address=0x{start:x}", f"--stop-address=0x{end:x}", elf_path],
            capture_output=True, text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError) as e:
        return [f"  (couldn't disassemble {name}: {e})"]

    # Two passes: first collect every instruction's address, then
    # resolve all of them in one resolve_lines() call -- it reloads
    # and re-decodes the whole .debug_line program from scratch each
    # time it's called, so calling it per-instruction here would be
    # quadratic in this function's instruction count.
    disasm_lines = result.stdout.splitlines()
    addrs = [int(m.group(1), 16) for m in map(ADDR_RE.match, disasm_lines) if m]
    lines_by_addr = dict(zip(addrs, resolve_lines(elf_path, addrs)))

    out = [f"\n{name} (0x{start:x}-0x{end:x}, {total_for_func} samples):"]
    last_line = None
    for line in disasm_lines:
        m = ADDR_RE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        this_line = lines_by_addr.get(addr)
        if this_line != last_line:
            out.append(f"        ; {this_line or '?'}")
            last_line = this_line
        count = pc_counts.get(addr, 0)
        pct = f"{100 * count / total_for_func:5.1f}%" if count else "      "
        prefix = f"{count:6d} {pct} |" if count else "            |"
        out.append(f"{prefix} {line.strip()}")
    return out


def unwind_sample(unwinder: DwarfCFIUnwinder, s: dict) -> list[int]:
    thumb = (s["spsr"] & SPSR_T_BIT) != 0
    fp_reg = 7 if thumb else 11
    registers = {SP_REG: s["sp"], LR_REG: s["lr"], fp_reg: s["fp"]}
    read_memory = make_read_memory(s["stack"], s["sp"])
    return unwinder.unwind(s["pc"], registers, read_memory)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", help="captured serial log containing 'profiler dump' output")
    ap.add_argument("elf", help="the built lk.elf to unwind/symbolize against")
    ap.add_argument("--folded", help="write a FlameGraph-compatible folded-stack file here")
    ap.add_argument("--annotate", type=int, metavar="N",
                    help="disassemble the N hottest functions with per-instruction "
                         "sample counts (needs arm-none-eabi-objdump on PATH)")
    args = ap.parse_args()

    samples = parse_samples(args.log)
    if not samples:
        sys.exit(f"error: no SAMPLE lines found in {args.log}")

    # Review finding #12, fixed: a single sample whose CFA rule needs a
    # register this sample didn't capture (only whichever of r7/r11
    # matched the interrupted PC's own Thumb/ARM mode is ever recorded,
    # see profiler.c:340) used to raise straight out of this loop and
    # abort the ENTIRE report, discarding every other sample with it.
    # Falls back to a leaf-only, single-frame chain instead -- the same
    # view Stage 1's PC-only sampling always gave -- so one bad sample
    # only costs its own multi-frame detail, not the whole run.
    chains = []
    unwind_errors = 0
    with DwarfCFIUnwinder(args.elf) as unwinder:
        for s in samples:
            try:
                chains.append(unwind_sample(unwinder, s))
            except (ValueError, NotImplementedError):
                unwind_errors += 1
                chains.append([s["pc"]])
    if unwind_errors:
        print(f"warning: {unwind_errors}/{len(samples)} sample(s) failed to unwind "
              f"past the leaf frame (kept as leaf-only, not dropped)\n", file=sys.stderr)

    leaf_pcs = [strip_isa_bit(c[0]) for c in chains]
    total = len(samples)
    per_cpu = Counter(s["cpu"] for s in samples)
    threads = Counter(s["tid"] for s in samples)
    depths = Counter(len(c) for c in chains)
    leaf_names = symbolize(args.elf, leaf_pcs)
    leaf_counts = Counter(leaf_names)
    # One representative source line per unique leaf PC -- resolved
    # once, batched, then looked up per name below (resolve_lines()
    # reloads the whole .debug_line program each call it's given, so
    # this stays a single call regardless of sample count).
    unique_pcs = sorted(set(leaf_pcs))
    line_by_pc = dict(zip(unique_pcs, resolve_lines(args.elf, unique_pcs)))
    line_by_name = {}
    for pc, name in zip(leaf_pcs, leaf_names):
        line_by_name.setdefault(name, line_by_pc.get(pc))

    print(f"{total} samples across {len(per_cpu)} cpu(s): {dict(sorted(per_cpu.items()))}")
    print(f"{len(threads)} distinct thread(s) sampled (by thread_t* -- see profiler.c)")
    print(f"unwind depth histogram (frames per sample): {dict(sorted(depths.items()))}")
    print()
    print(f"{'count':>8}  {'%':>6}  leaf function (source line)")
    for name, count in leaf_counts.most_common(30):
        line = line_by_name.get(name)
        suffix = f"  ({line})" if line else ""
        print(f"{count:>8}  {100 * count / total:5.1f}%  {name}{suffix}")

    if args.annotate:
        func_pcs: dict[tuple, Counter] = {}
        for pc in leaf_pcs:
            fn = find_function(args.elf, pc)
            if fn is None:
                continue
            func_pcs.setdefault(fn, Counter())[pc] += 1
        ranked = sorted(func_pcs.items(), key=lambda kv: -sum(kv[1].values()))
        for (name, start, size, _is_thumb), pc_counts in ranked[:args.annotate]:
            total_for_func = sum(pc_counts.values())
            for line in annotate_function(args.elf, name, start, size,
                                           pc_counts, total_for_func):
                print(line)

    if args.folded:
        folded_counts: Counter[str] = Counter()
        for chain in chains:
            names = symbolize(args.elf, chain)
            # root-to-leaf for FlameGraph, chain itself is leaf-to-root
            folded_counts[";".join(reversed(names))] += 1
        # flamegraph.pl rejects every line ending in "\r\n"; text mode on Windows writes that
        with open(args.folded, "w", newline="\n") as f:
            for stack, count in folded_counts.items():
                f.write(f"{stack} {count}\n")
        print(f"\nwrote {args.folded} -- render with: "
              f"perl scripts/flamegraph.pl {args.folded} > flame.svg")


if __name__ == "__main__":
    main()
