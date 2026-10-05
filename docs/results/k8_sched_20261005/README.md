# K8: scheduling, wakeup, blocking and CPU-frequency capture (2026-10-05)

Captured by Claude on the Raspberry Pi 4B (A72, AArch32, Non-secure SVC) at
6 Mbaud. Image `lk.bin` SHA-256 `5c6af40e858a79d7bc7aa7bd7163efd89bcd2aa3d53289bbc30a24a5d9132200`, matching `lk.elf` `03aae4ac1e1e5202c911a8bd48a49af8f822a43d7d158deb6f54f72bc8178f58` (both
archived).

## What is recorded

LK overlay 0015 adds two hooks in `kernel/thread.c`, weak no-ops unless
the profiler is linked in:

- `lkperf_sched_switch(old, new)`: called right before every context switch.
- `lkperf_sched_wakeup(t, from, wq, err)`: called at the six places a thread
  becomes ready again (resume, unblock, sleep timer, wait-queue wake one/all,
  wait-queue timeout or kill).

Both run with `thread_lock` held and interrupts disabled. The profiler
appends to lock-free per-core rings (8192 events per core, oldest
overwritten and counted), timestamped with CNTPCT:

| Event | Fields |
|---|---|
| SWITCH | old thread, new thread, the state the old one leaves in (preempted/yielded, blocked, sleeping, suspended, exited), the wait queue it blocks on |
| WAKE | woken thread, the thread that was running when it was woken (the waker), the state it leaves, the wait queue, the wait result (timeout) |
| NAME | thread, priority, flags, name: emitted the first time a thread is seen. A `thread_t` address is reused after its thread exits, so names travel in the stream and the host starts a new thread identity after each exit |
| FREQ | measured ARM clock, set ARM clock, firmware throttling flags, from the VideoCore mailbox, by a low-priority thread every `freq_ms` |

Commands: `profiler sched on [freq_ms]` / `off` / `sched` (status),
`profiler schedump`, `profiler freq`, and `profiler schedtest [iters]`
(ground truth). The dump is checksummed per line and carries the K2 run id
and image hash. The thread table is printed before and after the events, so
one lost burst on the link cannot cost the names.

Host: `scripts/pi4_sched_report.py LOG ELF [--systrace FILE]` reports, per
thread, CPU time, wakeup-to-running latency and why it left the CPU; time
off the CPU by thread and reason, with wait queues symbolized (including
"X's thread_t+off" for a join); per-core busy time; and the clock history.
`--systrace` writes `sched_switch`, `sched_waking` and `cpu_frequency` for
Perfetto.

## Ground truth (`schedtest 200`, `schedtest.log.gz`)

A waker sleeps 2 ms, works 200 us and signals an event; a waiter blocks on
it and works 500 us. Two lockers each hold a mutex for 300 us and then
sleep 1 ms. The test also times every `thread_sleep` with CNTPCT, as an
independent check.

| Check | Expected | Trace |
|---|---|---|
| Waiter blocks on `profiler_st_event` | 200 | 197 (3 lost in transfer in this log; 200 in the stress run) |
| Waiter CPU time | 200 x 500 us | 99.1 ms (100.5 ms in loss-free runs) |
| Waker CPU time / lockers' CPU time | 40 ms / 60 ms | 40.4 / 60.7, 58.8 ms |
| Mutex blocks, longest | at most the 300 us hold | 297.2 us |
| Waker sleep: traced off-CPU + wakeup latency vs CNTPCT | equal | 2013 + 47 us vs 2066 us |
| Lockers' 1 ms sleep vs CNTPCT | equal | 1005, 1009 us vs 1012 us |
| ARM clock | 600 MHz, no throttling | 600.117-600.170 MHz, flags 0 |

The CNTPCT window around `thread_sleep` also includes the time from wakeup
to running again, which the trace reports per thread (about 45 us for the
waker, about 2 us for the others), so the two agree.

## Stress (`stress_nmi_sampling.log.gz`)

Pseudo-NMI on, 1 kHz timer sampling, recording with a 20 ms clock thread,
`schedtest 200` and then `profiler smp 20000000` on four cores. Everything
coexisted: 2935 events, both dumps verified. The four `smp` workers reuse
the `thread_t` addresses of the exited `schedtest` threads; they are
reported as separate threads (66.8 ms each), and a join on one of them is
named after the worker, not the thread that held the address before. The
low-priority clock thread waited 2.2 ms on average (up to 49 ms) to run
while the workers held every core.

## Perfetto (`stress.systrace.gz`, `perfetto_trace_processor.txt`)

Perfetto's `trace_processor` imports the systrace output. Per-thread CPU
time from its `sched_slice` table equals the report (st_waiter 98.791 vs
98.789 ms, workers 66.78-66.79 ms, lockers 61.66/61.65 ms, waker 41.00 ms).
The clock appears as a cpufreq counter (100 samples, 600117-600169 kHz).
Wakeups: Perfetto's systrace importer ignores `sched_wakeup` lines but turns
`sched_waking` into runnable states linked to the waker, so the exporter
writes `sched_waking`. 884 wakeups have a waker: `st_waker` woke the
waiter 196 times, the lockers woke each other 24 and 28 times (the mutex),
and sleep timers fired on idle cores 614 times. The one import warning
(`mismatched_sched_switch_tids`) comes from events lost in transfer.

## Cost (`hook_cost.log.gz`)

`profiler stat -e 0x8 profiler schedtest 200`, recording off and on, twice
each: 106.08-106.10M instructions off, 106.53-106.60M on, about 170
instructions per recorded event; cycles (160.9-161.5M) and wall time
(459-463 ms) show no measurable difference. With recording off a hook is a
call and a load.

## Idle and sleep on the Pi

LK's idle thread runs `wfi` (`arch/arm/arm/ops.S`). There is no deeper
idle state, core power-down, cluster or system suspend, or frequency
scaling on this platform: the clock stays at the firmware's 600 MHz. Any
interrupt wakes a core, normally the per-core timer that LK's dynamic tick
programs for the next timer event. Wakeup to running is about 2 us
(p50 1.6-1.9 us).

## Limits

- Interrupt handlers do not appear as threads. A wakeup from a timer or
  device interrupt names the thread that happened to be running (often
  idle) as the waker.
- Events lost in transfer (0.1-2% here) can break a switch chain; the
  report counts such breaks.
- A thread that runs throughout the window without a switch is not named.
- The clock thread's own sleeps and switches are part of the trace.
