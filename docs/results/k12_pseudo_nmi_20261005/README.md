# K12: GIC-priority pseudo-NMI sampling on the real Pi (2026-10-05)

Captured by Claude on the Raspberry Pi 4B (A72, AArch32, Non-secure SVC, the
target's state) at 6 Mbaud. Real hardware data. K12 is route 3 for the
IRQ-masked blind spot: with `profiler nmion`, LK masks thread-context code by
raising the GIC priority mask instead of setting CPSR.I, and the PMU sampling
interrupts get a higher priority, so they still arrive inside masked code.
Design: [ARCHITECTURE.md §11.1](../../ARCHITECTURE.md#111-irq-masked-execution-is-a-systematic-blind-spot).

## Images

| Runs | Image | lk.bin SHA-256 | lk.elf SHA-256 |
|---|---|---|---|
| t1 | first K12 build (readback check too strict) | `e390f8cc…de3` | `28356108…99b` |
| t3–t10 | readback check fixed | `6cecbe6ac73e1b08604243ca84788f5f8f7ec28d2d5d89f6896d1656cf694f85` (`lk-6cecbe6a.bin.gz`) | `1fea14edf0121c2efcee33db10824c70ae6f0beadb98f4e05e01ccf0d857da17` (`lk-6cecbe6a.elf.gz`) |
| t11 | **final:** + `MASKNMI` dump line | `9001ea770f3559cc86d976034c93f53bac0148df12a40ab0ae74fc47e3370efa` (`lk.bin.gz`) | `18084b3a13f335b8b19301c3fe38d011f01db0a5f0cd0f83cfa82c3c0603bc23` (`lk.elf.gz`) |

Both archived images were rebuilt from source and reproduced these hashes.
LK base `88a8efae` plus overlays 0001–0014; the series replays on a fresh
clone of that commit and matches the built tree byte for byte. No FPU/NEON
instructions.

## Results

Ground truth: `profiler masktest 2000 500` keeps IRQs masked exactly half of
the time; PMU cycle sampling, period 1000000.

| Run | Mode | Samples inside the masked code | Delayed samples | Notes |
|---|---|---|---|---|
| t9 | CPSR.I (default) | 0 (582 moved to the unmask point, attributed) | 582 / 1201 | Same as K6: the masked half is only visible through attribution |
| t4 | pseudo-NMI | **604 / 1198 (50.4%)** in `profiler_masked_spin → profiler_spin_ticks` | **0** | Accounting 49.8% masked |
| t11 | pseudo-NMI, final image | **607 / 1201 (50.5%)** | **0** | Report states the cores were in pseudo-NMI mode |
| t10 | pseudo-NMI, `profiler smp 100000000` | — | 0 / 800 | Regression: 100% `profiler_workload_inner`, as without K12 |

Bring-up and stability:

- **t1, t3:** the first `nmion` refused to enable, by design: the setup checks
  that priorities and the mask read back. The diagnostic (t3) showed two GIC-400
  properties, not faults. The Non-secure priority mask keeps 4 bits (0xff reads
  back as 0xf0), and PPI IDs 16–24 are not implemented (read as zero, ignore
  writes). The check now compares implemented bits only and requires a priority
  to read back only for interrupts that are enabled.
- **t6 (stress):** 6 four-core workloads, 12 ground-truth runs, 6 benchmarks
  and 7 `nmion` / 6 `nmioff` switches under load, with timer and PMU sampling
  running together: all completed, 115,690 samples, no fault.
- **t8 (soak, 6 minutes):** 25 four-core workloads and 25 ground-truth runs in
  pseudo-NMI mode, both samplers on: all completed, 944,589 samples,
  `pmu_missed` 0, shell responsive.
- Two stress attempts stopped early because the host's USB-serial path dropped
  chunks of long output (a `threads` listing, a `profiler mask` table),
  including the shell prompt. A direct probe each time found LK responsive and
  still sampling in pseudo-NMI mode. The UART transmit path is polled, so the
  target cannot drop bytes; this is the known 6 Mbaud transport limit.

## What it does not cover

- IRQ handlers stay unsampled: exception entry still sets CPSR.I, and LK does
  not nest interrupts.
- Timer-mode sampling is unchanged: the scheduler tick reschedules, so it
  cannot run as a pseudo-NMI.
- Each mask change on GICv2 is a memory-mapped write. On a GICv3 target it is
  a system register (`ICC_PMR`), like Linux's arm64 pseudo-NMI; the LK side
  (per-core state, IRQ-path save/restore, handler rules) carries over.

## Reproduce

```text
profiler stop
profiler pmustop
profiler clear
profiler nmion
profiler maskon
profiler pmustart 0x11 1000000
profiler masktest 2000 500
profiler pmustop
profiler status
profiler dump
```
