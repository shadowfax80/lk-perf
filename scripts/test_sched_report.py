#!/usr/bin/env python3
"""Offline regressions for scripts/pi4_sched_report.py (K8): a synthetic,
checksummed `profiler schedump` against a small assembled ELF."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pi4_pc_histogram import _fnv, image_hash  # noqa: E402
from pi4_sched_report import (BLOCKED, DEATH, FREQ, NAME, READY, SLEEPING, SWITCH, WAKE,  # noqa: E402
                              DataSymbols, analyse, parse_sched, split_sched_dumps,
                              wait_queue_name, write_systrace)

HZ = 1_000_000          # 1 tick = 1 us, to keep the arithmetic readable
IDLE = 0x10             # THREAD_FLAG_IDLE

FIXTURE = """
    .text
    .global _start
_start:
    bx lr
    .data
    .global my_event
    .type my_event, %object
    .size my_event, 16
my_event:
    .space 16
"""


def build_elf(tmp: Path) -> tuple[Path, dict]:
    (tmp / "f.S").write_text(FIXTURE)
    subprocess.run(["arm-none-eabi-as", "f.S", "-o", "f.o"], check=True, cwd=tmp)
    subprocess.run(["arm-none-eabi-ld", "-Ttext=0x8000", "-Tdata=0x9000", "f.o", "-o", "f.elf"],
                   check=True, cwd=tmp)
    from elftools.elf.elffile import ELFFile
    with open(tmp / "f.elf", "rb") as f:
        sym = {s.name: s["st_value"] for s in ELFFile(f).get_section_by_name(".symtab")
               .iter_symbols() if s.name}
    return tmp / "f.elf", sym


def name_words(name: str) -> list[int]:
    return [ord(c) for c in name]


def dump(events, threads, lo=0x8000, hi=0x8004, build=0, start=1000, stop=11000, cpus=2,
         corrupt_seq=None):
    """Lines of one schedump. events: (cpu, ts, type, state, a, b, c, err[, name])."""
    run = 0x1234
    w = [1, run, run >> 32, HZ, cpus, 8192, lo, hi, build, start, start >> 32, stop, stop >> 32]
    out = [f"SCHEDBEGIN fmt=00000001 run={run:016x} cntfrq={HZ:08x} cpus={cpus:08x} "
           f"buf=00002000 image={lo:08x}-{hi:08x} build={build:08x} start={start:016x} "
           f"stop={stop:016x} crc={_fnv(w):08x}"]
    per = {c: [e for e in events if e[0] == c] for c in range(cpus)}
    for c in range(cpus):
        n = len(per[c])
        out.append(f"SCHEDCPU cpu={c} total={n:08x} retained={n:08x} crc={_fnv([c, n, n]):08x}")
    for tid, (name, prio, flags) in threads.items():
        out.append(f"THREAD tid={tid:08x} prio={prio:08x} flags={flags:08x} name={name} "
                   f"crc={_fnv([tid, prio, flags] + name_words(name)):08x}")
    seq = 0
    for c in range(cpus):
        for e in per[c]:
            _, ts, typ, st, a, b, cc, err = e[:8]
            name = e[8] if len(e) > 8 else ""
            ev = typ | st << 8
            words = [seq, c, ts, ts >> 32, ev, a, b, cc, err & 0xffffffff] + name_words(name)
            crc = _fnv(words) ^ (1 if seq == corrupt_seq else 0)
            nm = f"name={name} " if name else ""
            out.append(f"SCHED seq={seq:08x} cpu={c} ts={ts:016x} ev={ev:08x} a={a:08x} b={b:08x} "
                       f"c={cc:08x} err={err & 0xffffffff:08x} {nm}crc={crc:08x}")
            seq += 1
    out.append(f"SCHEDEND run={run:016x} events={seq:08x} "
               f"crc={_fnv([run, run >> 32, seq]):08x}")
    return [line + "\n" for line in out]


IDLE0, IDLE1, A, B = 0x80100000, 0x80100200, 0x80001000, 0x80002000


class SchedReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.elf, cls.sym = build_elf(Path(cls.tmp.name))
        cls.build = image_hash(str(cls.elf), 0x8000, 0x8004)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scenario(self, **kw):
        ev_q = self.sym["my_event"] + 4
        threads = {IDLE0: ("idle_0", 0, IDLE), IDLE1: ("idle_1", 0, IDLE)}
        events = [
            (0, 1100, NAME, 0, A, 16, 0, 0, "worker_a"),
            # cpu0: idle -> A at 2000; A blocks on my_event at 5000
            (0, 2000, SWITCH, READY, IDLE0, A, 0, 0),
            (0, 5000, SWITCH, BLOCKED, A, IDLE0, ev_q, 0),
            # cpu1: B wakes A at 7000; A runs on cpu1 at 7003 and exits at 8000
            (1, 1200, NAME, 0, B, 16, 0, 0, "worker_b"),
            (1, 1500, SWITCH, READY, IDLE1, B, 0, 0),
            (1, 7000, WAKE, BLOCKED, A, B, ev_q, 0),
            (1, 7001, SWITCH, SLEEPING, B, IDLE1, 0, 0),
            (1, 7003, SWITCH, READY, IDLE1, A, 0, 0),
            (1, 8000, SWITCH, DEATH, A, IDLE1, 0, 0),
            # the same thread_t address reused by a new thread at 9000
            (0, 8900, NAME, 0, A, 16, 0, 0, "worker_c"),
            (0, 9000, SWITCH, READY, IDLE0, A, 0, 0),
            (0, 9500, FREQ, 0, 600_000_000, 600_000_000, 0, 0),
        ]
        return dump(events, threads, build=self.build, **kw)

    def parsed(self, **kw):
        return parse_sched(split_sched_dumps(self.scenario(**kw))[-1])

    def test_times_reasons_and_latency(self):
        d = self.parsed()
        self.assertEqual(sum(d["rejected"].values()), 0)
        a = analyse(d)
        names = {a["threads"][k]["name"]: v for k, v in a["oncpu"].items()}
        self.assertEqual(names["worker_a"], (5000 - 2000) + (8000 - 7003))
        self.assertEqual(names["worker_b"], 7001 - 1500)
        self.assertEqual(names["worker_c"], 11000 - 9000)
        self.assertEqual(a["latency"], [3])
        (key, (n, total, mx)), = [(k, v) for k, v in a["off_time"].items() if k[1] == BLOCKED]
        self.assertEqual((n, total, mx), (1, 2000, 2000))
        syms = DataSymbols(str(self.elf))
        self.assertEqual(wait_queue_name(key[2], a["threads"], syms), "my_event+0x4")
        self.assertEqual(len(a["freq"]), 1)

    def test_address_reuse_starts_a_new_thread(self):
        a = analyse(self.parsed())
        idents = {k: v["name"] for k, v in a["threads"].items() if k[0] == A}
        self.assertEqual(idents, {(A, 0): "worker_a", (A, 1): "worker_c"})

    def test_corrupt_event_is_rejected_and_reported_lost(self):
        d = self.parsed(corrupt_seq=3)
        self.assertEqual(d["rejected"]["event"], 1)
        self.assertEqual(d["footer"]["events"] - len(d["events"]), 1)

    def test_wrong_elf_is_detected(self):
        lines = dump([], {}, build=self.build ^ 1)
        d = parse_sched(lines)
        self.assertNotEqual(image_hash(str(self.elf), d["header"]["lo"], d["header"]["hi"]),
                            d["header"]["build"])

    def test_systrace_uses_sched_waking_and_idle_pid_0(self):
        d = self.parsed()
        a = analyse(d)
        out = Path(self.tmp.name) / "t.systrace"
        write_systrace(str(out), d, a)
        text = out.read_text()
        self.assertIn("sched_waking: comm=worker_a pid=2", text)
        self.assertIn("worker_b-1 [001] d..4 0.007000: sched_waking", text)              # the waker is the task
        self.assertIn("target_cpu=001", text)                # where it ran next
        self.assertIn("prev_comm=idle_0 prev_pid=0", text)
        self.assertIn("prev_state=X ==> next_comm=idle_1", text)
        self.assertIn("next_comm=worker_c next_pid=3", text)   # a new pid after reuse
        self.assertIn("cpu_frequency: state=600000 cpu_id=1", text)
        self.assertNotIn("sched_wakeup", text)


if __name__ == "__main__":
    unittest.main()
