# K11: cross-core PC sampling through EDPCSR, feasibility on the Pi (2026-10-05)

Route 2 for the IRQ-masked blind spot: one core reads another core's
external-debug PC sample register (`EDPCSR`) through memory. The sampled
core's IRQ mask does not matter, and interrupt handlers would become
visible (leaf PC only). Probe output: [probe.txt](probe.txt), from the
image archived with [K10](../k10_timer_sampling_20261005/README.md)
(`lk.bin` `866ffee1…`), commands `profiler dbginfo`, `dbgrom` and
`dbgpcsr`.

## What the A72 reports

| Check | Result | Meaning |
|---|---|---|
| `DBGDEVID.PCSample` (CP14) | 3 | EDPCSR, EDCIDSR and EDVIDSR implemented |
| `DBGDEVID1.PCSROffset` | 2 | Sampled PC needs no offset correction |
| `DBGDRAR` | `ff820003` | Valid: ROM table at physical `0xff820000`, inside the device window LK already maps |
| ROM table walk | Cluster table `0xffc00000`; per core: debug `0xff{c,d,e,f}10000`, CTI `+0x10000`, PMU `+0x20000`; affinity matches cores 0-3 | The CPU can reach every core's debug block |
| Accesses | No faults; OS lock cleared from software (`DBGOSLAR`); `EDPRSR` 0x28b (powered up) | Register access works |
| **`DBGAUTHSTATUS` / `EDAUTHSTATUS`** | **`0xaa`** | **Non-invasive and invasive debug disabled in both security states** |
| `EDPCSR` (4 reads × 4 cores) | `ffffffff` every time | "Sampling prohibited", as the architecture specifies when non-invasive debug is not allowed |

## Conclusion

On this Pi, as booted, route 2 is not possible. The mechanism is there and
reachable, but the SoC holds the debug authentication signal NIDEN low.
Software at Non-secure PL1 cannot change it: it is an input to the core,
driven by the SoC or its firmware.

One lever is untested: the Pi firmware's `enable_jtag_gpio=1` option in
`config.txt`. On-chip debuggers can halt Pi 4 cores in that configuration,
which needs DBGEN, and DBGEN also allows non-invasive debug. Trying it means
an SD-card change, so it waits for the user's approval.

On the real target (T4), run `profiler dbginfo` (after porting the window
check in `dbgrom`/`dbgpcsr`). If `DBGAUTHSTATUS` shows Non-secure
non-invasive debug enabled and `DBGDRAR` is valid, route 2 can be built
there: a sampler on one core reading the other cores' `EDPCSR`.
