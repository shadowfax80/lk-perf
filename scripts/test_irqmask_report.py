#!/usr/bin/env python3
"""K6/K1/K2 regressions: dump format and capture-session handling (host side).

Covers the SAMPLE line's src/lat/msite/mgap extension and its checksum,
older dumps without those fields, MASKINFO/MASKCPU/MASKSITE parsing with
checksum rejection, delayed-sample classification and attribution, and
the perf-script exporter accepting the extended format.
"""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parent))
import shutil
import subprocess

from pi4_pc_histogram import (_fnv, _sample_checksum, check_build, classify_delays, image_hash,
                              parse_mask, parse_samples, parse_session, read_lines,
                              select_dump)
from pi4_perf_export import read_capture

FREQ = 54000000


def sample_line(seq=0, cpu=0, pc=0x8000, lr=0, fp=0x1000, sp=0x1000, spsr=0x33,
                tid=0x80002000, ts=1000000, stack=bytes(128), k6=None, slen=None, crc=None):
    """k6 = (src, lat, msite, mgap) or None for an older-format record; slen (K1)
    is appended only with k6."""
    extra = None if k6 is None else (ord(k6[0]), k6[1], k6[2], k6[3])
    if extra is not None and slen is not None:
        extra += (slen,)
    crc = _sample_checksum(cpu, seq, pc, lr, fp, sp, spsr, tid, ts, stack, extra) \
        if crc is None else crc
    tail = "" if k6 is None else \
        f" src={k6[0]} lat={k6[1]:08x} msite={k6[2]:08x} mgap={k6[3]:08x}"
    if k6 is not None and slen is not None:
        tail += f" slen={slen:08x}"
    return (f"SAMPLE seq={seq:08x} cpu={cpu} pc={pc:08x} lr={lr:08x} fp={fp:08x} "
            f"sp={sp:08x} spsr={spsr:08x} tid={tid:08x} ts={ts:016x} "
            f"stack={stack.hex()}{tail} crc={crc:08x}\n")


