#!/usr/bin/env python3
"""Parse `profiler dump` output (captured via pi4_serial_boot.py --log
or scripts/pi4_run.py --log) into a symbolized, multi-frame sample
histogram, using DwarfCFIUnwinder against the per-sample stack-memory
capture app/profiler.c's `dump` command now includes.

Usage:
    python scripts/pi4_pc_histogram.py pi4.log build/lk/build-rpi4-test/lk.elf \
        [--folded out.folded]

`pi4.log` just needs to contain the "SAMPLE ..." lines `profiler dump`
prints -- everything else in the log (shell echoes, other output) is
ignored. Each sample's `stack=<hex>` field is PROFILER_STACK_CAPTURE_BYTES
raw bytes starting at `sp`; reads outside that window return 0, which
DwarfCFIUnwinder.unwind() already treats as a clean stop (same as a
real, legitimately-zero saved LR) -- so an unwind that runs past the
captured window just truncates there rather than erroring.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).parent))
from dwarf_unwind import DwarfCFIUnwinder, SP_REG, LR_REG, symbolize

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
    args = ap.parse_args()

    samples = parse_samples(args.log)
    if not samples:
        sys.exit(f"error: no SAMPLE lines found in {args.log}")

    with DwarfCFIUnwinder(args.elf) as unwinder:
        chains = [unwind_sample(unwinder, s) for s in samples]

    total = len(samples)
    per_cpu = Counter(s["cpu"] for s in samples)
    threads = Counter(s["tid"] for s in samples)
    depths = Counter(len(c) for c in chains)
    leaf_names = symbolize(args.elf, [c[0] for c in chains])
    leaf_counts = Counter(leaf_names)

    print(f"{total} samples across {len(per_cpu)} cpu(s): {dict(sorted(per_cpu.items()))}")
    print(f"{len(threads)} distinct thread(s) sampled (by thread_t* -- see profiler.c)")
    print(f"unwind depth histogram (frames per sample): {dict(sorted(depths.items()))}")
    print()
    print(f"{'count':>8}  {'%':>6}  leaf function")
    for name, count in leaf_counts.most_common(30):
        print(f"{count:>8}  {100 * count / total:5.1f}%  {name}")

    if args.folded:
        folded_counts: Counter[str] = Counter()
        for chain in chains:
            names = symbolize(args.elf, chain)
            # root-to-leaf for FlameGraph, chain itself is leaf-to-root
            folded_counts[";".join(reversed(names))] += 1
        with open(args.folded, "w") as f:
            for stack, count in folded_counts.items():
                f.write(f"{stack} {count}\n")
        print(f"\nwrote {args.folded} -- render with: "
              f"perl scripts/flamegraph.pl {args.folded} > flame.svg")


if __name__ == "__main__":
    main()
