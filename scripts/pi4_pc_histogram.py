#!/usr/bin/env python3
"""Parse `profiler dump` output (captured via pi4_serial_boot.py --log
or scripts/pi4_run.py --log) into a symbolized, multi-frame sample
histogram, using DwarfCFIUnwinder against the per-sample stack-memory
capture app/profiler.c's `dump` command now includes.

Usage:
    python scripts/pi4_pc_histogram.py pi4.log build/lk/build-rpi4-test/lk.elf \
        [--folded out.folded] [--annotate N] [--no-mask-frames]

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

Capture sessions (K2): dumps from current images start with a checksummed
DUMPBEGIN header (session run id, dump number, image range and hash,
sampling modes, PMU event/period) and per-core DUMPCPU lines (total,
retained, overwritten, PMU overflows lost), and end with a DUMPEND footer
giving the exact number of SAMPLE records. A log may hold several dumps; the
latest is used unless `--dump N` picks another (0 = first, -1 = latest). The
image hash is recomputed from the ELF and a mismatch is refused
(`--allow-elf-mismatch` overrides). Logs without DUMPBEGIN are read as before.

IRQ-masked time (K6): dumps from images with masked-time accounting add
`src/lat/msite/mgap` to each sample and MASKINFO/MASKCPU/MASKSITE lines.
The report then states how much of each core's time ran with IRQs masked
(invisible to sampling), which code did the masking, and which samples
were delayed by a masked region. In `--folded` output a delayed sample
gets an extra leaf frame `[irq-masked: <site>]` naming the region that
held it back (`--no-mask-frames` turns this off). Older dumps without
these fields still parse and report as before.
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
    r"SAMPLE seq=(?P<seq>[0-9a-fA-F]+) cpu=(?P<cpu>\d+) pc=(?P<pc>[0-9a-fA-F]+) "
    r"lr=(?P<lr>[0-9a-fA-F]+) fp=(?P<fp>[0-9a-fA-F]+) sp=(?P<sp>[0-9a-fA-F]+) "
    r"spsr=(?P<spsr>[0-9a-fA-F]+) tid=(?P<tid>[0-9a-fA-F]+) ts=(?P<ts>[0-9a-fA-F]+) "
    r"stack=(?P<stack>[0-9a-fA-F]+)"
    # K6 fields, absent in older dumps (their checksum then omits them too)
    r"(?: src=(?P<src>[tp?]) lat=(?P<lat>[0-9a-fA-F]+) msite=(?P<msite>[0-9a-fA-F]+) "
    r"mgap=(?P<mgap>[0-9a-fA-F]+))?"
    # K1: bytes of `stack` actually copied (the rest is zero fill)
    r"(?: slen=(?P<slen>[0-9a-fA-F]+))?"
    r" crc=(?P<crc>[0-9a-fA-F]+)"
)

MASKINFO_RE = re.compile(
    r"MASKINFO cntfrq=(?P<freq>[0-9a-fA-F]{8}) on=(?P<on>\d+) now=(?P<now>[0-9a-fA-F]{16}) "
    r"crc=(?P<crc>[0-9a-fA-F]{8})\s*$"
)
MASKCPU_RE = re.compile(
    r"MASKCPU cpu=(?P<cpu>\d+) start=(?P<start>[0-9a-fA-F]{16}) masked=(?P<masked>[0-9a-fA-F]{16}) "
    r"regions=(?P<regions>[0-9a-fA-F]{8}) irq=(?P<irq>[0-9a-fA-F]{16}) "
    r"irqregions=(?P<irqregions>[0-9a-fA-F]{8}) max=(?P<max>[0-9a-fA-F]{8}) "
    r"maxsite=(?P<maxsite>[0-9a-fA-F]{8}) dropped=(?P<dropped>[0-9a-fA-F]{8}) "
    r"droppedticks=(?P<droppedticks>[0-9a-fA-F]{16}) crc=(?P<crc>[0-9a-fA-F]{8})\s*$"
)
MASKSITE_RE = re.compile(
    r"MASKSITE cpu=(?P<cpu>\d+) site=(?P<site>[0-9a-fA-F]{8}) count=(?P<count>[0-9a-fA-F]{8}) "
    r"ticks=(?P<ticks>[0-9a-fA-F]{16}) max=(?P<max>[0-9a-fA-F]{8}) crc=(?P<crc>[0-9a-fA-F]{8})\s*$"
)
MASKNMI_RE = re.compile(r"MASKNMI cpus=(?P<cpus>[0-9a-fA-F]{8}) crc=(?P<crc>[0-9a-fA-F]{8})\s*$")
DUMPBEGIN_RE = re.compile(
    r"DUMPBEGIN fmt=(?P<fmt>[0-9a-fA-F]{8}) run=(?P<run>[0-9a-fA-F]{16}) "
    r"dump=(?P<dump>[0-9a-fA-F]{8}) image=(?P<lo>[0-9a-fA-F]{8})-(?P<hi>[0-9a-fA-F]{8}) "
    r"build=(?P<build>[0-9a-fA-F]{8}) modes=(?P<modes>[0-9a-fA-F]{8}) "
    r"event=(?P<event>[0-9a-fA-F]{8}) period=(?P<period>[0-9a-fA-F]{8}) "
    r"mixed=(?P<mixed>[0-9a-fA-F]{8}) cpus=(?P<cpus>[0-9a-fA-F]{8}) "
    r"buf=(?P<buf>[0-9a-fA-F]{8}) "
    r"(?:tperiod=(?P<tperiod>[0-9a-fA-F]{8}) tmixed=(?P<tmixed>[0-9a-fA-F]{8}) )?"
    r"crc=(?P<crc>[0-9a-fA-F]{8})\s*$"
)
DUMPCPU_RE = re.compile(
    r"DUMPCPU cpu=(?P<cpu>\d+) total=(?P<total>[0-9a-fA-F]{8}) "
    r"retained=(?P<retained>[0-9a-fA-F]{8}) overwritten=(?P<overwritten>[0-9a-fA-F]{8}) "
    r"pmumissed=(?P<pmumissed>[0-9a-fA-F]{8}) (?:tmissed=(?P<tmissed>[0-9a-fA-F]{8}) )?"
    r"crc=(?P<crc>[0-9a-fA-F]{8})\s*$"
)
DUMPEND_RE = re.compile(
    r"DUMPEND run=(?P<run>[0-9a-fA-F]{16}) dump=(?P<dump>[0-9a-fA-F]{8}) "
    r"samples=(?P<samples>[0-9a-fA-F]{8}) crc=(?P<crc>[0-9a-fA-F]{8})\s*$"
)
MODE_TIMER, MODE_PMU = 1, 2
IRQ_SITE = 0xffff0000  # app/profiler + arch/arm irqmask.c: IRQ regions are IRQ_SITE | vector

SPSR_T_BIT = 1 << 5  # Thumb state -- same bit app/profiler.c reads


def _fnv(values) -> int:
    """FNV-1a-style fold of 32-bit words, as profiler.c's profiler_fnv()."""
    c = 0x811c9dc5
    for v in values:
        c = ((c ^ (v & 0xffffffff)) * 16777619) & 0xffffffff
    return c