def mask_lines(now=10 * FREQ, start=FREQ, masked=FREQ // 10, site=0x80001234):
    info = (f"MASKINFO cntfrq={FREQ:08x} on=1 now={now:016x} "
            f"crc={_fnv([FREQ, 1, now, now >> 32]):08x}\n")
    words = [0, start, start >> 32, masked, masked >> 32, 7, 100, 0, 2, 500, site, 0, 0, 0]
    cpu = (f"MASKCPU cpu=0 start={start:016x} masked={masked:016x} regions=00000007 "
           f"irq={100:016x} irqregions=00000002 max=000001f4 maxsite={site:08x} "
           f"dropped=00000000 droppedticks={0:016x} crc={_fnv(words):08x}\n")
    site_line = (f"MASKSITE cpu=0 site={site:08x} count=00000005 ticks={masked:016x} "
                 f"max=000001f4 crc={_fnv([0, site, 5, masked, masked >> 32, 500]):08x}\n")
    return info + cpu + site_line


def dump_text(run=0x1234, dump=1, samples=((0, 0x8000),), build=0xabcdef01, lo=0x8000,
              hi=0x8010, modes=2, event=0x11, period=1000000, mixed=0, cpus=1,
              totals=None, footer_count=None, header_crc=None):
    """One complete K2 dump: header, per-core lines, samples, footer."""
    words = [2, run, run >> 32, dump, lo, hi, build, modes, event, period, mixed, cpus, 4096]
    hcrc = _fnv(words) if header_crc is None else header_crc
    text = (f"DUMPBEGIN fmt=00000002 run={run:016x} dump={dump:08x} image={lo:08x}-{hi:08x} "
            f"build={build:08x} modes={modes:08x} event={event:08x} period={period:08x} "
            f"mixed={mixed:08x} cpus={cpus:08x} buf=00001000 crc={hcrc:08x}\n")
    per_cpu = {}
    for cpu, _ in samples:
        per_cpu[cpu] = per_cpu.get(cpu, 0) + 1
    for cpu in range(cpus):
        kept = per_cpu.get(cpu, 0)
        total = (totals or {}).get(cpu, kept)
        text += (f"DUMPCPU cpu={cpu} total={total:08x} retained={kept:08x} "
                 f"overwritten={total - kept:08x} pmumissed=00000000 "
                 f"crc={_fnv([cpu, total, kept, total - kept, 0]):08x}\n")
    for seq, (cpu, pc) in enumerate(samples):
        text += sample_line(seq=seq, cpu=cpu, pc=pc, ts=1000000 + seq,
                            k6=("p", 400, 0, 0xffffffff), slen=128)
    count = len(samples) if footer_count is None else footer_count
    text += (f"DUMPEND run={run:016x} dump={dump:08x} samples={count:08x} "
             f"crc={_fnv([run, run >> 32, dump, count]):08x}\nSAMPLE done\n")
    return text


class K6Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.log = Path(self.temp.name) / "capture.log"

    def write(self, text):
        self.log.write_text(text, encoding="utf-8")
        return str(self.log)

    def test_extended_sample_round_trips(self):
        s = parse_samples(self.write(sample_line(k6=("p", 4000, 0x80001234, 30))))
        self.assertEqual(len(s), 1)
        self.assertEqual((s[0]["src"], s[0]["lat"], s[0]["msite"], s[0]["mgap"]),
                         ("p", 4000, 0x80001234, 30))

    def test_extended_field_corruption_is_rejected(self):
        good = sample_line(k6=("t", 3, 0, 0xffffffff))
        bad = good.replace("lat=00000003", "lat=00000004")
        self.assertEqual(parse_samples(self.write(bad)), [])

    def test_slen_bounds_the_stack_window(self):
        stack = bytes(range(16)) + bytes(112)
        s = parse_samples(self.write(sample_line(stack=stack, k6=("p", 400, 0, 0xffffffff),
                                                 slen=16)))
        self.assertEqual(s[0]["slen"], 16)
        from pi4_pc_histogram import make_read_memory
        window = s[0]["stack"][:s[0]["slen"]]
        read = make_read_memory(window, 0x1000)
        self.assertEqual(read(0x1000 + 12, 4), int.from_bytes(bytes(range(12, 16)), "little"))
        self.assertEqual(read(0x1000 + 16, 4), 0)   # past the copied bytes: clean stop

    def test_slen_corruption_and_overrange_are_rejected(self):
        good = sample_line(k6=("p", 400, 0, 0xffffffff), slen=16)
        self.assertEqual(parse_samples(self.write(good.replace("slen=00000010",
                                                                "slen=00000011"))), [])
        self.assertEqual(parse_samples(self.write(sample_line(k6=("p", 400, 0, 0xffffffff),
                                                              slen=200))), [])

    def test_k6_record_without_slen_still_parses(self):
        s = parse_samples(self.write(sample_line(k6=("t", 3, 0, 0xffffffff))))
        self.assertIsNone(s[0]["slen"])

    def test_exporter_accepts_slen_and_rejects_overrange(self):
        ok = sample_line(k6=("p", 400, 0, 0xffffffff), slen=64) + "SAMPLE done\n"
        samples, _ = read_capture(Path(self.write(ok)))
        self.assertEqual(samples[0]["slen"], 64)
        bad = sample_line(k6=("p", 400, 0, 0xffffffff), slen=200) + "SAMPLE done\n"
        with self.assertRaises(ValueError):
            read_capture(Path(self.write(bad)))

    def test_latest_dump_is_selected_and_earlier_ones_ignored(self):
        log = self.write(dump_text(run=0x1, samples=((0, 0x1000),) * 3)
                         + "] profiler dump\n"
                         + dump_text(run=0x2, samples=((0, 0x2000),) * 2))
        self.assertEqual([s["pc"] for s in parse_samples(log)], [0x2000, 0x2000])
        self.assertEqual([s["pc"] for s in parse_samples(log, 0)], [0x1000] * 3)
        _, where = select_dump(read_lines(log))
        self.assertEqual((where["dumps"], where["index"]), (2, 1))
        with self.assertRaises(ValueError):
            select_dump(read_lines(log), 5)

    def test_session_header_cpu_lines_and_footer(self):
        lines, _ = select_dump(read_lines(self.write(dump_text(totals={0: 5000},
                                                                samples=((0, 0x1000),) * 4))))
        s = parse_session(lines)
        self.assertEqual(s["problems"], [])
        self.assertEqual(s["header"]["period"], 1000000)
        self.assertEqual(s["cpus"][0]["overwritten"], 4996)
        self.assertEqual(s["footer"]["samples"], 4)

    def test_damaged_header_and_missing_footer_are_reported(self):
        lines, _ = select_dump(read_lines(self.write(dump_text(header_crc=0))))
        self.assertIn("dump header missing or damaged", parse_session(lines)["problems"])
        cut = dump_text().split("DUMPEND")[0]
        lines, where = select_dump(read_lines(self.write(cut)))
        self.assertFalse(where["complete"])
        self.assertTrue(any("footer" in p for p in parse_session(lines)["problems"]))

    def test_footer_gives_exact_transfer_loss(self):
        text = dump_text(samples=((0, 0x1000),) * 3)
        damaged = text.replace("seq=00000001", "seq=0000000x")   # one record unreadable
        samples, quality = read_capture(Path(self.write(damaged)))
        self.assertEqual((quality["expected_samples"], quality["lost_or_rejected_samples"]),
                         (3, 1))
        self.assertTrue(quality["trailing_loss_count_known"])

    def test_exporter_refuses_inconsistent_session(self):
        text = dump_text(samples=((0, 0x1000),) * 2, footer_count=5)
        with self.assertRaisesRegex(ValueError, "do not add up"):
            read_capture(Path(self.write(text)))

    @unittest.skipUnless(shutil.which("arm-none-eabi-as") and shutil.which("arm-none-eabi-ld"),
                         "needs arm-none-eabi binutils")
    def test_image_hash_matches_elf_bytes_and_detects_mismatch(self):
        tmp = Path(self.temp.name)
        payload = bytes(range(1, 33))
        (tmp / "img.s").write_text(".text\n.global _start\n_start:\n.byte "
                                   + ",".join(str(b) for b in payload) + "\n")
        subprocess.run(["arm-none-eabi-as", "-o", str(tmp / "img.o"), str(tmp / "img.s")],
                       check=True)
        subprocess.run(["arm-none-eabi-ld", "-Ttext=0x8000", "-o", str(tmp / "img.elf"),
                        str(tmp / "img.o")], check=True)
        h = 0x811c9dc5
        for b in payload:
            h = ((h ^ b) * 16777619) & 0xffffffff
        self.assertEqual(image_hash(tmp / "img.elf", 0x8000, 0x8020), h)
        self.assertIsNone(image_hash(tmp / "img.elf", 0x8000, 0x9000))   # not all in file
        lines, _ = select_dump(read_lines(self.write(dump_text(build=h, lo=0x8000,
                                                                hi=0x8020))))
        self.assertEqual(check_build(tmp / "img.elf", parse_session(lines)), (True, h))
        lines, _ = select_dump(read_lines(self.write(dump_text(build=h ^ 1, lo=0x8000,
                                                                hi=0x8020))))
        self.assertEqual(check_build(tmp / "img.elf", parse_session(lines))[0], False)

    def test_older_record_still_parses_without_k6_fields(self):
        s = parse_samples(self.write(sample_line()))
        self.assertEqual(len(s), 1)
        self.assertIsNone(s[0]["src"])

    def test_mask_lines_parse_and_corruption_is_counted(self):
        text = mask_lines()
        mask = parse_mask(self.write(text))
        self.assertEqual(mask["info"]["cntfrq"], FREQ)
        self.assertEqual(mask["cpus"][0]["masked"], FREQ // 10)
        self.assertEqual(mask["sites"][0][0]["count"], 5)
        bad = text.replace("count=00000005", "count=00000006")
        mask = parse_mask(self.write(bad))
        self.assertEqual(mask["rejected"], 1)
        self.assertNotIn(0, mask["sites"])

    def test_pseudo_nmi_cores_are_reported(self):
        line = f"MASKNMI cpus=0000000f crc={_fnv([0xf]):08x}\n"
        self.assertEqual(parse_mask(self.write(mask_lines() + line))["nmi"], 0xf)
        bad = line.replace("cpus=0000000f", "cpus=0000000e")
        mask = parse_mask(self.write(mask_lines() + bad))
        self.assertIsNone(mask["nmi"])
        self.assertEqual(mask["rejected"], 1)

    def test_no_mask_lines_means_no_accounting(self):
        self.assertIsNone(parse_mask(self.write(sample_line())))

    def test_delay_classification_and_attribution(self):
        samples = [{"src": "p", "lat": 300, "msite": 0x80001000, "mgap": 5} for _ in range(9)]
        samples.append({"src": "p", "lat": 90000, "msite": 0x80002000, "mgap": 20})
        samples.append({"src": "p", "lat": 90000, "msite": 0x80002000, "mgap": 10 ** 6})
        samples.append({"src": "t", "lat": 30, "msite": 0, "mgap": 0xffffffff})
        result = classify_delays(samples, FREQ)
        self.assertEqual(result["thresholds"]["p"], 1200)    # 4 x 5th percentile 300
        self.assertFalse(samples[0]["delayed"])
        self.assertEqual(samples[9]["mask_site"], 0x80002000)  # ended just before
        self.assertTrue(samples[10]["delayed"])
        self.assertIsNone(samples[10]["mask_site"])            # region ended too long ago
        self.assertFalse(samples[11]["delayed"])               # timer floor: 1 us = 54 ticks

    def test_mostly_delayed_capture_still_finds_the_entry_latency(self):
        # 3 prompt samples, 17 delayed: a median threshold would flag none.
        samples = [{"src": "p", "lat": 400, "msite": 0x80001000, "mgap": 3}
                   for _ in range(3)]
        samples += [{"src": "p", "lat": 200000, "msite": 0x80002000, "mgap": 3}
                    for _ in range(17)]
        classify_delays(samples, FREQ)
        self.assertEqual(sum(s["delayed"] for s in samples), 17)

    def test_exporter_accepts_extended_records(self):
        text = (sample_line(seq=0, k6=("p", 400, 0x80001000, 3))
                + sample_line(seq=1, ts=1000100, k6=("p", 90000, 0x80002000, 4))
                + mask_lines() + "SAMPLE done\n")
        samples, quality = read_capture(Path(self.write(text)))
        self.assertEqual(len(samples), 2)
        self.assertEqual(quality["checksum_rejections"], 0)
        self.assertEqual(quality["malformed_records"], 0)

    def test_exporter_rejects_short_extended_field(self):
        line = sample_line(k6=("p", 400, 0x80001000, 3)).replace("mgap=00000003", "mgap=3")
        with self.assertRaises(ValueError):          # no valid sample remains
            read_capture(Path(self.write(line + "SAMPLE done\n")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
