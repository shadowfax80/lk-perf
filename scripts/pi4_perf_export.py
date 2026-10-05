#!/usr/bin/env python3
"""Export one completed lk-perf dump as timestamped perf-script text.

Usage: python3 scripts/pi4_perf_export.py capture.log matching-lk.elf \
    --output capture.perf --mode timer

The accompanying .metadata.json records artifact hashes, synthetic thread IDs,
operator-supplied event information, data-quality counts, and known limitations.
No missing frames or execution with IRQs masked can be reconstructed by export.
See docs/EXPORT.md. This emits text for viewers, not a native perf.data file.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import TextIO
import uuid

from elftools.common.exceptions import ELFError

from dwarf_unwind import DwarfCFIUnwinder, find_function, strip_isa_bit
from pi4_pc_histogram import SAMPLE_RE, _sample_checksum, sample_extra, unwind_sample


KNOWN_LIMITATIONS = [
    "Missing stack frames cannot be recovered: the 128-byte snapshot, missing "
    "registers/CFI, unsupported CFI rules, and optimized-away frames limit unwinding.",
    "Execution while IRQs are masked is invisible to both IRQ-driven sampling "
    "modes; a delayed sample cannot reconstruct that execution.",
    "The target dump has no build/run/mode/event identity. ELF pairing and "
    "single-mode capture are operator responsibilities; artifact hashes do not "
    "prove that the ELF matches the running image.",
    "The completion marker is required, but no target footer count proves "
    "absence of trailing loss or ring overwrite; captures must be stopped "
    "before dumping for a coherent snapshot.",
    "Thread-object pointers may be reused. Synthetic IDs identify pointer "
    "values within this export, not operating-system PIDs or thread lifetimes.",
    "Timestamps are target generic-timer microseconds, not a host/Linux clock. "
    "External importers may round timestamps or omit CPU/event/period fields.",
    "Scheduling, wakeups, blocking reasons/durations, and CPU-frequency history "
    "are not captured. Stack samples cannot establish scheduler latency, "
    "off-CPU wait causes, or frequency changes; additional instrumentation "
    "and event export are required.",
]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_capture(log: Path) -> tuple[list[dict], dict]:
    """Require exactly one completed dump; discard corrupt records with counts.

    Sequence is validated only after checksum verification: a damaged sequence
    field must not make otherwise legitimate captures look like a second run.
    Gap counts can overlap rejected records and are not a total loss estimate.
    """
    samples = []
    quality = {"checksum_rejections": 0, "malformed_records": 0,
               "sequence_gaps": 0, "dump_done_observed": False,
               "trailing_loss_count_known": False, "overwrite_count_known": False}
    expected_seq = 0
    with log.open("r", encoding="utf-8", errors="replace") as stream:
        for line_number, line in enumerate(stream, 1):
            if line.strip() == "SAMPLE done":
                if quality["dump_done_observed"]:
                    raise ValueError("multiple dumps: use a fresh log containing one dump")
                quality["dump_done_observed"] = True
                continue
            if "SAMPLE seq=" not in line:
                continue
            if quality["dump_done_observed"]:
                raise ValueError(f"sample after completion marker at line {line_number}; "
                                 "multiple dumps are not supported")
            match = SAMPLE_RE.search(line)
            if (match is None or line[match.end():].strip()
                    or len(match["cpu"]) > 10
                    or any(len(match[field]) != 8 for field in
                           ("seq", "pc", "lr", "fp", "sp", "spsr", "tid", "crc"))
                    or len(match["ts"]) != 16 or len(match["stack"]) != 256
                    or (match["src"] is not None
                        and any(len(match[field]) != 8 for field in ("lat", "msite", "mgap")))):
                quality["malformed_records"] += 1
                continue
            sample = {field: int(match[field], 16) for field in
                      ("seq", "pc", "lr", "fp", "sp", "spsr", "tid", "ts")}
            sample["cpu"] = int(match["cpu"])
            sample["stack"] = bytes.fromhex(match["stack"])
            if sample["cpu"] > 0xffffffff:
                quality["malformed_records"] += 1
                continue
            checksum = _sample_checksum(
                sample["cpu"], sample["seq"], sample["pc"], sample["lr"],
                sample["fp"], sample["sp"], sample["spsr"], sample["tid"],
                sample["ts"], sample["stack"], sample_extra(match))
            if checksum != int(match["crc"], 16):
                quality["checksum_rejections"] += 1
                continue
            if sample["ts"] > ((1 << 63) - 1) // 1000:
                raise ValueError("timestamp exceeds signed 64-bit nanosecond importer range")
            if sample["seq"] < expected_seq:
                raise ValueError(f"sequence reset/duplicate at line {line_number}; "
                                 "multiple or reordered dumps are not supported")
            quality["sequence_gaps"] += sample["seq"] - expected_seq
            expected_seq = sample["seq"] + 1
            samples.append(sample)
    if not quality["dump_done_observed"]:
        raise ValueError("incomplete dump: missing SAMPLE done marker")
    if not samples:
        raise ValueError("no checksum-valid samples in completed dump")
    return samples, quality


def frame_symbol(elf: Path, pc: int, is_return: bool) -> str:
    """Validate the function extent, using call-site lookup for return frames.

    Keep the original normalized PC in the output; use PC-1 only for finding
    the symbol of a recovered return address. Zero-size symbols are unknown.
    """
    lookup = pc - 1 if is_return else pc
    function = find_function(str(elf), lookup)
    if function is None:
        return "[unknown]"
    name, start, size, _thumb = function
    if not start <= lookup < start + size:
        return "[unknown]"
    # ELF names are untrusted text. Keep each frame on one line and disallow
    # mapping delimiters; preserve ordinary C/C++ symbol characters otherwise.
    name = re.sub(r"[\s()]", "_", name)
    return name or "[unknown]"


def write_perf_script(stream: TextIO, elf: Path, samples: list[dict], *,
                      event: str, period: int) -> dict:
    """Write leaf-to-root callchains, globally sorted by integer microseconds."""
    thread_ids = {pointer: index + 1
                  for index, pointer in enumerate(sorted({s["tid"] for s in samples}))}
    ordered = sorted(samples, key=lambda s: (s["ts"], s["cpu"], s["seq"]))
    mapping = re.sub(r"[^A-Za-z0-9_.-]", "_", elf.name) or "lk.elf"
    errors = 0
    depths = Counter()
    with DwarfCFIUnwinder(str(elf)) as unwinder:
        for sample in ordered:
            try:
                chain = unwind_sample(unwinder, sample)
            except (ValueError, NotImplementedError):
                errors += 1
                chain = [sample["pc"]]
            chain = [strip_isa_bit(pc) for pc in chain]
            depths[len(chain)] += 1
            seconds, micros = divmod(sample["ts"], 1000000)
            stream.write(f"lk-{sample['tid']:08x} 1/{thread_ids[sample['tid']]} "
                         f"[{sample['cpu']:03d}] {seconds}.{micros:06d}: "
                         f"{period} {event}:\n")
            for depth, pc in enumerate(chain):
                symbol = frame_symbol(elf, pc, depth > 0)
                stream.write(f"\t{pc:08x} {symbol} ({mapping})\n")
            # Blank separator, including after the final record, is required
            # by Perfetto's streaming perf-text tokenizer. No comment header.
            stream.write("\n")
    return {
        "unwind_exception_samples": errors,
        "unwind_depth_counts": dict(sorted(depths.items())),
        "unwind_completeness": "not_proven",
        "threads": [{"pid": 1, "tid": tid, "thread_pointer": f"0x{pointer:08x}",
                     "comm": f"lk-{pointer:08x}"}
                    for pointer, tid in thread_ids.items()],
    }


def export_capture(log: Path, elf: Path, output: Path, *, mode: str = "unknown",
                   event: str | None = None, period: int | None = None,
                   image: Path | None = None, run_id: str | None = None) -> dict:
    """Export with a sidecar; never overwrite inputs or existing outputs."""
    if mode not in ("timer", "pmu", "unknown"):
        raise ValueError("mode must be timer, pmu, or unknown")
    if mode == "pmu" and (event is None or period is None):
        raise ValueError("PMU export requires --event and --period from the capture")
    event = event or ("timer-tick" if mode == "timer" else "lk-sample")
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.-]*", event):
        raise ValueError("event must be a simple label (letters, digits, _, ., -)")
    if period is not None and not 1 <= period <= 0xffffffff:
        raise ValueError("period must be in 1..4294967295")
    sidecar = Path(str(output) + ".metadata.json")
    inputs = [log, elf] + ([image] if image else [])
    if any(target.resolve() == source.resolve()
           for target in (output, sidecar) for source in inputs):
        raise ValueError("output must not overwrite a capture, ELF, or image")
    if output.exists() or sidecar.exists():
        raise ValueError("output or metadata already exists; choose a fresh output path")
    samples, quality = read_capture(log)
    artifacts = {"log": {"path": str(log), "sha256": sha256_file(log)},
                 "elf": {"path": str(elf), "sha256": sha256_file(elf)}}
    if image:
        artifacts["image"] = {"path": str(image), "sha256": sha256_file(image)}
    # Do not leave a seemingly usable export after a write/ELF failure.
    created_output = False
    created_sidecar = False
    try:
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            created_output = True
            unwind = write_perf_script(stream, elf, samples, event=event,
                                       period=period if period is not None else 1)
        metadata = {
            "schema_version": 1, "format": "perf-script-text",
            "export_id": str(uuid.uuid4()), "capture_run_id": run_id,
            "artifacts": artifacts,
            "capture": {"mode": mode, "event": event, "period": period,
                        "metadata_source": "operator-supplied; not target-verified",
                        "clock": "target-generic-timer-microseconds",
                        "clock_offset_applied_us": 0, "pid": 1,
                        "pid_tid_kind": "synthetic; pointer mapping below",
                        "header_period_kind": "operator-period" if period is not None
                                              else "unit-sample-weight",
                        "first_timestamp_us": min(s["ts"] for s in samples),
                        "last_timestamp_us": max(s["ts"] for s in samples)},
            "quality": {**quality, **unwind, "exported_samples": len(samples),
                        "per_cpu_samples": dict(sorted(Counter(
                            s["cpu"] for s in samples).items()))},
            "known_limitations": KNOWN_LIMITATIONS,
            "output": {"path": str(output), "sha256": sha256_file(output)},
        }
        with sidecar.open("x", encoding="utf-8", newline="\n") as stream:
            created_sidecar = True
            stream.write(json.dumps(metadata, indent=2) + "\n")
    except Exception:
        if created_output:
            output.unlink(missing_ok=True)
        if created_sidecar:
            sidecar.unlink(missing_ok=True)
        raise
    return metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log", type=Path)
    parser.add_argument("elf", type=Path, help="unstripped ELF matching the captured image")
    parser.add_argument("--output", type=Path, required=True, help="new .perf text file")
    parser.add_argument("--mode", choices=("timer", "pmu", "unknown"), default="unknown")
    parser.add_argument("--event", help="capture event label, e.g. cpu-cycles or event-0x08")
    parser.add_argument("--period", type=int, help="PMU overflow period, decimal")
    parser.add_argument("--image", type=Path, help="optional matching image to hash")
    parser.add_argument("--run-id", help="optional operator-assigned capture ID")
    args = parser.parse_args()
    try:
        metadata = export_capture(args.log, args.elf, args.output, mode=args.mode,
                                  event=args.event, period=args.period,
                                  image=args.image, run_id=args.run_id)
    except (OSError, ValueError, NotImplementedError, ELFError) as error:
        parser.exit(1, f"error: {error}\n")
    quality = metadata["quality"]
    print(f"wrote {args.output} and {args.output}.metadata.json "
          f"({quality['exported_samples']} samples)")
    if any(quality[key] for key in ("checksum_rejections", "malformed_records",
                                   "sequence_gaps", "unwind_exception_samples")):
        print(f"warning: checksum rejections={quality['checksum_rejections']}, "
              f"malformed={quality['malformed_records']}, "
              f"sequence gaps={quality['sequence_gaps']} (may overlap rejections), "
              f"leaf-only unwind fallbacks={quality['unwind_exception_samples']}",
              file=sys.stderr)
    print("known limitations: missing stack frames and IRQ-masked execution "
          "cannot be recovered; scheduling/wakeup/blocking/frequency history "
          "is not captured; see metadata sidecar", file=sys.stderr)


if __name__ == "__main__":
    main()