def read_lines(log_path) -> list[str]:
    with open(log_path, "r", errors="replace") as f:
        return f.readlines()


def split_dumps(lines: list[str]) -> list[dict] | None:
    """K2 dumps in a log, in order: each runs from a DUMPBEGIN line to the next
    `SAMPLE done` (inclusive). A dump cut short by the next DUMPBEGIN or the end
    of the log is kept but marked incomplete. None for a log with no DUMPBEGIN
    at all (an older image's dump format)."""
    starts = [i for i, line in enumerate(lines) if "DUMPBEGIN" in line]
    if not starts:
        return None
    dumps = []
    for n, start in enumerate(starts):
        limit = starts[n + 1] if n + 1 < len(starts) else len(lines)
        end = next((i for i in range(start, limit) if lines[i].strip() == "SAMPLE done"), None)
        dumps.append({"lines": lines[start:(end + 1 if end is not None else limit)],
                      "complete": end is not None})
    return dumps


def select_dump(lines: list[str], index: int = -1) -> tuple[list[str], dict]:
    """The lines of one dump (default: the latest) and where it came from."""
    dumps = split_dumps(lines)
    if dumps is None:
        return lines, {"legacy": True, "dumps": None, "index": None, "complete": None}
    if not -len(dumps) <= index < len(dumps):
        raise ValueError(f"log has {len(dumps)} dump(s); --dump {index} does not exist")
    chosen = index % len(dumps)
    return dumps[chosen]["lines"], {"legacy": False, "dumps": len(dumps), "index": chosen,
                                    "complete": dumps[chosen]["complete"]}


