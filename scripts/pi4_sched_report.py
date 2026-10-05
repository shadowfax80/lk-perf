#!/usr/bin/env python3
"""Scheduling report and Perfetto export for `profiler schedump` (K8).

Reads the checksummed SCHED* / THREAD lines from a serial log, verifies the
ELF against the dump's image hash, and reports, per thread, CPU time, why
the thread left the CPU, how long it stayed blocked or asleep (by wait
queue, symbolized), wakeup-to-run latency, per-core busy time and the ARM
clock history. `--systrace FILE` writes the same events as systrace text
(sched_switch, sched_waking, cpu_frequency), which Perfetto
(ui.perfetto.dev, trace_processor) opens as a scheduling timeline.

    python3 scripts/pi4_sched_report.py capture.log build/lk/build-rpi4-test/lk.elf \\
        --systrace capture.systrace
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter, defaultdict

from pi4_pc_histogram import _fnv, image_hash, read_lines

H8 = r"[0-9a-fA-F]{8}"
H16 = r"[0-9a-fA-F]{16}"
BEGIN_RE = re.compile(
    rf"SCHEDBEGIN fmt=(?P<fmt>{H8}) run=(?P<run>{H16}) cntfrq=(?P<freq>{H8}) cpus=(?P<cpus>{H8}) "
    rf"buf=(?P<buf>{H8}) image=(?P<lo>{H8})-(?P<hi>{H8}) build=(?P<build>{H8}) "
    rf"start=(?P<start>{H16}) stop=(?P<stop>{H16}) crc=(?P<crc>{H8})\s*$")
CPU_RE = re.compile(rf"SCHEDCPU cpu=(?P<cpu>\d+) total=(?P<total>{H8}) retained=(?P<kept>{H8}) "
                    rf"crc=(?P<crc>{H8})\s*$")
THREAD_RE = re.compile(rf"THREAD tid=(?P<tid>{H8}) prio=(?P<prio>{H8}) flags=(?P<flags>{H8}) "
                       rf"name=(?P<name>\S+) crc=(?P<crc>{H8})\s*$")
EV_RE = re.compile(rf"SCHED seq=(?P<seq>{H8}) cpu=(?P<cpu>\d+) ts=(?P<ts>{H16}) ev=(?P<ev>{H8}) "
                   rf"a=(?P<a>{H8}) b=(?P<b>{H8}) c=(?P<c>{H8}) err=(?P<err>{H8}) "
                   rf"(?:name=(?P<name>\S+) )?crc=(?P<crc>{H8})\s*$")
END_RE = re.compile(rf"SCHEDEND run=(?P<run>{H16}) events=(?P<events>{H8}) crc=(?P<crc>{H8})\s*$")

SWITCH, WAKE, FREQ, NAME = 1, 2, 3, 4
SUSPENDED, READY, RUNNING, BLOCKED, SLEEPING, DEATH = range(6)
STATE_NAME = {SUSPENDED: "suspended", READY: "preempted/yielded", BLOCKED: "blocked",
              SLEEPING: "sleeping", DEATH: "exited"}
STATE_LETTER = {SUSPENDED: "T", READY: "R", RUNNING: "R", BLOCKED: "D", SLEEPING: "S",
                DEATH: "X"}
THREAD_FLAG_IDLE = 1 << 4
ERR_TIMED_OUT = -13
THROTTLE_BITS = {0: "under-voltage", 1: "ARM frequency capped", 2: "throttled",
                 3: "soft temperature limit"}


def split_sched_dumps(lines: list[str]) -> list[list[str]]:
    starts = [i for i, line in enumerate(lines) if "SCHEDBEGIN" in line]
    dumps = []
    for n, i in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else len(lines)
        block = lines[i:end]
        for j, line in enumerate(block):
            if "SCHEDEND" in line:
                block = block[:j + 1]
                break
        dumps.append(block)
    return dumps


def parse_sched(lines: list[str]) -> dict:
    """One dump's header, per-core counts, thread names and events, each
    line checksum-verified; damaged lines are counted, not used."""
    d = {"header": None, "cpus": {}, "threads": {}, "events": [], "footer": None,
         "rejected": Counter()}
    for raw in lines:
        if "SCHED" not in raw and "THREAD" not in raw:
            continue
        line = raw.strip()
        if m := EV_RE.search(line):
            v = {k: int(m[k], 16) for k in ("seq", "ts", "ev", "a", "b", "c", "err")}
            cpu = int(m["cpu"])
            words = [v["seq"], cpu, v["ts"], v["ts"] >> 32, v["ev"], v["a"], v["b"], v["c"],
                     v["err"]] + [ord(ch) for ch in (m["name"] or "")]
            if _fnv(words) != int(m["crc"], 16):
                d["rejected"]["event"] += 1
                continue
            err = v["err"] - (1 << 32) if v["err"] & 0x80000000 else v["err"]
            d["events"].append({"seq": v["seq"], "cpu": cpu, "ts": v["ts"],
                                "type": v["ev"] & 0xff, "state": (v["ev"] >> 8) & 0xff,
                                "a": v["a"], "b": v["b"], "c": v["c"], "err": err,
                                "name": m["name"]})
        elif m := THREAD_RE.search(line):
            tid, prio, flags = (int(m[k], 16) for k in ("tid", "prio", "flags"))
            name = "" if m["name"] == "-" else m["name"]
            if _fnv([tid, prio, flags] + [ord(ch) for ch in name]) != int(m["crc"], 16):
                d["rejected"]["thread"] += 1
                continue
            prio = prio - (1 << 32) if prio & 0x80000000 else prio
            d["threads"].setdefault(tid, {"name": name or f"thread-{tid:08x}", "prio": prio,
                                          "flags": flags})
        elif m := BEGIN_RE.search(line):
            v = {k: int(m[k], 16) for k in ("fmt", "run", "freq", "cpus", "buf", "lo", "hi",
                                             "build", "start", "stop")}
            words = [v["fmt"], v["run"], v["run"] >> 32, v["freq"], v["cpus"], v["buf"], v["lo"],
                     v["hi"], v["build"], v["start"], v["start"] >> 32, v["stop"], v["stop"] >> 32]
            if _fnv(words) != int(m["crc"], 16):
                d["rejected"]["header"] += 1
                continue
            d["header"] = v
        elif m := CPU_RE.search(line):
            cpu, total, kept = int(m["cpu"]), int(m["total"], 16), int(m["kept"], 16)
            if _fnv([cpu, total, kept]) != int(m["crc"], 16):
                d["rejected"]["cpu"] += 1
                continue
            d["cpus"][cpu] = {"total": total, "retained": kept}
        elif m := END_RE.search(line):
            run, n = int(m["run"], 16), int(m["events"], 16)
            if _fnv([run, run >> 32, n]) != int(m["crc"], 16):
                d["rejected"]["footer"] += 1
                continue
            d["footer"] = {"run": run, "events": n}
        elif line.startswith(("SCHED", "THREAD")):
            d["rejected"]["format"] += 1
    d["events"].sort(key=lambda e: (e["ts"], e["cpu"], e["seq"]))
    return d


class DataSymbols:
    """Address -> "symbol+0xoff" over every sized symbol (wait queues live
    inside data objects such as an event_t or mutex_t)."""

    def __init__(self, elf_path: str):
        from elftools.elf.elffile import ELFFile
        self.syms = []
        with open(elf_path, "rb") as f:
            tab = ELFFile(f).get_section_by_name(".symtab")
            for s in tab.iter_symbols() if tab else []:
                if s.name and s["st_size"] and s["st_info"]["type"] in ("STT_OBJECT", "STT_FUNC"):
                    self.syms.append((s["st_value"] & ~1, s["st_size"], s.name))
        self.syms.sort()

    def name(self, addr: int) -> str:
        best = None
        for start, size, name in self.syms:
            if start > addr:
                break
            if addr < start + size:
                best = (start, name)
        if not best:
            return f"0x{addr:08x}"
        return best[1] if addr == best[0] else f"{best[1]}+0x{addr - best[0]:x}"


def assign_identities(d: dict) -> dict:
    """A thread_t address is reused after its thread exits. Give every event
    thread identities (tid, generation): the generation of a tid advances
    after a switch-out in state DEATH. Names come from NAME events in the
    stream (the THREAD table, which only knows the last occupant of each
    address, is the fallback). Returns {identity: {name, prio, flags}}."""
    gen = Counter()
    info: dict[tuple[int, int], dict] = {}
    live: dict[int, tuple[int, int]] = {}   # tid -> its current identity

    def ident(tid: int) -> tuple[int, int]:
        key = (tid, gen[tid])
        if key not in info:
            t = d["threads"].get(tid)
            info[key] = dict(t) if t and gen[tid] == 0 else {
                "name": f"thread-{tid:08x}" + (f"#{gen[tid]}" if gen[tid] else ""),
                "prio": 0, "flags": 0}
            info[key]["named"] = False
        live[tid] = key
        return key

    def owner(wq: int):
        """The identity, at this point of the trace, whose thread_t holds wq."""
        for tid, key in live.items():
            if tid <= wq < tid + THREAD_T_SIZE:
                return key
        return None

    for e in d["events"]:
        if e["type"] == NAME:
            k = ident(e["a"])
            info[k].update(name=e["name"] or info[k]["name"], prio=e["b"], flags=e["c"],
                           named=True)
            e["ia"] = k
        elif e["type"] == SWITCH:
            e["ia"], e["ib"] = ident(e["a"]), ident(e["b"])
            e["wq_owner"] = owner(e["c"]) if e["c"] else None
            if e["state"] == DEATH:
                gen[e["a"]] += 1
        elif e["type"] == WAKE:
            e["ia"], e["ib"] = ident(e["a"]), ident(e["b"])
            e["wq_owner"] = owner(e["c"]) if e["c"] else None
    return info


def analyse(d: dict) -> dict:
    h = d["header"]
    start, stop = h["start"], h["stop"]
    threads = assign_identities(d)
    cur: dict[int, tuple[int, int]] = {}
    oncpu = Counter()
    oncpu_cpu = Counter()
    out_reason = defaultdict(Counter)
    switches_in = Counter()
    pending_off: dict[int, tuple[int, int, int]] = {}
    off_time = defaultdict(lambda: [0, 0, 0])   # (state, wq, timed out) -> count, total, max
    pending_wake: dict[int, int] = {}
    wakeups = Counter()
    latency = []
    lat_by = defaultdict(list)
    freq = []
    inconsistent = 0
    for e in d["events"]:
        ts, cpu = e["ts"], e["cpu"]
        if e["type"] == SWITCH:
            old, new = e["ia"], e["ib"]
            if cpu in cur:
                tid, since = cur[cpu]
                if tid != old:
                    inconsistent += 1
                    since = ts
            else:
                since = start
            oncpu[old] += ts - since
            oncpu_cpu[(cpu, old)] += ts - since
            out_reason[old][e["state"]] += 1
            if e["state"] in (BLOCKED, SLEEPING, SUSPENDED):
                pending_off[old] = (ts, e["state"], (e["c"], e["wq_owner"]))
            cur[cpu] = (new, ts)
            switches_in[new] += 1
            if new in pending_wake:
                lat = ts - pending_wake.pop(new)
                latency.append(lat)
                lat_by[new].append(lat)
        elif e["type"] == WAKE:
            tid = e["ia"]
            wakeups[(tid, e["state"], e["c"], e["err"] == ERR_TIMED_OUT)] += 1
            if tid in pending_off:
                t0, st, wq = pending_off.pop(tid)
                rec = off_time[(tid, st, (e["c"], e["wq_owner"]) if e["c"] else wq,
                                e["err"] == ERR_TIMED_OUT)]
                rec[0] += 1
                rec[1] += ts - t0
                rec[2] = max(rec[2], ts - t0)
            pending_wake[tid] = ts
        elif e["type"] == FREQ:
            freq.append((ts, cpu, e["a"], e["b"], e["c"]))
    for cpu, (tid, since) in cur.items():
        oncpu[tid] += stop - since
        oncpu_cpu[(cpu, tid)] += stop - since
    idle = {k for k, v in threads.items() if v["flags"] & THREAD_FLAG_IDLE}
    return {"threads": threads, "lat_by": lat_by,
            "oncpu": oncpu, "oncpu_cpu": oncpu_cpu, "out_reason": out_reason,
            "switches_in": switches_in, "off_time": off_time, "wakeups": wakeups,
            "latency": sorted(latency), "freq": freq, "idle": idle,
            "inconsistent": inconsistent, "window": stop - start}


def tname(threads: dict, ident) -> str:
    if ident in threads:
        return threads[ident]["name"]
    tid = ident[0] if isinstance(ident, tuple) else ident
    return f"thread-{tid:08x}"


THREAD_T_SIZE = 0x200   # generous bound on sizeof(thread_t)


def wait_queue_name(wq: tuple, threads: dict, syms: DataSymbols) -> str:
    """A static object's symbol, or "<thread>'s thread_t+off" for wait queues
    inside a thread (thread_join waits on the joined thread's own queue),
    named after the thread that occupied that thread_t at the time."""
    addr, owner = wq
    if owner is not None:
        return f"{threads[owner]['name']}'s thread_t+0x{addr - owner[0]:x}"
    return syms.name(addr)


def pct(sorted_values: list[int], p: float) -> int:
    return sorted_values[min(len(sorted_values) - 1, int(p * len(sorted_values)))]


def report(d: dict, a: dict, syms: DataSymbols, built: bool) -> None:
    h, threads = d["header"], a["threads"]
    hz = h["freq"]
    us = lambda ticks: ticks * 1_000_000 / hz  # noqa: E731
    sent = d["footer"]["events"] if d["footer"] else None
    got = len(d["events"])
    print(f"sched dump: run {h['run']:016x}, {us(a['window']) / 1000:.3f} ms window, "
          f"{h['cpus']} cores; ELF {'matches' if built else 'DOES NOT MATCH'} the image "
          f"({h['build']:08x})")
    for cpu, c in sorted(d["cpus"].items()):
        lost = c["total"] - c["retained"]
        print(f"  cpu{cpu}: {c['total']} events{f', {lost} overwritten (oldest)' if lost else ''}")
    if sent is not None:
        print(f"events: {sent} sent, {got} accepted"
              f"{f', {sent - got} lost or rejected in transfer' if sent != got else ', none lost'}")
    else:
        print("warning: dump footer missing or damaged: trailing loss unknown", file=sys.stderr)
    if d["rejected"]:
        print(f"warning: rejected lines: {dict(d['rejected'])}", file=sys.stderr)
    if a["inconsistent"]:
        print(f"warning: {a['inconsistent']} switch(es) did not continue the previous one on "
              f"their core (lost events)", file=sys.stderr)
    print()

    print("per thread (CPU time over the window; wakeup-to-running mean; why it left the CPU):")
    print(f"  {'thread':<18} {'cpu ms':>9} {'%':>6} {'in':>6} {'wake us':>8}  left the CPU")
    for tid, t in sorted(a["oncpu"].items(), key=lambda kv: -kv[1]):
        reasons = ", ".join(f"{STATE_NAME.get(s, s)} {n}"
                            for s, n in sorted(a["out_reason"][tid].items()))
        lats = a["lat_by"].get(tid)
        wake = f"{us(sum(lats) / len(lats)):8.1f}" if lats else f"{'-':>8}"
        print(f"  {tname(threads, tid):<18} {us(t) / 1000:9.3f} {100 * t / a['window']:5.1f}% "
              f"{a['switches_in'][tid]:6} {wake}  {reasons}")
    print()

    print("time off the CPU, by thread and reason (switch-out to wakeup):")
    print(f"  {'thread':<18} {'reason':<40} {'count':>6} {'total ms':>9} {'mean us':>9} "
          f"{'max us':>9}")
    for (tid, st, wq, timeout), (n, total, mx) in sorted(a["off_time"].items(),
                                                          key=lambda kv: -kv[1][1]):
        what = STATE_NAME.get(st, str(st))
        if wq[0]:
            what += f" on {wait_queue_name(wq, threads, syms)}"
        if timeout:
            what += " (timed out)"
        print(f"  {tname(threads, tid):<18} {what:<40} {n:6} {us(total) / 1000:9.3f} "
              f"{us(total) / n:9.1f} {us(mx):9.1f}")
    print()

    lat = a["latency"]
    if lat:
        print(f"wakeup -> running: {len(lat)} wakeups, p50 {us(pct(lat, .5)):.1f} us, "
              f"p90 {us(pct(lat, .9)):.1f} us, p99 {us(pct(lat, .99)):.1f} us, "
              f"max {us(lat[-1]):.1f} us")
    busy = Counter()
    for (cpu, tid), t in a["oncpu_cpu"].items():
        if tid not in a["idle"]:
            busy[cpu] += t
    print("busy (not idle) per core: " + ", ".join(
        f"cpu{c} {100 * busy[c] / a['window']:.1f}%" for c in sorted(d["cpus"])))
    if a["freq"]:
        meas = [f[2] for f in a["freq"]]
        sets = sorted({f[3] for f in a["freq"]})
        flags = 0
        for f in a["freq"]:
            flags |= f[4]
        decoded = ", ".join(n for b, n in THROTTLE_BITS.items() if flags & (1 << b)) or "none"
        print(f"ARM clock: {len(meas)} samples, measured {min(meas) / 1e6:.3f}-"
              f"{max(meas) / 1e6:.3f} MHz, set {', '.join(f'{s / 1e6:.0f}' for s in sets)} MHz; "
              f"throttling now: {decoded}; since boot: "
              + (", ".join(n for b, n in THROTTLE_BITS.items() if flags & (1 << (b + 16)))
                 or "none"))


def write_systrace(path: str, d: dict, a: dict) -> int:
    """systrace text: sched_switch, sched_waking and cpu_frequency."""
    h, threads = d["header"], a["threads"]
    hz = h["freq"]
    pids: dict = {}

    def pid(tid) -> int:
        if tid in a["idle"]:
            return 0
        if tid not in pids:
            pids[tid] = len(pids) + 1
        return pids[tid]

    def comm(tid) -> str:
        return tname(threads, tid).replace(" ", "_")

    def prio(tid) -> int:
        return 120 - threads[tid]["prio"] if tid in threads else 120

    running = {}
    out = ["# tracer: nop", "#",
           "#           TASK-PID     CPU#  ||||    TIMESTAMP  FUNCTION",
           "#              | |         |   ||||       |         |"]
    # next CPU each woken thread runs on (target_cpu)
    target = {}
    for i, e in enumerate(d["events"]):
        if e["type"] == WAKE:
            for f in d["events"][i + 1:]:
                if f["type"] == SWITCH and f["ib"] == e["ia"]:
                    target[i] = f["cpu"]
                    break
    n = 0
    for i, e in enumerate(d["events"]):
        ts = e["ts"] / hz
        cpu = e["cpu"]
        cur = running.get(cpu)
        task = f"{comm(cur)}-{pid(cur)}" if cur is not None else "<idle>-0"
        if e["type"] == SWITCH:
            old, new = e["ia"], e["ib"]
            out.append(f"{f'{comm(old)}-{pid(old)}':>22} [{cpu:03d}] d..3 {ts:.6f}: sched_switch: "
                       f"prev_comm={comm(old)} prev_pid={pid(old)} prev_prio={prio(old)} "
                       f"prev_state={STATE_LETTER.get(e['state'], 'R')} ==> next_comm={comm(new)} "
                       f"next_pid={pid(new)} next_prio={prio(new)}")
            running[cpu] = new
        elif e["type"] == WAKE:
            # sched_waking, not sched_wakeup: Perfetto's systrace importer
            # turns sched_waking into a runnable state linked to its waker
            # (verified with trace_processor) and ignores sched_wakeup.
            t, w = e["ia"], e["ib"]
            out.append(f"{f'{comm(w)}-{pid(w)}':>22} [{cpu:03d}] d..4 {ts:.6f}: sched_waking: "
                       f"comm={comm(t)} pid={pid(t)} prio={prio(t)} "
                       f"target_cpu={target.get(i, cpu):03d}")
        elif e["type"] == NAME:
            continue
        elif e["type"] == FREQ:
            for c in sorted(d["cpus"]):   # one clock for the whole cluster on the Pi
                out.append(f"{task:>22} [{cpu:03d}] .... {ts:.6f}: cpu_frequency: "
                           f"state={e['a'] // 1000} cpu_id={c}")
        n += 1
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("elf")
    ap.add_argument("--dump", type=int, default=-1,
                    help="which sched dump in the log (0 = first, -1 = latest)")
    ap.add_argument("--systrace", help="write the events as systrace text for Perfetto")
    ap.add_argument("--allow-elf-mismatch", action="store_true")
    args = ap.parse_args()
    dumps = split_sched_dumps(read_lines(args.log))
    if not dumps:
        print("error: no SCHEDBEGIN in this log", file=sys.stderr)
        return 1
    try:
        lines = dumps[args.dump]
    except IndexError:
        print(f"error: log has {len(dumps)} sched dump(s)", file=sys.stderr)
        return 1
    d = parse_sched(lines)
    if d["header"] is None:
        print("error: sched dump header missing or damaged", file=sys.stderr)
        return 1
    h = d["header"]
    built = image_hash(args.elf, h["lo"], h["hi"]) == h["build"]
    if not built and not args.allow_elf_mismatch:
        print(f"error: ELF does not match the captured image (build {h['build']:08x}); "
              f"--allow-elf-mismatch to report anyway", file=sys.stderr)
        return 1
    a = analyse(d)
    report(d, a, DataSymbols(args.elf), built)
    if args.systrace:
        n = write_systrace(args.systrace, d, a)
        print(f"\nsystrace: {n} events -> {args.systrace} (open in ui.perfetto.dev)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
