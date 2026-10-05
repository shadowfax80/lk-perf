#!/usr/bin/env python3
"""Exporter regressions and optional real Perfetto importer verification.

python3 scripts/test_perf_export.py
python3 scripts/test_perf_export.py --trace-processor /path/to/trace_processor

The optional importer is an external tool, not bundled or mocked. Tests build
an assembly ELF and model interrupted stack bytes; they never access the Pi.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from elftools.elf.elffile import ELFFile

from pi4_pc_histogram import _sample_checksum
from pi4_perf_export import export_capture, frame_symbol, read_capture

ROOT = Path(__file__).resolve().parent
TRACE_PROCESSOR = None


def sample_line(seq=0, cpu=0, pc=0x8000, lr=0, fp=0x1000, sp=0x1000,
                spsr=0x33, tid=0x80002000, ts=1000000, stack=None):
    stack = bytes(128) if stack is None else stack
    crc = _sample_checksum(cpu, seq, pc, lr, fp, sp, spsr, tid, ts, stack)
    return (f"SAMPLE seq={seq:08x} cpu={cpu} pc={pc:08x} lr={lr:08x} "
            f"fp={fp:08x} sp={sp:08x} spsr={spsr:08x} tid={tid:08x} "
            f"ts={ts:016x} stack={stack.hex()} crc={crc:08x}\n")


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / "capture.log"

    def read(self, text):
        self.log.write_text(text, encoding="utf-8")
        return read_capture(self.log)

    def test_requires_completed_dump(self):
        with self.assertRaisesRegex(ValueError, "missing SAMPLE done"):
            self.read(sample_line())

    def test_rejects_two_complete_dumps(self):
        with self.assertRaisesRegex(ValueError, "sample after completion"):
            self.read((sample_line() + "SAMPLE done\n") * 2)

    def test_rejects_second_completion_even_without_samples(self):
        with self.assertRaisesRegex(ValueError, "multiple dumps"):
            self.read(sample_line() + "SAMPLE done\nSAMPLE done\n")

    def test_rejects_duplicate_or_reset_sequence(self):
        with self.assertRaisesRegex(ValueError, "sequence reset/duplicate"):
            self.read(sample_line() + sample_line() + "SAMPLE done\n")

    def test_counts_crc_corruption_and_sequence_gap(self):
        corrupted = sample_line(seq=1).replace("pc=00008000", "pc=00008002")
        samples, quality = self.read(sample_line() + corrupted + sample_line(seq=2)
                                     + "SAMPLE done\n")
        self.assertEqual(len(samples), 2)
        self.assertEqual(quality["checksum_rejections"], 1)
        self.assertEqual(quality["sequence_gaps"], 1)

    def test_damaged_sequence_does_not_reset_run_detection(self):
        corrupted = sample_line(seq=1).replace("seq=00000001", "seq=00000000")
        samples, quality = self.read(sample_line() + corrupted + sample_line(seq=2)
                                     + "SAMPLE done\n")
        self.assertEqual(len(samples), 2)
        self.assertEqual(quality["checksum_rejections"], 1)

    def test_rejects_short_stack_with_valid_checksum(self):
        _, quality = self.read(sample_line(stack=bytes(4)) + sample_line(seq=1)
                               + "SAMPLE done\n")
        self.assertEqual(quality["malformed_records"], 1)
        self.assertEqual(quality["sequence_gaps"], 1)

    def test_records_uncountable_trailing_loss(self):
        _, quality = self.read(sample_line() + "SAMPLE seq=corrupted\nSAMPLE done\n")
        self.assertEqual(quality["malformed_records"], 1)
        self.assertFalse(quality["trailing_loss_count_known"])
        self.assertFalse(quality["overwrite_count_known"])

    def test_empty_completed_dump_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "no checksum-valid samples"):
            self.read("SAMPLE done\n")

    def test_out_of_range_timestamp_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "timestamp exceeds"):
            self.read(sample_line(ts=(1 << 64) - 1) + "SAMPLE done\n")

    def test_console_noise_and_crlf_are_accepted(self):
        samples, quality = self.read("boot banner\n] profiler dump\n" +
                                     sample_line().replace("\n", "\r\n") +
                                     "SAMPLE done\r\n] ")
        self.assertEqual(len(samples), 1)
        self.assertTrue(quality["dump_done_observed"])


@unittest.skipUnless(shutil.which("arm-none-eabi-as") and shutil.which("arm-none-eabi-ld"),
                     "requires ARM GNU assembler/linker for real CFI fixtures")
class ExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build_temp.cleanup)
        cls.fixture = Path(cls.build_temp.name)
        cls.elf = cls.fixture / "fixture.elf"
        subprocess.run(["arm-none-eabi-as", "-g", "-mcpu=cortex-a15",
                        str(ROOT / "testdata/perf_export.S"), "-o",
                        str(cls.fixture / "fixture.o")], check=True)
        subprocess.run(["arm-none-eabi-ld", "-Ttext=0x8000", "-e", "root",
                        str(cls.fixture / "fixture.o"), "-o", str(cls.elf)], check=True)
        with cls.elf.open("rb") as stream:
            elf = ELFFile(stream)
            cls.symbols = {symbol.name: int(symbol["st_value"]) & ~1
                           for symbol in elf.get_section_by_name(".symtab").iter_symbols()}

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.log = self.work / "capture.log"
        self.output = self.work / "capture.perf"
        stack = bytearray(128)
        for offset, value in {0: 0x1008, 4: self.symbols["root_return"] | 1,
                              8: 0x1010, 12: 0}.items():
            stack[offset:offset + 4] = value.to_bytes(4, "little")
        self.stack = bytes(stack)
        self.log.write_text(self.good_line() + "SAMPLE done\n", encoding="utf-8")

    def good_line(self, **kw):
        fields = {"pc": self.symbols["leaf"] | 1,
                  "lr": self.symbols["caller_return"] | 1, "stack": self.stack}
        fields.update(kw)
        return sample_line(**fields)

    def export(self, **kw):
        return export_capture(self.log, self.elf, self.output, **kw)

    def test_real_cfi_chain_and_thread_identity(self):
        metadata = self.export(mode="timer", run_id="fixture-run")
        blocks = self.output.read_text().strip().split("\n\n")
        names = [line.split()[1] for line in blocks[0].splitlines()[1:]]
        self.assertEqual(names, ["leaf", "caller", "root"])
        self.assertIn("lk-80002000 1/1 [000] 1.000000: 1 timer-tick:", blocks[0])
        self.assertEqual(metadata["capture_run_id"], "fixture-run")
        self.assertEqual(metadata["quality"]["threads"][0]["thread_pointer"], "0x80002000")
        self.assertEqual(metadata["quality"]["unwind_exception_samples"], 0)

    def test_global_order_and_microsecond_precision(self):
        self.log.write_text(self.good_line(seq=0, cpu=0, ts=2000001) +
                            self.good_line(seq=1, cpu=3, ts=1000001) +
                            self.good_line(seq=2, cpu=0, ts=1000000) +
                            "SAMPLE done\n")
        self.export()
        headers = self.output.read_text().split("\n\n")[:3]
        self.assertIn("[000] 1.000000:", headers[0])
        self.assertIn("[003] 1.000001:", headers[1])
        self.assertIn("[000] 2.000001:", headers[2])

    def test_migrating_thread_keeps_tid_and_other_pointer_gets_distinct_tid(self):
        self.log.write_text(self.good_line(seq=0, cpu=0) +
                            self.good_line(seq=1, cpu=3) +
                            self.good_line(seq=2, tid=0x90000000) + "SAMPLE done\n")
        metadata = self.export()
        headers = [block.splitlines()[0] for block in self.output.read_text().split("\n\n")
                   if block]
        self.assertIn("1/1 [000]", headers[0])
        self.assertIn("1/2 [000]", headers[1])
        self.assertIn("1/1 [003]", headers[2])
        self.assertEqual(len(metadata["quality"]["threads"]), 2)

    def test_missing_register_degrades_only_affected_sample(self):
        self.log.write_text(self.good_line() + self.good_line(
            seq=1, pc=self.symbols["arm_leaf"], spsr=0x13, fp=0) + "SAMPLE done\n")
        metadata = self.export()
        blocks = self.output.read_text().strip().split("\n\n")
        self.assertEqual(len(blocks[0].splitlines()), 4)
        self.assertEqual(len(blocks[1].splitlines()), 2)
        self.assertIn(" arm_leaf ", blocks[1])
        self.assertEqual(metadata["quality"]["unwind_exception_samples"], 1)

    def test_no_cfi_uses_checked_lr_and_does_not_claim_complete_unwind(self):
        # K3: LR here is a real return address (after `bl leaf` in caller),
        # so the no-CFI leaf gets that caller and the stack ends there.
        self.log.write_text(self.good_line(pc=self.symbols["no_cfi"], spsr=0x13)
                            + "SAMPLE done\n")
        metadata = self.export()
        text = self.output.read_text()
        self.assertIn(" no_cfi ", text)
        self.assertIn(" caller ", text)
        self.assertEqual(metadata["quality"]["unwind_depth_counts"], {2: 1})
        self.assertEqual(metadata["quality"]["lr_fallback_samples"], 1)
        self.assertEqual(metadata["quality"]["unwind_completeness"], "not_proven")

    def test_no_cfi_with_lr_not_after_a_call_keeps_leaf_only(self):
        self.log.write_text(self.good_line(pc=self.symbols["no_cfi"], spsr=0x13,
                                           lr=self.symbols["caller"] | 1)
                            + "SAMPLE done\n")
        metadata = self.export()
        self.assertEqual(metadata["quality"]["unwind_depth_counts"], {1: 1})
        self.assertEqual(metadata["quality"]["lr_fallback_samples"], 0)

    def test_unmapped_pc_is_not_attributed_to_nearest_function(self):
        self.log.write_text(self.good_line(pc=0x12340000, spsr=0x13) + "SAMPLE done\n")
        self.export()
        self.assertIn("12340000 [unknown]", self.output.read_text())

    def test_return_address_at_function_boundary_names_the_caller(self):
        # The Thumb leaf immediately follows caller's final pop. A recovered
        # return address at that boundary must not be named as the next leaf.
        boundary = self.symbols["leaf"]
        self.assertEqual(frame_symbol(self.elf, boundary, False), "leaf")
        self.assertEqual(frame_symbol(self.elf, boundary, True), "caller")

    def test_metadata_hashes_and_known_limitations(self):
        image = self.work / "lk.bin"
        image.write_bytes(b"offline test image")
        metadata = self.export(image=image)
        saved = json.loads(Path(str(self.output) + ".metadata.json").read_text())
        self.assertEqual(saved["export_id"], metadata["export_id"])
        self.assertEqual(len(saved["artifacts"]["elf"]["sha256"]), 64)
        self.assertIn("image", saved["artifacts"])
        self.assertIsNone(saved["capture"]["period"])
        self.assertIn("Missing stack frames", saved["known_limitations"][0])
        self.assertIn("IRQs are masked", saved["known_limitations"][1])
        self.assertNotIn(b"\r", self.output.read_bytes())
        self.assertTrue(self.output.read_bytes().endswith(b"\n\n"))

    def test_pmu_metadata_requires_event_and_period(self):
        with self.assertRaisesRegex(ValueError, "requires --event and --period"):
            self.export(mode="pmu")
        metadata = self.export(mode="pmu", event="cpu-cycles", period=1000000)
        self.assertIn("1000000 cpu-cycles:", self.output.read_text())
        self.assertEqual(metadata["capture"]["period"], 1000000)
        self.assertIn("not target-verified", metadata["capture"]["metadata_source"])

    def test_invalid_event_and_period(self):
        for args in ({"event": "cycles:\nforged"}, {"period": 0},
                     {"period": 1 << 32}):
            with self.subTest(args=args), self.assertRaises(ValueError):
                self.export(**args)
        self.assertFalse(self.output.exists())

    def test_does_not_overwrite_inputs_or_existing_output(self):
        with self.assertRaisesRegex(ValueError, "must not overwrite"):
            export_capture(self.log, self.elf, self.log)
        self.output.write_text("existing result")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.export()
        self.assertEqual(self.output.read_text(), "existing result")

    def test_no_partial_output_after_invalid_elf(self):
        bad = self.work / "bad.elf"
        bad.write_bytes(b"not an ELF")
        with self.assertRaises(Exception):
            export_capture(self.log, bad, self.output)
        self.assertFalse(self.output.exists())
        self.assertFalse(Path(str(self.output) + ".metadata.json").exists())

    def test_command_line_generates_export_and_prints_limitations(self):
        result = subprocess.run([sys.executable, str(ROOT / "pi4_perf_export.py"),
                                 str(self.log), str(self.elf), "--output", str(self.output),
                                 "--mode", "timer"], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("IRQ-masked", result.stderr)
        self.assertEqual(json.loads(Path(str(self.output) + ".metadata.json").read_text())
                         ["quality"]["exported_samples"], 1)

    def test_command_line_rejects_incomplete_capture_without_outputs(self):
        self.log.write_text(self.good_line())
        result = subprocess.run([sys.executable, str(ROOT / "pi4_perf_export.py"),
                                 str(self.log), str(self.elf), "--output", str(self.output)],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing SAMPLE done", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertFalse(self.output.exists())

    def test_real_perfetto_importer(self):
        if TRACE_PROCESSOR is None:
            self.skipTest("pass --trace-processor for consumer verification")
        self.log.write_text(self.good_line(seq=0, cpu=0, ts=2000001) +
                            self.good_line(seq=1, cpu=0, ts=1000001) +
                            self.good_line(seq=2, cpu=3, ts=1000000, tid=0x90000000,
                                           pc=self.symbols["arm_leaf"], spsr=0x13, fp=0) +
                            self.good_line(seq=3, cpu=3, ts=2000000, tid=0x90000000,
                                           pc=self.symbols["no_cfi"], spsr=0x13) +
                            "SAMPLE done\n")
        self.export(mode="timer")

        def query(sql):
            sql_file = self.work / "query.sql"
            sql_file.write_text(sql)
            prefix = [sys.executable, str(TRACE_PROCESSOR)] if Path(
                TRACE_PROCESSOR).read_bytes().startswith(b"#!") else [str(TRACE_PROCESSOR)]
            result = subprocess.run(prefix + ["query", "-f", str(sql_file), str(self.output)],
                                    text=True, capture_output=True, check=True)
            return list(csv.DictReader(io.StringIO(result.stdout)))

        rows = query("SELECT s.ts, t.tid, p.pid FROM cpu_profile_stack_sample s "
                     "JOIN thread t USING(utid) JOIN process p USING(upid) ORDER BY s.ts;")
        self.assertEqual(len(rows), 4, rows)
        self.assertEqual([int(r["tid"]) for r in rows], [2, 1, 2, 1])
        self.assertEqual({int(r["pid"]) for r in rows}, {1})
        for row, expected in zip(rows, [1000000000, 1000001000, 2000000000, 2000001000]):
            self.assertLessEqual(abs(int(row["ts"]) - expected), 1)
        names = query("SELECT name FROM stack_profile_frame ORDER BY name;")
        self.assertEqual({r["name"] for r in names},
                         {"root", "caller", "leaf", "arm_leaf", "no_cfi"})
        stacks = query("WITH RECURSIVE chain(ts, depth, name, parent_id) AS ("
                       "SELECT s.ts, c.depth, f.name, c.parent_id "
                       "FROM cpu_profile_stack_sample s "
                       "JOIN stack_profile_callsite c ON c.id=s.callsite_id "
                       "JOIN stack_profile_frame f ON f.id=c.frame_id UNION ALL "
                       "SELECT chain.ts, c.depth, f.name, c.parent_id FROM chain "
                       "JOIN stack_profile_callsite c ON c.id=chain.parent_id "
                       "JOIN stack_profile_frame f ON f.id=c.frame_id) "
                       "SELECT ts, group_concat(name, ';') AS stack FROM "
                       "(SELECT ts, name FROM chain ORDER BY ts, depth) GROUP BY ts ORDER BY ts;")
        self.assertEqual([r["stack"] for r in stacks],
                         ["arm_leaf", "root;caller;leaf", "no_cfi", "root;caller;leaf"])
        errors = query("SELECT name, value FROM stats WHERE severity = 'error' AND value > 0;")
        self.assertEqual(errors, [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--trace-processor", type=Path)
    args, rest = parser.parse_known_args()
    TRACE_PROCESSOR = args.trace_processor.resolve() if args.trace_processor else None
    unittest.main(argv=[sys.argv[0]] + rest)