def parse_session(lines: list[str]) -> dict | None:
    """K2 header, per-core lines and footer of one dump, checksum-verified.
    None when the lines hold no DUMPBEGIN."""
    header, cpus, footer, rejected = None, {}, None, 0
    for line in lines:
        if "DUMP" not in line:
            continue
        line = line.strip()
        if m := DUMPBEGIN_RE.search(line):
            v = {k: int(m[k], 16) for k in ("fmt", "run", "dump", "lo", "hi", "build", "modes",
                                             "event", "period", "mixed", "cpus", "buf")}
            words = [v["fmt"], v["run"], v["run"] >> 32, v["dump"], v["lo"], v["hi"],
                     v["build"], v["modes"], v["event"], v["period"], v["mixed"], v["cpus"],
                     v["buf"]]
            # K10 (format 3): timer-mode period in microseconds and whether it
            # changed during the session; absent before.
            v["tperiod"] = v["tmixed"] = None
            if m["tperiod"] is not None:
                v["tperiod"], v["tmixed"] = int(m["tperiod"], 16), int(m["tmixed"], 16)
                words += [v["tperiod"], v["tmixed"]]
            if _fnv(words) != int(m["crc"], 16):
                rejected += 1
                continue
            header = v
        elif m := DUMPCPU_RE.search(line):
            v = {k: int(m[k], 16) for k in ("total", "retained", "overwritten", "pmumissed")}
            cpu = int(m["cpu"])
            words = [cpu, v["total"], v["retained"], v["overwritten"], v["pmumissed"]]
            v["tmissed"] = None
            if m["tmissed"] is not None:
                v["tmissed"] = int(m["tmissed"], 16)
                words.append(v["tmissed"])
            if _fnv(words) != int(m["crc"], 16):
                rejected += 1
                continue
            cpus[cpu] = v
        elif m := DUMPEND_RE.search(line):
            v = {k: int(m[k], 16) for k in ("run", "dump", "samples")}
            if _fnv([v["run"], v["run"] >> 32, v["dump"], v["samples"]]) != int(m["crc"], 16):
                rejected += 1
                continue
            footer = v
        elif line.startswith("DUMP"):
            rejected += 1
    if header is None and footer is None and not cpus:
        return None
    problems = []
    if header is None:
        problems.append("dump header missing or damaged")
    if footer is None:
        problems.append("dump footer missing or damaged: trailing loss unknown")
    if header and footer and (header["run"], header["dump"]) != (footer["run"], footer["dump"]):
        problems.append("header and footer belong to different dumps")
    if header and len(cpus) != header["cpus"]:
        problems.append(f"{header['cpus'] - len(cpus)} per-core line(s) missing or damaged")
    if footer and cpus and len(cpus) == (header or {}).get("cpus") and \
            sum(c["retained"] for c in cpus.values()) != footer["samples"]:
        problems.append("per-core retained counts do not add up to the footer count")
    return {"header": header, "cpus": cpus, "footer": footer, "rejected": rejected,
            "problems": problems}


