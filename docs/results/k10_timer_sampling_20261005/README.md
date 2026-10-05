# K10: timer-mode sampling without phase lock, on the real Pi (2026-10-05)

Captured by Claude on the Raspberry Pi 4B (A72, AArch32, Non-secure SVC) at
6 Mbaud. Image `lk.bin` SHA-256 `866ffee1912e8c377d0b7f96fffc57d3070e22426d160961b3f7ba9d13bdf75e`, matching `lk.elf` `16002a54a54d8d6272a0e8dc90332746f0b8fe0cdb1088d38098b05ce435317a` (both
archived). `capture.log.gz` holds three dumps (one per run below, `--dump
0/1/2`) and the K11 probe output.

## The problem

Timer-mode samples came from LK's scheduler tick. LK re-arms that tick
relative to when it is handled, so a tick held back by IRQ-masked code
shifts every later tick. In the K6 ground truth (`masktest`, 50% masked) the
samples phase-locked onto the masking pattern: 65% / 35% instead of 50/50.

## The fix

The profiler now owns a sampling timer: the per-core virtual timer (CNTV,
GIC PPI 27; LK's tick stays on the Non-secure physical timer, PPI 30).

- **Fixed grid.** Time is cut into periods of the requested length, counted
  from `profiler start`. A late sample does not move later deadlines.
- **Stratified random phase.** Each period gets one deadline at a random
  offset inside it (xorshift32 per core). With a fixed offset the grid would
  still alias with a workload whose period divides the sampling period. With
  one uniform point per period, every piece of code is sampled in proportion
  to its time, periodic workload or not.
- **Lost periods are counted.** A period that ends entirely inside masked
  code gets no sample. It is counted per core (`tmissed` in the dump,
  `timer_missed` in `status`), not hidden.
- `profiler start [period_us]`: 50 us to 1 s, default 10000 (the old tick
  rate). The dump header (format 3) records the period, and whether it
  changed during the session.
- Under pseudo-NMI (`nmion`), the sampling timer also gets the top priority,
  so timer samples reach masked thread code directly.

## Results

`profiler masktest 4000 500` (or `40000 500`): 500 us masked, then 500 us
unmasked, on one core. "Masked share" counts the samples in the masked half:
taken at the unmask point in CPSR.I mode, or inside the masked spin under
pseudo-NMI.

| Run | Period | Samples on the test core | Masked share (±1σ) | Accounting | Delayed |
|---|---|---|---|---|---|
| Before (K6 runF, LK tick) | 10 ms | 200 | 65% | 49.6% | 69 |
| a: CPSR.I masking | 1 ms | 4006 | **49.0%** ± 0.8 | 50.01% | 1949 (all attributed) |
| b: CPSR.I, grid commensurate with the workload | 10 ms | 3999 | **50.9%** ± 0.8 | 50.00% | 2019 (all attributed) |
| c: pseudo-NMI | 1 ms | 4000 | **50.4%** ± 0.8 (2012 inside the masked spin) | 50.00% | **0** of 15999 |

All three agree with the accounting to within 1.3σ. Run b is the hard case:
the 1 ms workload divides the 10 ms grid exactly, so a fixed-phase grid
would give 0% or 100%. Exploratory runs on the image before the K11 probe
commands were added gave 49.6% (n=2003), 45.6% (n=600, 10 ms), 48.0%
(pseudo-NMI, n=1999), 49.1% (n=3999) and 50.8% (n=3999, 10 ms). Pooled over
all eight runs: 49.6% against 50.0%.

Each core took exactly one sample per period (2006 in 2.006 s, for example),
and `tmissed` was 0: no period lay entirely inside a 500 us masked span. The
sampling interrupt costs about 2.5 us per sample (`[irq handler, GIC ID 27]`
in the accounting: 0.085% of each core at 1 kHz).

Host: dump format 3 is parsed and checked (`test_irqmask_report.py`, 23
tests). Format 2 logs still parse; their timer mode is reported as LK's
tick. The exporter records `timer_period_us`, `timer_source` and
`timer_period_changed_mid_run`. The K3 archive reports byte-identically. The
build has no FPU instructions.
