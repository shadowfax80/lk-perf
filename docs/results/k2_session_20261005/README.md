# K2: self-describing capture sessions on the real Pi (2026-10-05)

Captured by Claude on the Raspberry Pi 4B (A72, AArch32, Non-secure SVC) at
6 Mbaud. Final image `lk.bin` SHA-256
`1528064a84e633629c119e61b4a4b2faa77f04071c66c2d1eeacbcb143a77f7d`, matching
`lk.elf` `445f4e45788ac536503b1b6236c4d73559722c65236c0d1bb391a0bfa4bb70a1`
(both archived). `probe.log` used the feasibility build (`282de5e1…`).

## Feasibility (probe.log)

The build identity is an FNV-1a hash of the image's read-only bytes,
`[_start, __rodata_end)`: code, unwind index, rodata and LK's constant tables.
On the Pi it read `ac6ac911` right after boot, after a four-core workload,
and after PMU sampling, pseudo-NMI and a dump; the host computed the same
value from the ELF. Nothing in that range is written at run time. Hashing
105 KiB takes 0.70 ms, once per dump.

## Dump structure

```text
DUMPBEGIN fmt run dump image=lo-hi build modes event period mixed cpus buf crc
DUMPCPU   cpu total retained overwritten pmumissed crc        (one per core)
SAMPLE    ...                                                 (as before)
MASKINFO / MASKNMI / MASKCPU / MASKSITE ...                   (K6/K12)
DUMPEND   run dump samples crc
SAMPLE done
```

A session starts at `profiler clear` (or boot); its run id is the CNTPCT
value at that moment and every dump of it is numbered.

## Results

| Log | Capture | Finding |
|---|---|---|
| session | Two dumps in one log: PMU period 1000000, then period 100000 plus timer sampling, no clear between | Both dumps share run `1fef55dd`, numbered 1 and 2; dump 2 reports modes timer+pmu and `mixed=1` (PMU config changed mid-session). The report uses dump 2 by default, `--dump 0` selects dump 1. Footers give exact transfer loss: **7 of 8458** records lost (dump 2), **15 of 400** (dump 1) |
| overwrite | PMU period 100000, `profiler smp 300000000`, after `clear` | New run id `1ff2a41f`. Each core took about 6038 samples, kept 4096 and reports about 1942 overwritten; 40 of 16384 records lost in transfer |
| session vs the K1 build's ELF | Wrong ELF | Refused: "the ELF does not contain the whole image range" |

Older-format dumps keep their old behaviour: Codex's archived demo capture
reports as before and its perf-script export is byte-identical to the
archived `demo.perf`.