def image_hash(elf_path, lo: int, hi: int) -> int | None:
    """FNV-1a over the ELF's loadable bytes in [lo, hi) -- the same bytes the
    target hashes for DUMPBEGIN's build= field. None if [lo, hi) is not
    inside the file-backed load image.

    K13: alignment gaps between load segments are hashed as zeros. The target
    hashes RAM, which holds the uploaded binary, and `objcopy -O binary`
    fills those gaps with zeros (a 2-byte gap in an ARM-mode build used to
    make every dump of that image look like a mismatch)."""
    from elftools.elf.elffile import ELFFile
    if hi < lo:
        return None
    data = bytearray(hi - lo)
    spans = []
    with open(elf_path, "rb") as f:
        for seg in ELFFile(f).iter_segments():
            if seg["p_type"] != "PT_LOAD" or not seg["p_filesz"]:
                continue
            va, size = seg["p_vaddr"], seg["p_filesz"]
            spans.append((va, va + size))
            a, b = max(lo, va), min(hi, va + size)
            if a < b:
                data[a - lo:b - lo] = seg.data()[a - va:b - va]
    if not spans or lo < min(a for a, _ in spans) or hi > max(b for _, b in spans):
        return None
    h = 0x811c9dc5
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xffffffff
    return h


def check_build(elf_path, session: dict | None) -> tuple[bool | None, int | None]:
    """(True/False, host hash) when the dump states its image; (None, None) if not."""
    if not session or not session["header"]:
        return None, None
    h = session["header"]
    host = image_hash(elf_path, h["lo"], h["hi"])
    return host == h["build"], host


def sample_extra(m: re.Match) -> tuple[int, ...] | None:
    """The K6 (and K1 `slen`) fields of a SAMPLE match as checksum inputs, or None."""
    if m["src"] is None:
        return None
    extra = (ord(m["src"]), int(m["lat"], 16), int(m["msite"], 16), int(m["mgap"], 16))
    if m["slen"] is not None:
        extra += (int(m["slen"], 16),)
    return extra


def _sample_checksum(cpu: int, seq: int, pc: int, lr: int, fp: int, sp: int,
                      spsr: int, tid: int, ts: int, stack: bytes,
                      extra: tuple[int, ...] | None = None) -> int:
    """Exact mirror of profiler.c's profiler_sample_checksum() -- FNV-1a-style,
    folding each binary field into a running multiply-xor state. See that
    function's own comment for the algorithm and why it's over binary
    values, not the printed hex text."""
    c = 0x811c9dc5

    def mix(v: int) -> None:
        nonlocal c
        c = ((c ^ (v & 0xffffffff)) * 16777619) & 0xffffffff

    mix(cpu); mix(seq); mix(pc); mix(lr); mix(fp); mix(sp); mix(spsr); mix(tid)
    mix(ts & 0xffffffff); mix((ts >> 32) & 0xffffffff)
    for b in stack:
        mix(b)
    for v in extra or ():
        mix(v)
    return c


def parse_samples(log_path: str, dump: int = -1) -> list[dict]:
    """Parses "SAMPLE ..." lines, verifying each one's seq/crc fields.

    Found on real hardware (2026-09-29): a sustained UART transfer of a
    large dump occasionally corrupts a byte with an exact 64-byte
    period (a USB packet-boundary artifact in the host's serial
    adapter/driver), independent of baud rate, and not fixable by a
    settling delay since it recurs throughout a long continuous burst.
    Most such corruption already breaks hex-parseability (a NUL byte
    isn't a valid hex digit) and gets silently dropped by this regex
    not matching at all -- `seq` (a plain per-dump counter) turns that
    silent drop into a countable gap. `crc` additionally catches a
    corrupted-but-still-valid-hex line (e.g. one digit flipped to
    another), which the regex alone can never detect. Either way the
    affected sample is dropped, not trusted -- the same "count it,
    don't crash on it" resilience already used for a failed unwind
    (review finding #12), not a reason to abort the whole report.
    """
    samples = []
    expected_seq = 0
    crc_mismatches = 0
    seq_gaps = 0
    lines, _ = select_dump(read_lines(log_path), dump)
    for line in lines:
        m = SAMPLE_RE.search(line)
        if not m:
            continue
        seq = int(m["seq"], 16)
        if seq != expected_seq:
            seq_gaps += max(seq - expected_seq, 1)
        expected_seq = seq + 1

        try:
            stack = bytes.fromhex(m["stack"])
        except ValueError:
            crc_mismatches += 1
            continue

        cpu, pc, lr = int(m["cpu"]), int(m["pc"], 16), int(m["lr"], 16)
        fp, sp = int(m["fp"], 16), int(m["sp"], 16)
        spsr, tid, ts = int(m["spsr"], 16), int(m["tid"], 16), int(m["ts"], 16)
        extra = sample_extra(m)
        expected_crc = _sample_checksum(cpu, seq, pc, lr, fp, sp, spsr, tid, ts, stack,
                                         extra)
        if int(m["crc"], 16) != expected_crc:
            crc_mismatches += 1
            continue

        sample = {
            "cpu": cpu, "pc": pc, "lr": lr, "fp": fp, "sp": sp,
            "spsr": spsr, "tid": tid, "ts": ts, "stack": stack,
            "src": None, "lat": None, "msite": None, "mgap": None, "slen": None,
        }
        if extra:
            sample.update(src=m["src"], lat=extra[1], msite=extra[2], mgap=extra[3])
            if len(extra) > 4:
                if extra[4] > len(stack):
                    crc_mismatches += 1   # inconsistent record: not trusted
                    continue
                sample["slen"] = extra[4]
        samples.append(sample)

    if crc_mismatches or seq_gaps:
        print(f"warning: {crc_mismatches} sample(s) failed their checksum, "
              f"{seq_gaps} sample(s) missing entirely (seq gap) -- both "
              f"dropped, not trusted", file=sys.stderr)
    return samples


