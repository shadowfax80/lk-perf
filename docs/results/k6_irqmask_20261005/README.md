# K6: IRQ-masked time accounting on the real Pi (2026-10-05)

Captured by Claude on the Raspberry Pi 4B (A72, AArch32, Non-secure SVC, the
target's state) at 6 Mbaud. Real hardware data. K6 is route 1 for the
IRQ-masked blind spot: measure the masked time, attribute the samples it
delays, and remove masking the profiler itself caused. Design and limits:
[ARCHITECTURE.md §11.1](../../ARCHITECTURE.md#111-irq-masked-execution-is-a-systematic-blind-spot).

## Images

| Runs | Change under test | lk.bin SHA-256 | lk.elf SHA-256 |
|---|---|---|---|
| run1 | Accounting only (overlay 0012) | `eddfab41…cfc` | `3413b7b2…754` |
| runA | + console without IRQ masking (0013), dump freezes accounting | `9458cf5d…bd4` | `cb8c7710…7b7` |
| runC | + `masktest` ground-truth workload | `429967c0…7d7` | `78a39a90…fbd` |
| runE, runF, runG | **Final:** + compensated PMU reload, cause chaining | `b85c9df8109147ec927f515cffbe8118f5cb10647af75ac690c73db92d4d7385` | `92afc62e9e2b30fe199749d06a56145718664901a5330fce47ffd9b9e922cc95` |

`lk.bin.gz` / `lk.elf.gz` are the final image and its matching ELF. LK base
`88a8efae` plus overlays 0001–0013; the overlay series was replayed on a fresh
clone of that commit and matched the built tree byte for byte. No FPU/NEON
instructions in the image.

## Results

| Run | Workload and mode | Finding |
|---|---|---|
| run1 | `profiler smp 100000000`, PMU cycles, period 1000000 | Accounting works on hardware: 200 PMU-handler regions per core, one per sample. `console_print_lock` masked IRQs for up to **349.5 us** per printed line; the dump's own output inflated cpu1 to 59% masked |
| runA | same | With 0013 the longest masked span on any core fell to **~4 us** (an IRQ handler); console printing no longer appears |
| runC | `profiler masktest 2000 500` (50% masked by construction), PMU | Accounting exact (cpu2 **49.4%** at `profiler_masked_spin`), but **999/1000** samples landed at the unmask point: reloading the full period at the delayed interrupt phase-locked the sampling grid to the masking pattern |
| runE | same, final image | Samples **51.8% / 48.2%** unmasked/masked (truth 50/50); 574/1200 delayed, **all 574 attributed to `profiler_masked_spin`** (before chaining, 61 were credited to the timer handler taken first at the same unmask); accounting 49.6%; `pmu_missed` 0 |
| runF | same, timer (scheduler tick) sampling | Accounting 49.6%; 69/200 delayed, all attributed to `profiler_masked_spin`. Sample split **65% / 35%**: LK re-arms its tick from the handling time, so timer samples still phase-lock to masking (limitation; use PMU mode) |
| runG | `profiler smp 100000000`, PMU, final image | Regression: 100% `profiler_workload_inner`, as before K6; 0 delayed; masked time 0.07–0.11% per core, almost all in IRQ handlers |

Reports (`*.report.txt`) and FlameGraph input (`*.folded`) were produced by
`scripts/pi4_pc_histogram.py` from the logs and `lk.elf`. In `runE_pmu_masktest.folded`
the masked half appears as
`cmd_profiler;profiler_masked_spin;[irq-masked: profiler_masked_spin] 574`.
runG had two serial-path sequence gaps (known transport loss, dropped and
counted).

## Reproduce

```text
profiler stop
profiler pmustop
profiler clear
profiler maskon
profiler pmustart 0x11 1000000
profiler masktest 2000 500
profiler pmustop
profiler stop
profiler status
profiler dump
```

`python3 scripts/pi4_pc_histogram.py <log> <matching lk.elf> --folded <out>`
prints the masked-time tables and the delayed-sample attribution.
