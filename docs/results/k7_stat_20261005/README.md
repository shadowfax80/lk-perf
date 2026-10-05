# K4 and K7: `profiler stat` on the real Pi (2026-10-05)

Captured by Claude on the Raspberry Pi 4B (A72, AArch32, Non-secure SVC) at
6 Mbaud. K7 image `lk.bin` SHA-256 `5a9b62edf8472e98bf68f5a8a63bdcf1ebe02d0027f8a61c8d79162eabe374d9`, matching `lk.elf` `9ee76eee1cda3b355ea45b42da5dfd1470258a484fe36e0a279602f8d3814877` (both
archived).

## K4: the count included its own console output

Before, four `printf` lines sat inside the counting window. The fix moves
them out. Logs: `k4_before.log.gz` (K10 image) and `k4_after.log.gz` (K4
image `5061f3f1…`).

| `profiler stat N` | Before | After |
|---|---|---|
| N = 1000 | 77,928 / 77,988 / 77,920 cycles | 2,119 / 2,009 / 2,009 |
| N = 5,000,000 | 10,053,414 / 10,053,203 | 10,001,332 / 10,000,981 (2.000 per iteration) |

## K7: counting any command, on every core

```text
profiler stat [-e ev,ev,...] <console command ...>
profiler stat [iters]                 built-in workload, as before
```

- Counts on all cores at once (IPI to arm, IPI to freeze and read), so a
  command that migrates or starts threads on other cores is fully counted.
  The report gives one row per core, a total, and derived metrics: IPC,
  L1D miss %, branch mispredict %, and L2D refills per 1000 instructions,
  computed in integer arithmetic.
- Up to six event counters plus the cycle counter. While `pmustart`
  sampling is armed, counter 0 stays with it: five events are available,
  and sampling keeps running. `pmustart` is refused while `stat` counts.
- 64-bit counts. The cycle counter uses its 64-bit mode (PMCR.LC). Event
  counters are extended through their overflow interrupt, the same per-core
  SPIs PMU sampling uses. The handler now reads PMOVSR, so only counter 0's
  flag produces a sample.
- Default events: INST_RETIRED, L1D_CACHE_REFILL, L1D_CACHE, BR_MIS_PRED,
  BR_PRED, L2D_CACHE_REFILL. Events beyond 0x3f are accepted, since the A72's
  implementation-defined events are not listed in PMCEID; for lower events,
  any not marked in PMCEID get a warning.
- The command runs through LK's `console_run_script_locked`. Nothing prints
  inside the window except the command itself; its own output is counted,
  as it would be under `perf stat`.

| Check | Result |
|---|---|
| Built-in workload, 5M iterations | 20,001,6xx instructions, 10,003,xxx cycles: 4.000 instructions and 2.000 cycles per iteration, 1.000 branches per iteration |
| `profiler smp 20000000` | 100.01M to 100.05M instructions on each of the 4 cores, IPC 2.50 |
| `-e 0x11,0x8,0x1b` | CPU_CYCLES event equals the 64-bit cycle counter exactly on every core (32,112,870 = 32,112,870) |
| 3,000,000,000 iterations, 10.0 s | 12,000,590,294 instructions (2.8 × 2^32): 4.0002 per iteration; 6,001,177,051 cycles |
| With `pmustart 0x11 1000000` armed | five events, counts unchanged; sampling took 167 samples over 166.5M cycles |
| Under pseudo-NMI (`masktest`) | CPU_CYCLES = cycle counter; 600.5M cycles in 1.0009 s |
| Bad input | 7 events, a malformed list, and `stat` inside `stat` refused |
| Idle cores | about 2,000 cycles per run: the cycle counter stops in WFI |

The cycle counts show that the A72 runs at **600 MHz** under LK (6.0012e9
cycles in 10.002 s). LK never raises the firmware's boot clock; earlier
timing results were all taken at this clock.

## Console output and the USB-serial link

The first K7 build printed each table row as many small `printf`
fragments. At 6 Mbaud the host side then lost large parts of the output, and
sometimes the shell prompt. The target transmits every byte (polled,
blocking PL011 writes), so the loss is in the USB-serial path. It is far
worse for fragmented output than for whole lines. Each `stat` line is now
assembled first and written once, which made the tables almost clean
(occasional small drops remain, the same as in dumps). `profiler mask`
still prints fragmented lines.