def parse_mask(log_path: str, dump: int = -1) -> dict | None:
    """Parses K6 MASKINFO/MASKCPU/MASKSITE lines (checksum-verified, the
    same drop-and-count policy as samples). Returns None when the log has
    no masked-time accounting at all (older images)."""
    info, cpus, sites, rejected, nmi = None, {}, {}, 0, None
    lines, _ = select_dump(read_lines(log_path), dump)
    for line in lines:
        if "MASK" not in line:
            continue
        line = line.strip()
        if m := MASKINFO_RE.search(line):
            freq, on, now = int(m["freq"], 16), int(m["on"]), int(m["now"], 16)
            if _fnv([freq, on, now, now >> 32]) != int(m["crc"], 16):
                rejected += 1
                continue
            info = {"cntfrq": freq, "on": bool(on), "now": now}
        elif m := MASKNMI_RE.search(line):
            bits = int(m["cpus"], 16)
            if _fnv([bits]) != int(m["crc"], 16):
                rejected += 1
                continue
            nmi = bits
        elif m := MASKCPU_RE.search(line):
            v = {k: int(m[k], 16) for k in ("start", "masked", "regions", "irq",
                                             "irqregions", "max", "maxsite", "dropped",
                                             "droppedticks")}
            cpu = int(m["cpu"])
            words = [cpu, v["start"], v["start"] >> 32, v["masked"], v["masked"] >> 32,
                     v["regions"], v["irq"], v["irq"] >> 32, v["irqregions"], v["max"],
                     v["maxsite"], v["dropped"], v["droppedticks"], v["droppedticks"] >> 32]
            if _fnv(words) != int(m["crc"], 16):
                rejected += 1
                continue
            cpus[cpu] = v
        elif m := MASKSITE_RE.search(line):
            cpu = int(m["cpu"])
            site, count = int(m["site"], 16), int(m["count"], 16)
            ticks, mx = int(m["ticks"], 16), int(m["max"], 16)
            if _fnv([cpu, site, count, ticks, ticks >> 32, mx]) != int(m["crc"], 16):
                rejected += 1
                continue
            sites.setdefault(cpu, []).append({"site": site, "count": count,
                                               "ticks": ticks, "max": mx})
        elif line.startswith("MASK"):
            rejected += 1
    if info is None and not cpus:
        return None
    if rejected:
        print(f"warning: {rejected} masked-time line(s) failed their checksum or format "
              f"-- dropped, so masked totals may be incomplete", file=sys.stderr)
    return {"info": info, "cpus": cpus, "sites": sites, "rejected": rejected, "nmi": nmi}


