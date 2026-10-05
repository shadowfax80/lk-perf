#!/usr/bin/env python3
"""K6 regressions: IRQ-masked time accounting in dumps (host side).

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
from pi4_pc_histogram import (_fnv, _sample_checksum, classify_delays, parse_mask,
                              parse_samples)
from pi4_perf_export import read_capture

FREQ = 54000000


def sample_line(seq=0, cpu=0, pc=0x8000, lr=0, fp=0x1000, sp=0x1000, spsr=0x33,
                tid=0x80002000, ts=1000000, stack=bytes(128), k6=None, crc=None):
    """k6 = (src, lat, msite, mgap) or None for an older-format record."""
    extra = None if k6 is None else (ord(k6[0]), k6[1], k6[2], k6[3])
    crc = _sample_checksum(cpu, seq, pc, lr, fp, sp, spsr, tid, ts, stack, extra) \
        if crc is None else crc
    tail = "" if k6 is None else \
        f" src={k6[0]} lat={k6[1]:08x} msite={k6[2]:08x} mgap={k6[3]:08x}"
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
