#!/usr/bin/env python3
"""Parse `profiler dump` output (captured via pi4_serial_boot.py --log)
into a per-function sample histogram, symbolized against the built ELF.

Registers only, not stack memory (see app/profiler/profiler.c's header
comment for why) -- this is a single-frame (leaf-function) view, the
M5 equivalent of Stage 1 of the original QEMU-era design. Multi-frame
DWARF-CFI unwinding needs a stack-memory capture this doesn't have
yet; scripts/dwarf_unwind.py is ready for that once it exists.

Usage:
    python scripts/pi4_pc_histogram.py pi4.log build/lk/build-rpi4-test/lk.elf \
        [--folded out.folded]

`pi4.log` just needs to contain the "SAMPLE ..." lines app/profiler.c's
`profiler dump` command prints -- everything else in the log (shell
echoes, other output) is ignored.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from dwarf_unwind import symbolize

SAMPLE_RE = re.compile(
    r"SAMPLE cpu=(?P<cpu>\d+) pc=(?P<pc>[0-9a-fA-F]+) lr=(?P<lr>[0-9a-fA-F]+) "
    r"fp=(?P<fp>[0-9a-fA-F]+) sp=(?P<sp>[0-9a-fA-F]+) spsr=(?P<spsr>[0-9a-fA-F]+) "
    r"tid=(?P<tid>[0-9a-fA-F]+) ts=(?P<ts>[0-9a-fA-F]+)"
)


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
            })
    return samples


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", help="captured serial log containing 'profiler dump' output")
    ap.add_argument("elf", help="the built lk.elf to symbolize against")
    ap.add_argument("--folded", help="write a FlameGraph-compatible folded-stack file here")
    args = ap.parse_args()

    samples = parse_samples(args.log)
    if not samples:
        sys.exit(f"error: no SAMPLE lines found in {args.log}")

    names = symbolize(args.elf, [s["pc"] for s in samples])

    total = len(samples)
    per_cpu = Counter(s["cpu"] for s in samples)
    threads = Counter(s["tid"] for s in samples)
    counts = Counter(names)

    print(f"{total} samples across {len(per_cpu)} cpu(s): {dict(sorted(per_cpu.items()))}")
    print(f"{len(threads)} distinct thread(s) sampled (by thread_t* -- see profiler.c)")
    print()
    print(f"{'count':>8}  {'%':>6}  function")
    for name, count in counts.most_common(30):
        print(f"{count:>8}  {100 * count / total:5.1f}%  {name}")

    if args.folded:
        with open(args.folded, "w") as f:
            for name, count in counts.items():
                f.write(f"{name} {count}\n")
        print(f"\nwrote {args.folded} -- render with: "
              f"perl scripts/flamegraph.pl {args.folded} > flame.svg")


if __name__ == "__main__":
    main()