def classify_delays(samples: list[dict], cntfrq: int | None,
                    thresholds: dict[str, int] | None = None,
                    gap_limit: int | None = None) -> dict:
    """Marks samples whose interrupt was taken late enough to have been
    held back by IRQ masking, and attributes each one to the masked
    region that closed just before it (`msite`, ended `mgap` ticks earlier).

    `lat` is in CNTPCT ticks for timer samples ('t') and in PMU events for
    PMU samples ('p'). Undelayed samples carry only the interrupt-entry
    latency, so by default a sample counts as delayed when `lat` exceeds
    four times the 5th-percentile latency for its source (a floor of 1 us
    of ticks, or 1000 events). A low percentile, not the median: when
    masking dominates, most samples are delayed and the median is itself
    a delay. A delayed sample is attributed when the last masked region
    ended within `gap_limit` ticks (default 2 us) of the sample.
    """
    thresholds = dict(thresholds or {})
    freq = cntfrq or 54000000
    floor = {"t": max(1, freq // 1000000), "p": 1000}
    for src in ("t", "p"):
        lats = sorted(s["lat"] for s in samples if s["src"] == src)
        if lats and src not in thresholds:
            thresholds[src] = max(4 * lats[len(lats) // 20], floor[src])
    gap = gap_limit if gap_limit is not None else max(1, 2 * freq // 1000000)
    for s in samples:
        s["delayed"] = s["src"] in thresholds and s["lat"] > thresholds[s["src"]]
        s["mask_site"] = (s["msite"] if s["delayed"] and s["msite"] and s["mgap"] <= gap
                          else None)
    return {"thresholds": thresholds, "gap_limit": gap}


def site_name(elf_path: str, site: int) -> str:
    """A masking site as a readable name: the function that masked IRQs,
    or the IRQ handler (by GIC interrupt ID) for exception-entry regions."""
    if site >= IRQ_SITE:
        return f"[irq handler, GIC ID {site & 0xffff}]"
    return symbolize(elf_path, [site])[0]


def print_session(session: dict, where: dict, accepted: int, built: bool | None) -> None:
    h, f = session["header"], session["footer"]
    if h:
        modes = "+".join(n for bit, n in ((MODE_TIMER, "timer"), (MODE_PMU, "pmu"))
                         if h["modes"] & bit) or "none"
        pmu = (f", PMU event 0x{h['event']:x} every {h['period']}"
               f"{' (CONFIG CHANGED MID-SESSION)' if h['mixed'] else ''}"
               if h["modes"] & MODE_PMU else "")
        if h["modes"] & MODE_TIMER and h.get("tperiod"):
            pmu = (f", timer one sample per {h['tperiod']} us"
                   f"{' (PERIOD CHANGED MID-SESSION)' if h['tmixed'] else ''}") + pmu
        elif h["modes"] & MODE_TIMER and h.get("tperiod") is None:
            pmu = ", timer on LK's tick (before K10: phase-locks to masking)" + pmu
        print(f"dump {where['index'] + 1} of {where['dumps']} in this log: run "
              f"{h['run']:016x}, dump #{h['dump']} of that run; modes {modes}{pmu}; "
              f"ELF {'matches' if built else 'DOES NOT MATCH'} the image "
              f"({h['build']:08x})")
    for cpu, c in sorted(session["cpus"].items()):
        extra = f", {c['overwritten']} overwritten" if c["overwritten"] else ""
        extra += f", {c['pmumissed']} PMU overflows lost while masked" if c["pmumissed"] else ""
        extra += (f", {c['tmissed']} timer periods without a sample (masked throughout)"
                  if c.get("tmissed") else "")
        print(f"  cpu{cpu}: {c['total']} taken, {c['retained']} retained{extra}")
    if f:
        lost = f["samples"] - accepted
        print(f"records: {f['samples']} sent, {accepted} accepted"
              f"{f', {lost} lost or rejected in transfer' if lost else ', none lost'}")
    for problem in session["problems"]:
        print(f"warning: {problem}", file=sys.stderr)
    if where["dumps"] and where["dumps"] > 1:
        print(f"note: the log holds {where['dumps']} dumps; reporting dump "
              f"{where['index'] + 1}, the others are ignored (--dump N picks another)",
              file=sys.stderr)
    print()


def print_mask_report(elf_path: str, samples: list[dict], mask: dict | None,
                      delay: dict | None) -> None:
    if mask and mask["info"]:
        info = mask["info"]
        freq = info["cntfrq"] or 1
        us = lambda ticks: 1e6 * ticks / freq
        nmi = mask.get("nmi") or 0
        if nmi:
            cores = ", ".join(f"cpu{c}" for c in range(32) if nmi >> c & 1)
            print(f"\nIRQ-masked time; {cores} masked by GIC priority (pseudo-NMI): PMU samples "
                  "reach that masked code, timer samples and IRQ handlers do not. Accounting "
                  f"{'on' if info['on'] else 'off'} at dump time:")
        else:
            print("\nIRQ-masked time (invisible to IRQ sampling), accounting "
                  f"{'on' if info['on'] else 'off'} at dump time:")
        total_window = total_masked = 0
        for cpu in sorted(mask["cpus"]):
            v = mask["cpus"][cpu]
            window = info["now"] - v["start"] if v["start"] and info["now"] > v["start"] else 0
            total_window += window
            total_masked += v["masked"]
            pct = 100 * v["masked"] / window if window else 0.0
            ipct = 100 * v["irq"] / window if window else 0.0
            extra = (f", {v['dropped']} region(s) at unrecorded sites"
                     if v["dropped"] else "")
            print(f"  cpu{cpu}: {pct:5.2f}% masked ({ipct:.2f}% in IRQ handlers), "
                  f"{v['regions']} regions, longest {us(v['max']):.1f} us at "
                  f"{site_name(elf_path, v['maxsite'])}{extra}")
        merged: dict[int, dict] = {}
        for cpu_sites in mask["sites"].values():
            for e in cpu_sites:
                m = merged.setdefault(e["site"], {"count": 0, "ticks": 0, "max": 0})
                m["count"] += e["count"]
                m["ticks"] += e["ticks"]
                m["max"] = max(m["max"], e["max"])
        if merged and total_window:
            print(f"\n  {'% of time':>9}  {'regions':>8}  {'longest':>10}  masking site")
            for site, m in sorted(merged.items(), key=lambda kv: -kv[1]["ticks"])[:15]:
                print(f"  {100 * m['ticks'] / total_window:8.3f}%  {m['count']:>8}  "
                      f"{us(m['max']):8.1f}us  {site_name(elf_path, site)}")
    if delay:
        print("\nSamples delayed by IRQ masking (taken late, so the PC is the unmask point):")
        for src, label in (("t", "timer, CNTPCT ticks"), ("p", "PMU, events")):
            group = [s for s in samples if s["src"] == src]
            if not group:
                continue
            late = [s for s in group if s["delayed"]]
            named = [s for s in late if s["mask_site"]]
            print(f"  {label}: {len(late)}/{len(group)} delayed "
                  f"(threshold {delay['thresholds'][src]}), {len(named)} attributed "
                  f"to the region that held them")
        by_site = Counter(site_name(elf_path, s["mask_site"])
                          for s in samples if s.get("mask_site"))
        for name, count in by_site.most_common(10):
            print(f"  {count:>8}  {name}")


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


def unwind_sample(unwinder: DwarfCFIUnwinder, s: dict, lr_fallback: bool = True) -> list[int]:
    thumb = (s["spsr"] & SPSR_T_BIT) != 0
    fp_reg = 7 if thumb else 11
    registers = {SP_REG: s["sp"], LR_REG: s["lr"], fp_reg: s["fp"]}
    # K1: only the copied bytes are stack; reads past them are a clean stop
    stack = s["stack"] if s.get("slen") is None else s["stack"][:s["slen"]]
    read_memory = make_read_memory(stack, s["sp"])
    # A copy cut short (0 < slen < window) ended exactly at the stack top.
    top = (s["sp"] + s["slen"]
           if s.get("slen") is not None and 0 < s["slen"] < len(s["stack"]) else None)
    return unwinder.unwind(s["pc"], registers, read_memory, stack_top=top,
                           lr_fallback=lr_fallback)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("log", help="captured serial log containing 'profiler dump' output")
    ap.add_argument("elf", help="the built lk.elf to unwind/symbolize against")
    ap.add_argument("--folded", help="write a FlameGraph-compatible folded-stack file here")
    ap.add_argument("--annotate", type=int, metavar="N",
                    help="disassemble the N hottest functions with per-instruction "
                         "sample counts (needs arm-none-eabi-objdump on PATH)")
    ap.add_argument("--dump", type=int, default=-1,
                    help="which dump of the log to report: 0 = first, -1 = latest (default)")
    ap.add_argument("--allow-elf-mismatch", action="store_true",
                    help="report even if the ELF does not match the captured image")
    ap.add_argument("--no-lr-fallback", action="store_true",
                    help="do not use the interrupted LR as the caller of code without CFI "
                         "(the behaviour before K3)")
    ap.add_argument("--no-mask-frames", action="store_true",
                    help="do not add [irq-masked: ...] leaf frames to delayed samples "
                         "in --folded output")
    args = ap.parse_args()

    all_lines = read_lines(args.log)
    try:
        dump_lines, where = select_dump(all_lines, args.dump)
    except ValueError as error:
        sys.exit(f"error: {error}")
    session = parse_session(dump_lines)
    if session:
        built, host_hash = check_build(args.elf, session)
        h = session["header"]
        if built is False and not args.allow_elf_mismatch:
            got = ("does not contain the whole image range "
                   f"{h['lo']:08x}-{h['hi']:08x}" if host_hash is None
                   else f"hashes to {host_hash:08x}")
            sys.exit(f"error: {args.elf} does not match the captured image (image hash "
                     f"{h['build']:08x}; the ELF {got}); use the ELF from the same build, "
                     f"or --allow-elf-mismatch")
    samples = parse_samples(args.log, args.dump)
    if not samples:
        sys.exit(f"error: no SAMPLE lines found in {args.log}")
    if session:
        print_session(session, where, len(samples), built)

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
    lr_fallbacks = 0
    with DwarfCFIUnwinder(args.elf) as unwinder:
        for s in samples:
            try:
                chains.append(unwind_sample(unwinder, s, not args.no_lr_fallback))
                lr_fallbacks += unwinder.last_lr_fallback
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
    if lr_fallbacks:
        print(f"no-CFI code: {lr_fallbacks} sample(s) in assembly without unwind data got their "
              f"caller from the interrupted LR (checked to follow a call); their stacks end there")
    bounded = [s for s in samples if s.get("slen") is not None]
    if bounded:
        short = sum(1 for s in bounded if 0 < s["slen"] < len(s["stack"]))
        none = sum(1 for s in bounded if s["slen"] == 0)
        print(f"stack window: {short} sample(s) bounded by their stack top (< "
              f"{len(bounded[0]['stack'])} bytes above SP), {none} with SP on no known "
              f"stack (nothing copied)")
    print()
    print(f"{'count':>8}  {'%':>6}  leaf function (source line)")
    for name, count in leaf_counts.most_common(30):
        line = line_by_name.get(name)
        suffix = f"  ({line})" if line else ""
        print(f"{count:>8}  {100 * count / total:5.1f}%  {name}{suffix}")

    mask = parse_mask(args.log, args.dump)
    delay = None
    if any(s["src"] for s in samples):
        delay = classify_delays(samples, mask["info"]["cntfrq"]
                                if mask and mask["info"] else None)
    print_mask_report(args.elf, samples, mask, delay)

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
        for s, chain in zip(samples, chains):
            names = symbolize(args.elf, chain)
            # root-to-leaf for FlameGraph, chain itself is leaf-to-root
            stack = list(reversed(names))
            if delay and not args.no_mask_frames and s.get("delayed"):
                stack.append(f"[irq-masked: {site_name(args.elf, s['mask_site'])}]"
                             if s["mask_site"] else "[irq-delayed]")
            folded_counts[";".join(stack)] += 1
        # flamegraph.pl rejects every line ending in "\r\n"; text mode on Windows writes that
        with open(args.folded, "w", newline="\n") as f:
            for stack, count in folded_counts.items():
                f.write(f"{stack} {count}\n")
        print(f"\nwrote {args.folded} -- render with: "
              f"perl scripts/flamegraph.pl {args.folded} > flame.svg")


if __name__ == "__main__":
    main()
