# K1: stack-copy bounds on the real Pi (2026-10-05)

Captured by Claude on the Raspberry Pi 4B (A72, AArch32, Non-secure SVC) at
6 Mbaud with image `lk.bin` SHA-256
`0700d4d28db9f1cf10fdf0decdf9b8d915795577fd4ef785fafc29ec3e2a16f1` and its
matching `lk.elf` `0074efed991395dea3844008f8b89e4ce37b78971d1bd90673659bf3a8a250b0`
(both archived here). LK `88a8efae` plus overlays 0001–0014.

## The defect

Each sample copied a fixed 128 bytes upward from the interrupted SP. A thread
running near the top of its stack has fewer bytes than that above SP, so the
copy read past the end of its stack allocation, into whatever followed it.

## The fix

The copy now stops at the top of the stack the SP is on:

- created threads: `thread_t::stack + stack_size`;
- idle and bootstrap threads, which record no stack: the per-core boot stack,
  `abort_stack + (cpu + 1) * ARCH_DEFAULT_STACK_SIZE`;
- an SP on neither (for example inside a context switch): nothing is copied.

The rest of the slot is zeroed and each record carries `slen`, the number of
bytes copied. The host reads only those bytes, and when the copy stopped at a
stack top (0 < `slen` < 128) the unwinder ends the walk at the frame whose CFA
reaches that top, instead of following stale data there.

## Results

| Run | Capture | Finding |
|---|---|---|
| a_pmu_smp | `profiler smp 100000000`, PMU cycles | **All 799 samples** had only 20 bytes of stack above SP (the workers run at the top of their 4 KiB stacks): before the fix, each copied **108 bytes past its stack**. Now `slen` = 20, the copy ends exactly at the stack top, and the chain is `initial_thread_func;profiler_smp_worker;profiler_workload_inner`. Before the fix the stale word at the stack top added a second `initial_thread_func` (or an `[unknown]` root in the exporter) |
| c_idle_pmu | PMU period 10000, `masktest` on cpu2 | 4 idle-thread samples on cpu0 in `arch_idle`, SP on cpu0's boot stack: `slen` = 40, ending exactly at its top `0x8002ab00`. The shell thread on cpu2 has room for the full 128 bytes. `pmu_missed` = 5118 on cpu2: with this short period, whole periods fit inside each 300 us masked span (K6 counter) |
| b_timer_idle | timer sampling, `profiler nest` | Only the busy core is sampled: idle cores take no ticks, so the boot-stack path needs PMU mode (run c) |
| d_nmi_masktest | pseudo-NMI ground truth | Regression: 605/1194 samples (50.7%) inside the masked code, 0 delayed |

Reports (`*.report.txt`) and folded stacks were produced with the archived
ELF. Host tests: `test_dwarf_unwind.py` (new stack-top case),
`test_irqmask_report.py` (14, including `slen` parsing, corruption, range and
window), `test_perf_export.py` (26).
