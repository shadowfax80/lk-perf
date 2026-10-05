# Raspberry Pi 4B bring-up plan

> Work queue, ownership and status updates: [HANDOFF.md](HANDOFF.md). This
> file is the hardware evidence record.

**No-CFI callers, K3 (2026-10-05, Claude):** samples in LK's hand-written
assembly (memcpy, memset, spinlocks, cache operations) had no caller because
that code has no DWARF CFI. The unwinder now takes the caller from the
interrupted LR when the instruction before it is a call outside the leaf. In
the new `profiler memtest` ground truth all 460 such samples went to their
true callers. Details:
[results/k3_nocfi_fallback_20261005](results/k3_nocfi_fallback_20261005/README.md).

**Self-describing capture, K2 (2026-10-05, Claude):** every dump now states
its session (run id, dump number), its image (a hash of the read-only bytes
that the host checks against the ELF), its sampling modes and PMU settings,
per-core taken/retained/overwritten/lost counts, and the exact number of
records sent. On the Pi this exposed exact serial loss (for example 7 of 8458
records) that sequence gaps alone could only estimate, and the host now
refuses a mismatched ELF. Details:
[results/k2_session_20261005](results/k2_session_20261005/README.md).

**Stack-copy bounds, K1 (2026-10-05, Claude):** the per-sample 128-byte
stack copy read past the end of thread stacks whenever a thread ran near its
stack top. That was the normal case: every four-core-workload sample had only
20 bytes above SP, so it read 108 bytes beyond. The copy now stops at the
stack top (thread stack, or the per-core boot stack for idle threads), and
the unwinder stops there too, removing a fake duplicate root frame. Details:
[results/k1_stack_bounds_20261005](results/k1_stack_bounds_20261005/README.md).

**Pseudo-NMI sampling, K12 (2026-10-05, Claude):** with `profiler nmion`,
LK masks thread code by GIC priority and the PMU interrupts outrank it. In the
ground-truth test, 50.5% of PMU samples landed inside the masked code itself
(0 delayed), against 0% in the default mode. Stress and a 6-minute soak with
both samplers and repeated mode switching ran clean (944,589 samples). Two
GIC-400 facts found on the way: the Non-secure priority mask keeps 4 bits,
and PPI IDs 16–24 are not implemented. Details:
[results/k12_pseudo_nmi_20261005](results/k12_pseudo_nmi_20261005/README.md).

**IRQ-masked time accounting, K6 (2026-10-05, Claude):** the profiler now
measures the blind spot it cannot sample. On the Pi, the K6 ground-truth test
(`profiler masktest`, 50% masked by construction) reported 49.6% masked time
at `profiler_masked_spin`, put 48.2% of PMU samples on the masked side and
attributed all 574 delayed samples to that function. Console printing had
been masking IRQs for up to 350 us per line; overlay 0013 removed that.
PMU reload is now delay-compensated; before that, the sampling grid
phase-locked to masking. Timer-mode sampling still phase-locks (HANDOFF K10).
Details and logs: [results/k6_irqmask_20261005](results/k6_irqmask_20261005/README.md).

**Hardware profile demo complete (2026-10-05, Codex):**
[`profiler smp 400000000` demo](results/lk_perf_demo_20261005/README.md)
captured PMU CPU-cycle samples (event 0x11, period 1000000) on all four Pi
cores. Target retained 3200 samples, 800/core. Initial serial dump lost five
records; a lightweight redump of the same stopped buffers exported all 3200
with zero malformed records, checksum rejections or sequence gaps. Published
raw archives, exact image/debug ELF, interactive FlameGraph SVG, perf-script
text/metadata, seven Perfetto SQL analyses with saved CSV, and offline replay.
Perfetto v58.2 imported 3200 samples, four threads with 800 each, and identical
stack paths; timestamps matched within 1 ns, importer error stats were zero.
Sample span: 1.332529 seconds. Every leaf is `profiler_workload_inner`; an
unresolved outer frame remains visible. Archive/ELF-image identity and full
offline replay passed. Reused existing build; no source/overlay/build change.
Pi left at lk-perf shell, 6000000 baud, both samplers stopped, 3200 samples
retained, COM5 closed. Software reset replaced the old BOLT payload; no
watchdog command issued by lk-perf. Shared Pi reservation released in BOLT
handoff; recheck state before next use. Known capture limitations remain open.

**Known-limitations update (2026-10-05, Codex):** scheduling, wakeups,
blocking reasons/durations, and CPU-frequency history are not captured.
Stack samples and gaps cannot establish scheduler latency, off-CPU wait
causes, or frequency changes. Added this limitation to README, architecture,
export documentation, and the generated metadata/stderr notice. Additional
target instrumentation/event export would be required; no such capability
was implemented or claimed. The targeted metadata export regression passed;
no target code or Pi state changed.

**Standard-tool exporter milestone (2026-10-05, Codex):**
`scripts/pi4_perf_export.py` now exports one completed dump as timestamped
perf-script text with a metadata sidecar. It preserves sample/CPU/time fields,
maps thread pointers to synthetic PID/TIDs, checks dump boundaries/integrity,
and records artifact hashes and operator-supplied event/run metadata.
All 26 exporter tests passed, including real Perfetto Trace Processor v58.2
import and SQL checks of sample counts, timestamps, identities, and call-chain
order; existing DWARF regressions passed. See [EXPORT.md](EXPORT.md) and
[verification evidence](results/PERF_EXPORT_VERIFICATION.md).
Missing stack frames and execution while IRQs are masked remain explicit
known limitations in docs and every export sidecar. No target code/overlay
or Pi state changed, and no new hardware capture was claimed. The target's
self-describing dump/build-ID/footer and capture-synchronization gaps remain
open; host-side export metadata does not close those items.

**Architecture documentation (2026-10-05, Codex):**
[ARCHITECTURE.md](ARCHITECTURE.md) describes the implementation at `5919f40`,
with diagrams, source references, capture/dump contracts, and correctness
boundaries. It reconciles earlier stage descriptions with current source,
including all-core PMU sampling, counter-1 `stat`, the 6-Mbaud payload versus
3-Mbaud host default, and the corrected Non-secure SVC target. This was a
documentation-only change; no new build, capture, or hardware verification
was performed. This file remains the hardware evidence and work-backlog
record; the architecture document creates no separate TODO queue.

Why this exists: `docs/DESIGN.md`'s Stage 5 (PMU-event-triggered sampling)
was confirmed real-hardware-only on QEMU (no PMU IRQ route in the virt
device tree, and PMU register access itself faults without a
secure-monitor boot stage). Real Cortex-A55/production hardware wasn't
available, so a Raspberry Pi 4B (Cortex-A72, BCM2711) was chosen as the
cheapest real-hardware path to validate two specific things QEMU
categorically cannot: DWARF-CFI unwinding robustness, and whether real PMU
event counters (cache misses, branch mispredicts) actually respond to
workload behavior.

**Scope: this project is a PoC for a real target platform's actual perf
use case**, not a general-purpose ARM32 profiler. That target platform's own shipped
firmware has no EXIDX (ARM EHABI unwind tables aren't generated for its
production build) but does carry DWARF CFI (`.debug_frame`) in its debug
symbol files -- the same mechanism Trace32 already uses there to unwind
crash dumps. lk-perf's unwinder uses DWARF CFI for the same reason, so
the mechanism validated here transfers directly, rather than validating
a format (EXIDX) the target platform doesn't actually use.

**Workload is ARM/Thumb interworking code (`-mthumb`), not pure ARM.**
`scripts/dwarf_unwind.py` masks the ARM interworking ISA bit (bit 0,
set by BL/BLX on a Thumb call target, and set on Thumb function symbols
in the ELF) before ever using an address as an FDE/PC lookup key --
see `strip_isa_bit()` and the regression case in
`scripts/test_dwarf_unwind.py`. DWARF CFI decoding itself doesn't care
about instruction set (GCC emits correct `.debug_frame` rules either
way); only address bookkeeping needed the fix. **Resolved as part of
M5**: the capture hook and dump format don't need their own ISA-bit
handling -- the raw `lr`/`pc` fields are dumped unmasked, and
`dwarf_unwind.py`'s `strip_isa_bit()` already strips the bit wherever
it's used as a lookup key, which is the only place it matters. The
`profiler_arm_mode_func` edge case (see the M5 edge-case validation
section below) confirmed this end to end with a real captured sample
from genuine ARM-mode code.

Two more real bugs in `scripts/dwarf_unwind.py` were found and fixed
while reasoning through what Thumb code actually needs from the
unwinder, both covered by a hand-written-assembly regression case in
`scripts/test_dwarf_unwind.py` (`testdata/thumb_edge.S`):

- Only SP and LR were carried from one resolved frame into the next;
  every other register's rule (r4-r11, in particular r7, the usual
  Thumb frame pointer) was discarded. Thumb code frequently defines
  the CFA via r7 rather than SP+offset, and that rule can be a frame
  or more removed from wherever the register was last live, so
  dropping it broke any unwind two or more levels deep through
  r7-framed code. Fixed by resolving and carrying forward every
  register's rule each step, not just LR's.
- A resolved return address (from LR) was looked up as-is to find the
  next frame's FDE/row, rather than at `address - 1`. LR points to the
  instruction *after* a call; when that call is the last instruction
  of its function -- the normal shape of a call to a noreturn function
  like `panic()` or an assert handler, which gets no epilogue -- the
  return address lands exactly on the next function's first byte, and
  looking it up as-is finds that unrelated function's FDE (or none at
  all) instead of the row that was actually active at the call site,
  either silently misattributing the frame or stopping the unwind one
  level short. Fixed by looking up `pc - 1` for every frame after the
  first (the initial, actually-executing PC still needs no adjustment).

## What "close to Linux `perf`" actually needs, revisited for the target platform

The M1-M4 hardware work above is board bring-up, not profiler
capability -- none of the timer-sampling/PC-LR-FP-capture/FP-chain-unwind
work from `docs/DESIGN.md`'s Stages 1-4 has run on this hardware yet,
only on QEMU. M5 is where the profiler itself starts running here.
Revised scope, closest analogue to `perf` noted per item:

1. **M5, redesigned (`perf record`, timer mode). In progress -- built
   and verified on a build pod, not yet run on the actual Pi.**

   Each sample must capture more than PC/LR/FP: also SPSR (Thumb-vs-ARM
   mode and the interrupted mode), the *interrupted* thread's own real
   SP, r7/r11, thread ID, CPU number, timestamp. Offline DWARF
   unwinding needs the stack contents as they were at sample time,
   which a live target can't preserve until the host reads it (QEMU's
   old approach paused the VM; real hardware can't). Prefer unwinding
   **on the target**: compile `.debug_frame` into a compact
   address-range -> CFA/register-rule table on the host, load it onto
   the Pi, and unwind inside the sample handler so only the resulting
   PC chain needs to leave the device -- `dwarf_unwind.py` becomes the
   table generator (and the reference to check the on-target unwinder
   against), the way the Linux kernel's own ORC unwinder works.

   **Done (2026-09-28):**
   - No-FPU/NEON build fidelity (`overlay/lk/0007-rpi4-no-fpu-neon.patch`,
     `project/rpi4-test.mk` drops `app/tests`) -- see the FPU/NEON note
     further down. Prerequisite for any new capture code, since it's
     easy to accidentally pull in a float path (e.g. via a library
     dependency) that the target platform's Cortex-A55 could never run.
   - **The interrupted SP derivation.** LK's standard IRQ entry
     (`arch/arm/arm/exceptions.S`'s `save` macro) captures `frame->usp`/
     `frame->ulr` via `stmia sp,{r13,r14}^` -- but that instruction only
     captures the **USR-mode banked** SP/LR. Checked `arch/arm/arm/thread.c`:
     this project's LK threads never switch to USR/SYS mode at all
     (`arch_context_switch`/`arm_context_switch` do a raw cooperative
     stack-pointer swap, no CPSR mode field anywhere) -- everything runs
     in SVC mode the whole time, so `usp`/`ulr` are dead, irrelevant
     registers here, not usable for unwinding. The real interrupted SP
     needs **no new assembly**: it's `(uint32_t)(frame + 1)`, i.e. one
     past the end of the `struct arm_iframe` that `save` already pushes
     -- traced the exact push/align sequence in `exceptions.S` by hand
     (`srsdb`+`push{r0-r3,r12,lr}`+the usp/ulr slot all land at a fixed
     offset before the variable `stack_align` padding; `save`'s own
     final `r0 = pre-align sp` plus the struct's own size gets you
     there without needing to reason about that padding at all -- `r0`
     *is* the iframe pointer). Not yet wired into `profiler.c` --
     that's part of the "not done" list below.

   **Also done (2026-09-28):**
   - **`app/profiler` was never actually linked into the rpi4 build.**
     Every prior hardware milestone (M1-M4) booted without it --
     `project/rpi4-test.mk` never listed it in `MODULES`. Fixed; the
     `profiler` shell command has never existed on this target before
     now.
   - **`PROFILER_TIMER_IRQ` was hardcoded to 27** (qemu-virt-arm's
     timer vector). BCM2711 uses GIC ID 30 for the same timer (see
     `overlay/lk/0004-bcm28xx-add-rpi4.patch`'s own redefinition of
     `INTERRUPT_ARM_LOCAL_CNTPNSIRQ`). Without this fix sampling would
     have silently captured zero samples forever on real hardware --
     found by checking the actual registered vector before the first
     hardware test, not by a failed one.
   - `profiler.c`'s sample record now includes SPSR, the interrupted
     SP (the `frame + 1` derivation above, now actually wired in), and
     the interrupted thread's `thread_t*` as a de-facto TID.
   - New `profiler dump` shell command: tagged `SAMPLE ...` text lines,
     oldest-to-newest per core.
   - New `scripts/pi4_pc_histogram.py`: parses `dump` output, symbolizes
     via `dwarf_unwind.py`, prints a per-function histogram, optionally
     writes a FlameGraph-compatible folded file. Tested offline against
     the `nested.c` fixture with a synthetic log.

   All of the above verified via a real `setup.sh` + `make rpi4-test`
   build on a RunPod CPU pod, including a full disassembly confirming
   the vector check, the SP/SPSR captures, and the thread-pointer read
   (via `TPIDRPRW`) all compile to exactly what was intended, and a
   full VFP/NEON re-scan (still zero instructions).

   **Run for real on the Pi 4B (2026-09-28) -- timer-tick sampling
   confirmed working end to end, single-core and SMP:**
   - New `scripts/pi4_run.py`: loads an image, then runs a scripted
     command sequence over serial (waits for output to go idle between
     commands rather than assuming any particular shell-prompt shape),
     capturing everything -- the actual driver for this test and for
     any future one.
   - `profiler start` -> `profiler bench 5000000` -> `stop` -> `dump`:
     13 real samples, all correctly landing in `profiler_workload_a`/
     `profiler_workload_b` (7/6 split) once run through
     `pi4_pc_histogram.py` -- the exact two functions `bench` runs.
     Boot log confirms the IRQ fix: `Generic timer register irq 30 on
     cpu 0`, `profiler: sampling started (irq 30, ...)`.
   - `profiler smp 8000000`: 8 samples, exactly 2 per core across all 4
     cores, each with a genuinely distinct SP and thread pointer (no
     cross-core aliasing) -- symbolized 8/8 to
     `profiler_workload_inner`, the one function all 4 worker threads
     actually run. `fp=0` and `lr=0x9e3779b9` (a literal data constant
     from that function's own loop body, not a return address) on
     these samples -- expected, not a new bug: this is the same
     GCC-reuses-lr/omits-fp-under-optimization limitation the original
     QEMU-era Stage 2/3 work already documented, and exactly why DWARF
     CFI (not the FP chain) is the real long-term unwinding mechanism.

   **Full multi-frame DWARF-CFI unwinding, working on real hardware
   (2026-09-28):** each sample now also captures
   `PROFILER_STACK_CAPTURE_BYTES` (128) of raw stack memory starting at
   `sp`, and `pi4_pc_histogram.py` feeds it to `dwarf_unwind.py`'s
   existing `DwarfCFIUnwinder` (built and tested weeks ago, unchanged
   here) via a `read_memory` callback backed by that captured window --
   a read outside it returns 0, which `unwind()` already treats as a
   clean stop, so nothing in `dwarf_unwind.py` needed to change.
   Verified offline first (replayed `test_dwarf_unwind.py`'s own
   ground-truth 4-frame scenario through a synthetic `dump`-shaped log
   and got back the identical chain), then for real:
   - `profiler smp 8000000`: all 8 samples (2 per core, all 4 cores)
     unwind to the identical, correct 4-frame chain --
     `initial_thread_func` -> `profiler_smp_worker` ->
     `profiler_workload_inner`. Confirmed correct against the
     disassembly, not assumed: `profiler_workload_mid`/`_outer` don't
     appear because GCC tail-call-optimized both into plain `b.w`
     branches (no `bl`, no frame, `lr` never touched) -- there is
     genuinely only one real call frame between `smp_worker` and
     `workload_inner` in the actual compiled code, and the unwinder
     correctly reflects that rather than the C-level 3-function
     abstraction.
   - `lr=0x9e3779b9` (that data constant, not a return address) shows
     up in the raw sample but the unwinder does **not** get fooled by
     it -- it reads the real saved return address from the captured
     stack memory instead of trusting the live (temporarily
     scratch-clobbered) `lr` register, which is exactly the case DWARF
     CFI was chosen over simpler LR/FP-chain unwinding to handle
     correctly.

   M5's core mechanism -- real timer-tick sampling *and* real
   multi-frame DWARF-CFI unwinding, both on actual Pi 4B hardware -- is
   now done and verified. Remaining polish: a real `-mthumb`-compiled
   unwinder regression case (still only hand-written .S, see item 2
   below), and whatever `profiler stat`/report-annotate/PMU work is
   still ahead per the rest of this section.
2. **Unwinder correctness for real `-mthumb` code.** The two fixes
   above, done. Still to do: a real `-mthumb`-compiled regression case
   (not just hand-written .S), and, if the target platform is built with
   armclang/armcc rather than GCC, confirm that toolchain's `.debug_frame`
   output matches the same assumptions -- CFI encoding details can
   differ between compilers.
3. **`profiler stat` (`perf stat`, counting mode). Done and confirmed
   working on real hardware (2026-09-28, commits `0dc3437`/`df94539`).**
   Reads PMCEID0/PMCEID1, configures event counter 0 for
   `L1D_CACHE_REFILL` (event `0x03`), enables it alongside the cycle
   counter, runs a workload, reports cycles / iters-per-cycle /
   cache-refills-per-iter. Printed a message before each new
   coprocessor access on purpose, so a fault would show exactly where
   in the UART log -- turned out not to matter: **no fault anywhere.**

   This resolves a real open question, not just adds a feature: the
   QEMU target's own `pmu` command had confirmed PMU coprocessor access
   *faults* there (no secure-monitor boot stage to clear the NSACR
   trap) -- untested until now whether real Pi 4B firmware clears that
   trap the way real secure-world boot normally does. It does. Real
   output on hardware: `PMCEID0=0x7fff0f3f` (most architectural events
   implemented), `PMCR=0x41023001` (6 programmable event counters,
   matches a real Cortex-A72's PMU), `5000000 iters, 10098142 cycles,
   12 L1D_CACHE_REFILL` for the simple arithmetic-loop workload (~2
   cycles/iter, near-zero cache misses -- both exactly what a
   register-resident loop touching one `volatile` variable should
   produce). `app/profiler/profiler.c`'s `pmu` command message updated
   to stop implying this is still an open question on real hardware --
   only the PMU *interrupt* path (event-overflow sampling) remained
   unverified at the time this was written -- see item 5, also now done.
4. **`profiler report`/`annotate` (symbols + source lines +
   instruction-level hotspots). Done (2026-09-28, commit `76e8e4f`),
   entirely host-side -- no on-target change needed.**
   - `dwarf_unwind.py` gained `find_function()` (nearest function with
     its real size/ISA mode, for disassembling exactly its address
     range) and `resolve_lines()` (PC -> `file:line` via `.debug_line`,
     correctly handling `end_sequence` gaps between CUs). Verified
     against `test_dwarf_unwind.py`'s own fixture before trusting
     either: exact `nested.c:9/15/19` for the three known sample points.
   - `pi4_pc_histogram.py --annotate N`: disassembles the N hottest
     functions via `arm-none-eabi-objdump` over each one's real address
     range -- objdump already handles ARM/Thumb-correct disassembly
     from the ELF's own `$t`/`$a` mapping symbols, so nothing here
     needs to track instruction sets itself -- prefixes every
     instruction with its own sample count and interleaves source-line
     markers, a `perf annotate`-style per-instruction view.
   - Verified against real hardware capture (the PMU-event-sampling run
     above): each hot function's single loop-body instruction (the
     XOR-rotate accumulator / the add accumulator) got ~100% of that
     function's samples, exactly as expected for a tight inner loop.
   - Real pitfall hit and worth remembering: an early check against
     this same capture used a **stale local `lk.elf`** (only `lk.bin`
     had been re-fetched after the last rebuild) and got a
     systematically wrong but internally-consistent line-number offset
     -- looked plausible, wasn't. Always re-fetch `lk.elf` alongside
     `lk.bin` after any rebuild, not just the binary needed to flash.
   - **Done (2026-10-05):** a timestamped `perf script`-compatible
     text emitter, `scripts/pi4_perf_export.py`, verified with the real
     Perfetto importer. See [EXPORT.md](EXPORT.md). This is not native
     `perf.data` or Hotspot compatibility; target-side run/build/event
     identity and counted dump footers remain separate open work.
5. **PMU-event sampling. Done and confirmed working on real hardware
   (2026-09-28, commits `e9ed753`/`6aebf9c`).** `profiler pmustart
   <event> <count>` / `pmustop`: a real second sampling mode alongside
   timer-tick (`start`/`stop`), sharing the same ring buffers -- "sample
   every N occurrences of event X" instead of "sample every timer
   tick", the actual capability that separates this from a plain
   statistical profiler.

   BCM2711's PMU interrupt is **not** a PPI like the timer -- confirmed
   from the real Raspberry Pi kernel's own device tree source
   (bcm2711.dtsi): 4 separate per-core SPIs (`GIC_SPI 16-19` = real GIC
   IDs 48-51), each explicitly affinity-bound to one CPU. Getting this
   working needed two fixes neither hardware bring-up here had hit
   before, since every prior IRQ (timer, UART) was either a PPI or a
   single shared SPI:
   - `dev/interrupt/arm_gic/gic_v2.c`'s GIC init routes **every** SPI to
     CPU0 by default -- without reprogramming `GICD_ITARGETSR`, all 4
     PMU interrupts would land on core 0 regardless of which core's
     counter actually overflowed. Fixed with one register write (all 4
     IDs share one `ITARGETSR` register).
   - The same GIC init also configures **every** SPI as edge-triggered
     by default, but the PMU's overflow signal is level (stays asserted
     until software clears `PMOVSR`), matching the DTB's own
     `IRQ_TYPE_LEVEL_HIGH`. Left at the GIC's edge default, it silently
     never fired -- confirmed the hard way: the first real hardware
     attempt ran clean, no crash, correct routing, and produced exactly
     zero samples. Fixed by calling `gic_configure_interrupt()`
     (a real function, just missing from `arm_gic.h`'s own public
     declarations -- added the `extern` in `profiler.c`) to explicitly
     set `IRQ_TRIGGER_MODE_LEVEL` before unmasking.

   Real result after both fixes: `profiler pmustart 8 100000` (event
   0x08 = INST_RETIRED, every 100,000 instructions) through `profiler
   bench 5000000`: 1599 samples, split 800/799 between
   `profiler_workload_a`/`profiler_workload_b` -- exactly the two
   functions `bench` alternates between -- all correctly unwound to
   the expected 2-frame chain, timestamps evenly spaced (consistent
   with a steady instruction-retirement rate).

   Single-core only in this version, explicitly scoped: PMU control
   registers are banked per-core in hardware, so arming every core
   needs cross-core signaling (e.g. an SGI to each core) this doesn't
   do yet. One event at a time, correctly wired end to end, first --
   multi-event multiplexing remains out of scope per the original plan.

   Still open, not addressed by this: the interrupts-masked blind spot
   -- see "The interrupts-masked blind spot: a position" below for the
   full analysis and recommendation (2026-09-28).
6. **How samples leave the device.** UART at 115200 baud is ~11 KiB/s --
   fine for a small image, a real bottleneck for the sample dumps this
   plans for (M5's richer per-sample record puts a full 4-core buffer
   around 590 KB, ~52s to dump at that rate). The target platform's real path is
   more likely a Trace32/JTAG memory dump than serial, so the
   sample-buffer layout should still be a documented, self-describing
   format a host tool can read from a raw memory image -- UART is just
   one way to deliver that image on the Pi, and it's worth being fast.

   **Baud-rate calibrated on real hardware (2026-09-27): 3,000,000
   baud, ~212.8 KiB/s sustained (~19.3x over 115200's ~11 KiB/s).**
   `experiments/pi4-baudcal` (a one-shot test payload, loaded the same
   way as any other payload -- no SD-card reflash) tried a table of
   candidate rates -- 230400/460800/921600/1000000/1500000/2000000/
   3000000, computed as exact or near-exact divisors of the
   chainloader's 48MHz UART clock -- confirming sync via a PING/PONG
   handshake at each new rate, then a byte-exact echo stress test
   (multiple sizes up to 64 KB, multiple repeats). Result, run twice
   independently plus a 10x64KB confirmation pass at the winning rate:

   | rate | result |
   |---|---|
   | 115200 | stable, 11.0 KiB/s (baseline) |
   | 230400 | stable, 21.8 KiB/s |
   | 460800 | stable, 42.4 KiB/s |
   | 921600 | stable, 80.5 KiB/s |
   | 1000000 | **never syncs** (reproducible, not a fluke) |
   | 1500000 | **never syncs** (reproducible) |
   | 2000000 | **never syncs** (reproducible) |
   | 3000000 | stable, 212.2-212.8 KiB/s, 10/10 on a follow-up stress run |

   The 1M/1.5M/2M gap right below the working 3M rate is real and
   repeatable across independent runs -- most likely this host's
   USB-serial adapter (Prolific PL2303, per `pi4_serial_boot.py`'s
   `USB_SERIAL_VIDS`) or its Windows driver only cleanly supports a
   specific discrete set of non-standard baud rates, not an arbitrary
   continuum, and 3,000,000 happens to be one of the ones it supports
   while 1M/1.5M/2M aren't. Not investigated further since 3,000,000 is
   already the fastest rate the PL011 can reach at all from this 48MHz
   clock with `IBRD >= 1` (the hardware ceiling, not just this
   adapter's ceiling).

   Only the transient test payload's own rate changed during
   calibration -- the persistent chainloader's initial 115200 handshake
   was never touched and stays the universal fallback.

   **Integration done (2026-09-28), except the one piece that needs an
   SD-card reflash, deliberately left as a TODO:**
   - `platform/bcm28xx/platform.c`'s BCM2711 branch now reprograms the
     UART to 3,000,000 baud (`bcm2711_uart_speed_init()`, IBRD=1/FBRD=0)
     as the very first thing `platform_early_init()` does, before
     `uart_init_early()` -- LK's own `uart.c` never programs the baud
     divisor itself (`uart_init_port()` is a no-op; it inherits
     whatever the previous bootloader left the UART at), so without
     this LK would stay at 115200 forever regardless of calibration.
     Added as `overlay/lk/0006-bcm2711-uart-baud.patch`. Verified with a
     real `make rpi4-test` build (RunPod CPU pod, avoids Windows
     MAX_PATH issues with LK's own long paths) -- confirmed via
     disassembly of the built `lk.elf` that the exact intended register
     sequence (busy-wait, `CR=0`, `IBRD=1`, `FBRD=0`, `LCRH=0x70`,
     `CR=0x301`) is inlined at the very start of `platform_early_init`,
     immediately before the call into `uart_init_early`.
   - `pi4_serial_boot.py` now takes `--post-jump-baud` (default
     `3000000`): after the chainloader's own 115200 handshake and
     image transfer finish and the payload jumps, the host follows to
     this rate to match. `--baud` (the initial link speed) is
     unchanged at 115200, since that's the resident SD-card
     chainloader's fixed rate. Pass `--post-jump-baud 115200` for a
     payload that doesn't reprogram its own UART (e.g. an old
     pi4-baremetal image); `pi4_baud_calibrate.py` is unaffected, since
     it manages its own baud switching directly rather than going
     through this default.
   - **TODO, not done, needs an SD-card reflash**: the persistent
     chainloader (`experiments/pi4-serialboot`) itself still only ever
     talks at 115200 -- its own header handshake *and* the bulk image
     transfer that follows it both stay at the slow rate, since
     negotiating a faster rate for either would mean changing the
     firmware that's resident on the SD card. Left alone on purpose
     per explicit instruction. Whenever an SD-card reflash is
     acceptable, extend that protocol to negotiate up to 3,000,000 baud
     for at least the bulk transfer (keeping the initial header
     handshake at 115200 as the safe fallback, the same pattern
     `pi4-baudcal` already validated).
7. **Not planned, noted as a deliberate scope decision**: per-task/
   per-thread breakdown beyond what the sample record's thread-ID field
   already gives for free, and PMU event multiplexing/frequency-based
   sampling (`perf -F`) -- revisit only if the target platform's actual use case
   needs them.

**Not A55-representative** — A72 is a different, higher-performance
core than the eventual real target (the target platform: multi-core Cortex-A55).
This validates the *mechanism* (does interrupt-driven PMU sampling work
at all on real silicon, does DWARF CFI give more complete unwinds than
the FP-chain walker), not A55-accurate numbers.

**FPU/NEON disabled entirely (2026-09-28), matching the target platform exactly**:
The target platform has no FPU and no SIMD/NEON unit at all, unlike the A72's own
hardware. `overlay/lk/0007-rpi4-no-fpu-neon.patch` turns both off via
`ARM_WITHOUT_VFP_NEON := true` and gates one inconsistent, unreachable
`arm_fpu_set_enable(true)` call in shared LK code; `project/rpi4-test.mk`
drops `app/tests` (it pulled in `lib/libm`, which compiles several
routines with real hardware VFP instructions independent of that
kernel-level setting). Verified via a full disassembly of the linked
`lk.elf`: zero VFP/NEON instructions of any kind remain in the binary.

## M5 edge-case validation (2026-09-28)

Every M5 result up to this point had exercised only "easy" cases:
shallow chains, no recursion, no forced frame pointers, no real
ARM-mode code. `profiler edgetest` adds four workloads deliberately
built to stress specific things the pipeline had never actually been
run against on real hardware -- see `app/profiler/profiler.c`'s own
comments on `profiler_deep_1..8`, `profiler_recurse`,
`profiler_fp_forced_{inner,outer}`, and `profiler_arm_mode_func` for
exactly what each targets and how each was confirmed to compile as
intended (real `bl` chains vs. tail calls, real r7-based frames, real
ARM-mode encodings) before ever touching hardware.

**Real bug found and fixed: the unwinder's cycle guard broke real
recursion.** `profiler_recurse(20, ...)` -- genuine, non-tail
recursion, confirmed via disassembly to compile to a real `bl
profiler_recurse` (calls itself) -- revisits the exact same
instruction address at every depth. The unwinder's original cycle
guard (`if cur_pc in chain: break`) checked pc alone, and broke
immediately: every single captured sample inside 20 levels of real
recursion unwound to a chain of length 1. Fixed by keying the guard on
`(pc, cfa)` instead (commit `48a98cd`) -- recursion's cfa is different
at every real depth (each call has its own stack frame), while an
actual CFI-driven infinite loop would repeat both. Added a regression
case to `scripts/test_dwarf_unwind.py` reusing `nested.c`'s own already
-verified CFI data (no new fixture needed), confirmed it fails against
the pre-fix code, then confirmed the fix on real hardware: the same
workload now reaches depth 15-18 (previously always 1) before running
out of `PROFILER_STACK_CAPTURE_BYTES` (128) -- the capture window, not
the algorithm, is the real limit on recursion depth now.

**Everything else confirmed correct, no fixes needed:**
- The 8-level genuine non-tail-call chain (`profiler_deep_8` ->
  ... -> `profiler_deep_1` -> `cmd_profiler`) unwinds completely, no
  truncation -- each of those frames only needs ~8-16 bytes, so 128
  bytes covers meaningfully deep *non-recursive* real chains in this
  codebase already (a useful negative result: the original guess that
  8 levels would exceed the window was wrong, in the good direction).
- `profiler_fp_forced_inner`/`_outer` (forced `-fno-omit-frame-pointer`,
  confirmed via disassembly to really push/use r7 as CFA) unwound
  correctly on real hardware for the first time -- every other sample
  captured on this target before this had `fp=0` or garbage, since
  `-O2` omits frame pointers by default. This is real proof the r7-CFA
  path (previously only exercised by the hand-written
  `testdata/thumb_edge.S` test) works on genuine compiled code too.
- `profiler_arm_mode_func` (`__attribute__((target("arm")))`,
  confirmed via disassembly to be real 32-bit ARM encodings inside
  this otherwise all-Thumb build) unwound correctly and its symbol
  correctly carries no `(thumb)` tag -- real BL/BX interworking, ISA
  bit handled right, not just the offline synthetic test from before.
- No frame pointer is required anywhere in the base mechanism: the
  three cases above with `fp=0`/garbage all unwound correctly because
  their real CFI data defines the CFA via SP, never referencing r7/r11
  at all -- `fp` is only ever read when a specific function's own
  `.debug_frame` row says to (confirmed by `_resolve_cfa()` reading
  whichever register `cfa_rule.reg` names, never a hardcoded r7/r11
  fallback). This is exactly the property DWARF CFI was chosen for
  over the old FP-chain walker in the first place, now confirmed with
  real edge cases instead of just the original design rationale.

## The interrupts-masked blind spot: a position (2026-09-28)

**The mechanism.** Both sampling modes here (`profiler start`/`stop`
and `profiler pmustart`/`pmustop`) work by hooking a GIC interrupt --
the timer PPI, or the PMU's per-core SPI. Standard ARM GIC behavior:
while `CPSR.I` is set, neither can actually interrupt the core. The
signal doesn't disappear -- it stays pending at the distributor and
fires the moment interrupts are unmasked again -- but nothing gets
sampled for the entire masked duration, and the *next* sample after
unmasking lands on whatever happens to be running right then, which is
often not representative of the masked code at all. This is not a bug
in this project's implementation; it's how IRQ-driven sampling works
on any architecture. `perf` on Linux hits the identical problem, which
is exactly why it exists.

**This already has a real, concrete example on this target, not just
a hypothetical one.** `lib/io/console.c`'s print lock (the M3 SMP
console-race fix, `overlay/lk/0005-fix-smp-console-output-race.patch`)
holds `spin_lock_irqsave(&print_spin_lock)` across the *entire*
`vfprintf()` call, not just the final UART write -- deliberately, to
stop concurrent cores' output from interleaving mid-string. Every
`printf`/`dprintf` call on this system runs with interrupts masked for
its whole duration as a direct, necessary consequence. Any code that
prints a lot -- which describes most of this project's own bring-up
and debug work -- gets systematically under-sampled by exactly this
mechanism. This is a known, accepted tradeoff (the SMP correctness
problem it fixes was real and hard-won), not something to casually
"fix" by shrinking the lock back down.

**Why this can't be fixed here, and that's a fact about *this*
environment, not about the mechanism in general.** Linux's real
answer is FIQ (older ARM) or GICv3 priority-based pseudo-NMI (newer
ARM64): route the PMU interrupt at a priority/exception path that
bypasses the normal IRQ-disable mechanism entirely. Both require
configuring interrupt *group* membership at the GIC distributor
(`GICD_IGROUPRn` on this GICv2 hardware), which is Secure-state-only
per the architecture. LK on this Pi boots straight into HYP, non-secure
(confirmed during M1's own HYP->SVC bring-up work), with no secure-world
code of its own at all -- there's no path here to reconfigure interrupt
groups, not because it hasn't been implemented, but because the
privilege to do so was never available on this boot chain in the first
place. Attempting it isn't a "todo"; it's out of reach for this
specific validation environment regardless of effort spent.

**Target platform (corrected 2026-10-05): Non-secure, always AArch32
SVC -- the same security state and mode as this Pi.** An earlier note
here (2026-09-28) recorded the target as Secure and concluded that its
RTOS could reconfigure `GICD_IGROUPRn` to route the PMU overflow
interrupt to Group 0/FIQ. That premise was wrong: the target runs
Non-secure SVC, and its PMU sampling interrupts are ordinary IRQs (user,
2026-10-04). So the blind spot is **not specific to the Pi**: code that
runs with `CPSR.I` set is invisible to sampling on the target too, for
the same architectural reason. "Always SVC" still holds and remains a
useful cross-check on this project's own design: the interrupted-SP
derivation (`frame + 1`, see the M5 section above) and the
`usp`/`ulr`-are-dead-registers finding both depend on threads never
leaving SVC mode, so that piece of the mechanism transfers as validated.

**The position:**
1. Document the blind spot as a known, quantified bias in every report
   from this profiler, not a blocker to using it. Every statistical
   sampling profiler has *some* systematic bias; the useful move is
   making this one visible, not pretending it doesn't exist.
2. Still don't attempt FIQ/pseudo-NMI *on the Pi*. The GIC-group
   reconfiguration it needs is genuinely unreachable from this specific
   non-secure boot chain regardless of effort spent, and the target is
   Non-secure too, so there is no target-side FIQ route to prototype
   against either.
3. What *is* worth building here, being genuinely cheap and actually
   informative regardless of target: a running counter of total cycles
   spent with interrupts masked (hook the same
   `spin_lock_irqsave`/IRQ-entry paths already instrumented for other
   reasons), reported alongside every `profiler stat`/`dump` -- turning
   "some unknown fraction of this run was invisible to sampling" into a
   real number, the same way `perf` itself reports lost/dropped
   samples rather than staying silent about them. Not built yet; a
   reasonable next small addition, distinct from closing the gap itself.
4. **On the target:** the same position applies. It runs Non-secure SVC
   with IRQ-based PMU sampling, so the masked-cycles counter from item 3
   is the practical mitigation there as well. (Superseded 2026-10-05: an
   earlier version planned FIQ-routed sampling on the target, based on
   the incorrect Secure-target premise.)

## Software reboot (2026-09-28)

LK's generic `reboot`/`poweroff` shell commands existed but did nothing
on bcm28xx: the platform never overrode `platform_halt()`, so both fell
through to "HALT: spinning forever". `overlay/lk/0008-bcm28xx-watchdog-reboot.patch`
fixes that with the PM block watchdog, the same sequence Linux's
`bcm2835_wdt` restart handler uses (the only software-reachable
full-SoC reset on these parts):

- **reboot**: `PM_WDOG = PASSWORD | 10` (~150us at 65536 ticks/s), then
  `PM_RSTC.WRCFG = full reset`. PM is at phys `0xFE100000`, inside the
  existing 64MB low-peripheral mapping (VA `0xE2100000`) -- no new
  mapping needed, and it's reachable from non-secure SVC.
- **poweroff**: first sets `PM_RSTS` to partition 63 (`0x555`), the
  firmware's "halt, don't boot" marker, then the same reset. **Built but
  not hardware-tested** -- recovering from it needs a physical
  power-cycle.
- `uart_flush_tx()` was an empty stub in the PL011 driver; it now waits
  for TXFE and !BUSY, so the "Rebooting..." line isn't cut off by the
  reset.
- `platform_halt` is declared `__WEAK` in `platform.h` itself, so both
  this and `platform/power.c`'s default are weak and link order picks
  one -- the same pattern other LK platforms rely on. Confirmed from
  `lk.elf`'s disassembly that the linked copy is the bcm28xx one (it
  passes both hooks to `platform_halt_default`).

After the reset the firmware boots the SD card again, i.e. the serial
chainloader, which prompts `SBOOT?` at 115200 -- so a reboot replaces
the manual power-cycle between test images. `pi4_serial_boot.py` and
`pi4_run.py` take `--reboot`: they listen for `SBOOT?` for 2.5s, and if
it doesn't show (so LK is running), send `reboot` at the LK baud
(3,000,000), drop back to 115200 and load as usual.

**Verified on the real Pi 4B:** after one initial power-cycle, `reboot`
printed `Rebooting, reason 'software reset'` in full and the chainloader
came back by itself; then 3/3 consecutive `pi4_run.py --reboot` cycles
(detect running LK -> reboot -> chainloader banner -> reload -> shell
command) with no power-cycle, ~18s each end to end (the image transfer
at 115200 is most of that). The line can carry one NUL byte during
the reset, harmless but it makes `grep` treat logs as binary (`grep -a`).

A manual power-cycle is still needed when the Pi is hung, when the
running image isn't a TARGET=rpi4 LK build with this patch, or after
`poweroff`.

## UART baud doubled to 6,000,000, no reflash (2026-09-29)

The prior 3,000,000 baud (see UART baud calibration above) depended on
the chainloader's own hardcoded mailbox clock request (48MHz,
`experiments/pi4-serialboot/main.c`) -- only the SD-card-flashed
chainloader could change that, needing a reflash. `overlay/lk/
0011-uart-higher-baud-no-reflash.patch` instead has **LK make its own
mailbox "set clock rate" request at its own boot** (`bcm2711_set_uart_clock()`,
requesting 96MHz, in `platform/bcm28xx/platform.c`), before programming
`IBRD=1`/`FBRD=0` -- the same clean exact-divide relationship as the
original 48MHz/3,000,000 pairing. The chainloader itself is completely
unchanged; raising the baud further only ever needs a new LK build sent
over serial.

IBRD/FBRD are computed generically from whatever clock the firmware
*actually* granted (read back from the mailbox response, not assumed),
with an explicit fallback to the original 48MHz/3,000,000 configuration
if the firmware ever grants less than the new target needs. LK runs
with the MMU and caches already on by this point (unlike the bare
chainloader), so the request buffer needs explicit
`arch_clean_cache_range`/`arch_invalidate_cache_range` and its address
converted to a VideoCore bus address, not passed as a raw kernel
virtual pointer -- getting this wrong would have the VideoCore reading
the wrong physical memory entirely.

**Confirmed on real hardware**: clean boot banner and shell round-trip
at 6,000,000 baud on first attempt, then a 796-sample (~296KB)
`profiler dump` transfer with exactly one `SAMPLE done` marker. (This
transfer was later found, on closer inspection, to have carried 2
corrupted bytes undetected -- see "Dump integrity: seq/crc" below;
noted here rather than silently left as an overstated "zero
corruption" claim.)

**Deliberately not pushed further**: a further doubling to 12,000,000
(192MHz, same clean pattern) was considered and explicitly not
attempted -- 1,000,000 and 1,500,000 baud, also exact divisors, already
failed to sync on this adapter at the old 48MHz clock while 3,000,000
worked, so 12,000,000 is a genuine unknown, not a predictable next
step. 6,000,000 is the settled rate.

## Dump integrity: seq/crc (2026-09-29)

A closer look at real captures (both 3,000,000 and 6,000,000 baud)
found occasional byte corruption with an exact **64-byte period** --
the USB Full-Speed bulk packet size -- clustered in bursts within a
large, continuous, gapless print (a `profiler dump` prints hundreds of
near-identical lines back to back with no pauses). This points to the
host's serial adapter/driver, not this project's own UART timing: a
settling delay right after the baud switch (`switch_baud()`, added to
`scripts/pi4_serial_boot.py`/`pi4_run.py`) was tried first and does fix
a *separate*, real issue right at the switch boundary, but doesn't
touch this one -- the corruption recurs throughout a long burst, at a
variable rate run to run (2 corrupted bytes in one capture, 18 samples'
worth in another with the same workload), consistent with host-side
USB/scheduling jitter rather than something a fixed delay can prevent.

Most such corruption already breaks hex-parseability (a NUL byte isn't
a valid hex digit), which `pi4_pc_histogram.py`'s regex already
silently dropped -- but *silently*, with no way to know a sample went
missing, and no defense at all against a bit-flip landing on another
valid hex digit. `profiler dump` now prints two more fields per line:
- `seq=`: a plain per-dump counter. A gap means a line was lost or
  malformed entirely.
- `crc=`: an FNV-1a-style hash (`profiler_sample_checksum()`) over the
  sample's real binary fields, not the printed text -- catches a
  corrupted-but-still-valid-hex line the regex alone could never see.
  Mirrored exactly on the host in `pi4_pc_histogram.py`'s
  `_sample_checksum()`; cross-checked against an independent
  reimplementation before trusting it, since no local host compiler
  was available to build-and-run the real C function directly.

Either failure drops that one sample -- counted, not silently lost --
same "don't trust it, don't abort the whole report" resilience already
used for a failed unwind (review finding #12). **Verified on real
hardware**: a capture that lost 18 samples to seq gaps (0 crc
mismatches that run) still produced a correct report over the
remaining 782 -- `pi4_pc_histogram.py` printed the exact loss count
instead of the previous unexplained "790 shown vs 800 total" mismatch.

## Code review, Phase 1 fixes (2026-09-28)

A full code review of the project (profiler app, all LK patches, host
scripts) found 15 issues, ranked by severity. Phase 1 -- the ones that
could hang or crash the target outright, not just mislead a report --
is done and verified on real hardware:

- **PMU overflow could hang a core (review finding #1).** `pmustart`/
  `pmustop` only ever touched whichever core happened to run the shell
  command -- its own banked PMU registers plus a single SHARED
  `profiler_pmu_enabled` flag. If they ran on different cores, the
  armed-but-never-disarmed core's next overflow found
  `profiler_on_tick()` already reading "disabled" and returned without
  clearing `PMOVSR`; since the PMU SPI is level-triggered, that refires
  the instant the handler returns and the core never leaves interrupt
  context again. Fixed by making the enabled flag per-core and
  broadcasting arm/disarm to every core via LK's `mp_sync_exec()`, plus
  unconditionally acknowledging any real PMU-overflow vector for a
  core's own index regardless of that flag (defense in depth for the
  transition window). Verified: 4 rapid `pmustart`/`pmustop` cycles
  (including one with an SMP workload running through the middle) with
  no hang, and PMU sampling now genuinely runs on all 4 cores at once
  as a side effect -- 303/304/304/304 samples in one stress run.
- **Per-CPU index could silently disagree with `arch_curr_cpu_num()`
  on a different target (finding #9).** `exceptions.S`'s hand-written
  `bic r3, r3, #0xff000000` was snapshotted from a build where
  `SMP_CPU_ID_BITS` was still 24 (the pre-rpi4 QEMU default); bcm28xx's
  rules.mk sets it to 8, which changes the real formula to plain
  `mpidr & 0xff`. The two only ever agreed on this board because
  BCM2711's MPIDR happens to have zero bits in [23:8] -- a target with
  a real Aff1 field there would index a different per-CPU slot in this
  assembly than the C side reads back, corrupting
  `profiler_fp_r7`/`r11` across cores with no signal anything was
  wrong. Fixed by computing the index in assembly from the exact same
  `SMP_CPU_ID_BITS`/`SMP_CPU_CLUSTER_SHIFT` build macros the C formula
  uses, so it can't drift again regardless of target. Verified via
  disassembly (matches the intended formula exactly on this board) and
  functionally via repeated `profiler smp`/PMU-sampling runs across all
  4 cores with no cross-core corruption.
- **Console print lock could self-deadlock (finding #2).** `out_count()`
  re-took the same non-recursive `print_spin_lock` that `vfprintf()`
  already holds for the whole call (added when 0005 fixed the SMP
  console race) -- harmless until anything registers a print callback,
  at which point the first `printf` after that spins forever on a lock
  its own core already holds. Fixed by removing the now-redundant inner
  lock. Not independently reproducible on real hardware right now:
  nothing in this codebase currently calls
  `register_print_callback()`, so this was a latent bug in the shipped
  code, not one exercised by anything here yet -- confirmed correct by
  inspection (its one caller, `__debug_stdio_write`, is only ever
  reached through `vfprintf()`), not by a live repro.
- **No minimum PMU sample period (finding #3).** `pmustart` only
  rejected `count == 0`; a small reload could make the counter overflow
  again before the interrupt handler -- GIC dispatch, several PMU/GIC
  register accesses, a 128-byte stack memcpy -- finished, leaving a
  core doing nothing but re-entering its own handler. Fixed with a
  conservative floor (`PROFILER_PMU_MIN_COUNT`, 10000) -- not a
  precisely measured threshold, just comfortably above that handler's
  known-nontrivial cost, the same reasoning behind Linux's own
  `perf_event_max_sample_rate`. Verified: `pmustart 8 100` and
  `pmustart 0x11 500` both rejected with a clear message.
- **Event IDs parsed as decimal only (finding #8).** `pmustart`'s
  argument was always read via decimal-only parsing, despite the
  command's own usage text showing hex event IDs -- `pmustart 11`
  silently programmed decimal 11 (0x0B) instead of the intended
  CPU_CYCLES (0x11), with no warning since the A72 implements both.
  Fixed via `strtoul(..., 0)`, which auto-detects a leading `0x`.
  Verified: `pmustart 11 ...` now reports arming event `0xb`, and
  `pmustart 0x11 ...` reports `0x11` -- distinct and correct.
- **`profiler fpcheck` could crash the Pi (finding #15).** Dereferenced
  whatever a core's last-captured r7/r11 happened to hold; at `-O2`
  those are ordinary registers, so a garbage value (e.g. a loop
  counter) pointed at unmapped memory and the resulting data abort
  hung the board. A leftover from the pre-DWARF-CFI, frame-pointer
  stage with no use now. Removed.

Phases 2-4 (report correctness, all-core-by-design sampling, the
self-describing dump format, pre-A55-port hardening) are tracked but
not started -- see the plan from this review for the full breakdown.

**Phase 2, must-fix subset done (2026-09-28).** Of Phase 2's items,
three were judged must-fix (an existing report can be wrong or
silently incomplete without them, not just less convenient) and are
now fixed and verified on real hardware; the rest of Phase 2 is
deferred as should-fix, not must:

- **Reports/folded output grouped by exact instruction, not function
  (finding #4).** `symbolize()` baked the raw offset and PC into every
  frame name, so two samples in the same hot function almost never
  produced the same string -- a real capture of 1203 samples all
  inside `profiler_workload_inner` used to scatter across ~1203
  near-0% rows in the flat report, and the folded FlameGraph output
  almost never merged two samples into the same stack either. Fixed by
  making `symbolize()` return the function name alone; per-instruction
  detail is still `--annotate`'s job, not the flat report's. Also
  cached the ELF symbol table (`_load_symtab()`, keyed by path+mtime)
  -- `symbolize()`/`find_function()` each used to reopen and rebuild
  it from scratch on *every* call, measured by the review at ~88ms/call,
  tens of minutes for a full `--folded`/`--annotate` pass. Verified: the
  1203-sample capture above now reports one `profiler_workload_inner`
  row at 99.0%; a second real capture of 2400 samples reports a clean
  50.0%/50.0% split matching `bench`'s known alternation.
- **`pi4_run.py` silently truncated long-running commands (finding
  #7).** It waited for `--idle` seconds of silence, capped at
  `--max-wait` (15s default) -- too short for a `profiler bench` that
  runs for tens of seconds with no output at all, let alone the dump
  afterward, and then typed the *next* command straight into LK's
  still-busy 16-byte UART receive buffer. Fixed by waiting for LK's
  own `"] "` shell prompt (`lib/console/console.c` prints exactly this
  before reading the next line) instead of a silence heuristic;
  `--max-wait` is now a much larger (90s) hard timeout for a genuine
  hang, not the normal completion signal. Verified: `profiler bench
  900000000` ran for ~24s (already past the old 15s ceiling) with no
  timeout, and the following `profiler dump` captured all 2400
  samples, exactly matching `profiler status` before and after --
  zero truncation.
- **One bad sample aborted the entire report (finding #12).** A CFA
  rule needing a register (r7 or r11) that this specific sample didn't
  capture -- only whichever one matched the *interrupted* PC's own
  Thumb/ARM mode is ever recorded, see `profiler.c:340` -- raised
  `ValueError` straight out of the per-sample loop and discarded every
  other sample with it. Fixed by catching the failure per-sample and
  falling back to a leaf-only chain, so one bad sample only costs its
  own multi-frame detail. Verified with a genuinely forced failure (a
  real mixed-Thumb/ARM CFI scenario reusing `test_dwarf_unwind.py`'s
  own `thumb_edge.elf` fixture, not a mock) alongside a normal sample
  in the same run: the bad one degrades to depth 1 with a printed
  warning, the good one still unwinds fully to depth 3, and the script
  exits 0 either way.

## Outstanding work, by priority (2026-09-28)

> **Superseded as the live backlog (2026-10-05):** open items now live in
> [HANDOFF.md](HANDOFF.md#claims-consolidated-todo) (IDs K1–K9). This section
> is kept as the original review record.

Everything below was surfaced by the code review and is tracked but
not yet done, ordered by actual priority -- not the order it was
found in. This supersedes "Next steps, in order" further down (already
superseded once, by the "close to Linux perf" section above); kept
here as the current, single backlog rather than three overlapping
lists. Judgment call behind the ordering: does skipping an item mean
an existing report can be *wrong or silently incomplete*, or just less
capable/convenient. Only the first tier clears that bar.

**Should-fix -- Phase 2's remainder (not must, but real bugs):**
- **No CFI fallback for assembly with no unwind data (finding #13).**
  None of LK's hand-written `.S` (memcpy/memset/bcopy/bzero, context
  switch, spinlocks) carries `.debug_frame` rows, so time spent there
  shows as a disconnected single frame with no caller -- that
  caller's own time is undercounted. Fix: fall back to the raw LR as
  the caller's PC when `_find_fde` finds nothing, instead of stopping.
- **Old and new capture runs mix in one log (finding #6).** `--log`
  opens in append mode and `pi4_pc_histogram.py` counts every `SAMPLE`
  line in the file with no way to tell one run from another. Fix: a
  run-id (boot timestamp or counter) on each `SAMPLE` line;
  `pi4_pc_histogram.py` filters to the latest by default.
- **`profiler stat` counts its own `printf` calls (finding #10).** The
  counters start before two UART prints and stop after reading them,
  so `stat`'s own console output is folded into the measured cycles.
  Fix: print all setup output, *then* start counting.
- **No labelling on shared counters/buffers (finding #11).** `stat`
  silently reprograms counter 0 out from under an active `pmustart`
  session, and timer/PMU samples share one buffer with no field
  saying which mode produced which sample. Fix: tag each sample with
  mode + event id; make `stat` refuse to run over an active
  `pmustart`.
- **`setup.sh` isn't idempotent (finding #14).** A second run on the
  same `build/lk` fails on a file patch 0004 leaves behind
  (`gic.h`) that a scoped `git reset --hard` doesn't clean up. Fix:
  `git clean -fd` scoped to that one path before the reset.

**Capability expansion -- Phase 3 (not fixes, real new capability):**
1. All-core cycle sampling as the *default* mode, not just something
   `pmustart` happens to now do on every core as a side effect of the
   Phase 1 fix -- tag each sample by CPU as the standard path, the
   real analogue of `perf record -a`.
2. A self-describing dump format (build-id, per-CPU sample counts,
   run-id header/footer) -- needed both for host-side integrity
   checking and for the eventual target's Trace32 memory-dump
   extraction path.
3. A real `profiler stat`: any command (not just the built-in loop),
   all 6 counters at once, derived IPC.
4. Cap or redesign stack capture: bound the 128-byte copy to each
   thread's actual stack instead of a fixed guess, or build the
   on-target unwinder `docs/DESIGN.md` originally proposed so only
   return addresses ever leave the device.

**Deferred by design -- Phase 4 (target-specific, premature now):**
- Finalize the buffer-format/memory-budget tradeoff from Phase 3
  against the real target's RAM constraints, once they're known.
- Re-verify `test_dwarf_unwind.py`'s hardcoded GCC-10.3 addresses
  against whatever toolchain actually builds for the target.

**Lower priority, fold into whichever commit next touches the same
file rather than doing as standalone work:**
- Stale docs/help text: README's leftover "M5 not yet implemented"
  claim, `CLAUDE.md`'s "Current task" section, the `pmu` command's
  own message, and the dangling `[[project_lk_perf_no_fpu_neon]]`
  memory-link reference at this file's own line ~1072 (open-risks
  section).
- `resolve_lines()` can return no line for the very start of a
  function when two `.debug_line` entries share an address.
- `docs/DESIGN.md` claims per-core state sits on separate cache
  lines; `profiler_head`/`profiler_total` and the r7/r11 arrays
  actually share lines across cores.
- `profiler dump` prints each stack byte with its own `printf` call
  and has no per-line checksum.
- The PMU interrupt handler assumes counter 0 is still the selected
  counter, rather than checking.

## Why Pi 4B over the Arm Cortex-A55 FVP route

The FVP was investigated first and set aside for concrete, confirmed
reasons, not preference:

- FVP's Base Platform models use **GICv3**; all three of this project's
  overlay patches (`0001`-`0003`) target GICv2 (`gic_v2.c`). Would need a
  full GICv3-equivalent rewrite.
- FVP boots Cortex-A55 (AArch64-capable) in AArch64 at EL3 by default.
  Getting to AArch32 EL1 needs a real secure-world boot chain — normally
  Trusted Firmware-A built with `ARCH=aarch32` — which doesn't exist in
  this project at all today. LK's QEMU boot path never needed this,
  since `-cpu cortex-a15` is AArch32-only hardware with no state to
  transition from.
- The FVP download itself is blocked by Akamai's bot-protection WAF on
  both `developer.arm.com` and `support.arm.com` — confirmed via direct
  curl with full browser headers (HTTP 403, Akamai edge reference),
  not just a login wall. Needs a real, human browser session.
- Free-tier FVP PMU event fidelity (vs. the licensed "Cycle Models"
  tier bundled with Arm Development Studio) was never confirmed either
  way.

**Pi 4B's concrete advantage**: BCM2711 uses a real **GIC-400, which
implements GICv2** — confirmed via BCM2711 device tree source
(`arm,gic-400` compatible, GICv2 architecture). This project's existing
GIC hook patches have a real shot at applying with much less rework.
And Raspberry Pi's own vendor firmware solves the AArch32 boot-state
question via a plain `config.txt` setting (`arm_64bit=0`) — no TF-A
integration needed, unlike the FVP.

## Update 2026-09-26: first hardware milestone passed (steps 1-2 done)

Moved to the home PC, which has the SD card and serial adapter attached and
can write to the card normally. `experiments/pi4-baremetal/kernel7l.img`
booted on the real Pi 4B, and the serial console showed the banner and a
steadily incrementing heartbeat, with one boot and no reset loop:

```
pi4-baremetal: AArch32 boot OK, PL011 UART live
core: 0 (others parked)
heartbeat 0
heartbeat 1
...
```

Things found along the way that the plan below didn't anticipate:

- **PL011 is routed to Bluetooth on Pi 4 by default.** `enable_uart=1`
  alone puts the *mini-UART* on GPIO14/15, so the original image would have
  booted and printed nothing. The fix is applied twice: `dtoverlay=disable-bt`
  in `config.txt`, and `main.c` now muxes GPIO14/15 to ALT0 and clears their
  pulls itself (BCM2711 `GPIO_PUP_PDN_CNTRL_REG0`, not the BCM283x
  `GPPUD` sequence). An LK `rpi4` target needs the same pin setup.
- **The firmware normally enters only core 0, in HYP mode.** The 32-bit
  armstub keeps cores 1-3 spinning on their ARM-local mailbox 3 (so it's
  not PSCI). That answers the secondary-release question under "Open risks"
  below, and it means LK's entry path has to drop from HYP to SVC. Both
  still need confirming on hardware during the LK port.
- **PL2303TA serial adapters are blocked on Windows 11.** Prolific's
  3.9.6.0 and 3.8.43.0 drivers both refuse the chip at runtime. Driver
  3.8.28.0 (Oct 2018, Microsoft Update Catalog, Microsoft-signed) works.
  Windows Update may upgrade it again later.
- The card actually holds Raspberry Pi OS **2021-05-07** (full image, boot
  label `boot`), not a current Lite image. Its firmware boots this board,
  but a newer board revision (1.5+) would need newer firmware.
- The original files are kept on the card as `kernel7l-linux-backup.img` and
  `config-linux-backup.txt`.

**Serial chainloader working (same day).** `experiments/pi4-serialboot/` is
now the `kernel7l.img` on the card. Images are sent over the console cable
with `scripts/pi4_serial_boot.py`, so the SD card no longer moves. On first
use it loaded `pi4-baremetal` (682 bytes, CRC OK, ~14 KiB/s) and the
heartbeat came up. Its banner confirmed two things on real hardware:

- **Entry mode is HYP** (`mode=HYP`), so the LK port does need the
  HYP->SVC drop.
- **The firmware's DTB is at `0x2eff3b00`** (`r2`), near the top of low RAM
  and nowhere near the `0x8000` payload region. The loader's DTB-move path
  wasn't needed. `r1=0xc42` is the device-tree machine type.

**Next up: step 3 (LK `TARGET=rpi4` port). The detailed plan is in the
next section.**

## Start here: state at end of 2026-09-26 and the LK port plan

This section is the handoff for picking the work up on another machine.
Everything below the next heading is history.

### Resuming with Claude Code

Clone or pull the repo, start `claude` in its root, and paste the prompt
below. The session also loads `CLAUDE.md` automatically, which has the
working rules: RunPod handling, ask-first actions, commit identity and
environment gotchas.

```
Continue the Raspberry Pi 4B bring-up of lk-perf on this PC; the Pi
and its USB-serial adapter are plugged in here. Read CLAUDE.md and
the "Start here" section of docs/RPI4_BRINGUP.md first.
1. Prove the serial path: pip-install pyserial, run
   scripts/pi4_doctor.py --no-pi, then --boot-test (tell me when to
   power-cycle the Pi). Fix failures from the doc's setup table, asking
   me before driver installs or admin actions. Don't continue until
   the heartbeat shows.
2. Set up the Linux build box: I'll create a RunPod CPU pod. Give me
   this PC's SSH public key to add, wait for my "SSH over exposed TCP"
   command, then clone the repo there, run ./setup.sh and confirm the
   `rpi4-test` baseline builds cleanly (`cd build/lk && make rpi4-test
   -j$(nproc)`).
3. Start the LK TARGET=rpi4 port per the doc's step 3 plan, as
   overlay/lk/0004-bcm28xx-add-rpi4.patch, single-core first, aiming
   for M1 then M2. Verify the GIC IDs and the boot-args symbol against
   the sources as the doc says. Commit and push at each milestone.
```

If the new machine runs Linux, the Windows driver steps don't apply, and
it can build LK itself with `./setup.sh`, so step 2 needs no RunPod.

### What exists and works (all verified on the real Pi 4B)

| Piece | Where | State |
|---|---|---|
| SD card | in the Pi | holds `experiments/pi4-serialboot/kernel7l.img` plus `config.txt` with `arm_64bit=0`, `enable_uart=1`, `dtoverlay=disable-bt`. The original Linux kernel and config are on the card as `-linux-backup` copies. Only needs touching again to change the bootloader or `config.txt`. |
| Serial chainloader | `experiments/pi4-serialboot/` | Boots from the card and prints `SBOOT?` once a second. Loads images to `0x8000` over the console cable with a CRC-32 check. |
| Host sender | `scripts/pi4_serial_boot.py` | `python scripts/pi4_serial_boot.py <image> --log <file>` (port auto-detected), then power-cycle the Pi. Needs pyserial. Close PuTTY first. Check a new machine with `scripts/pi4_doctor.py`. |
| Test payload | `experiments/pi4-baremetal/kernel7l.img` | Known-good image for checking the whole path: banner plus heartbeat. |
| Confirmed hardware facts | | Entered in **HYP**. `r0=0`, `r1=0xc42`, **DTB at `r2=0x2eff3b00`**. PL011 at `0xFE201000` works at 115200 on GPIO14/15 (ALT0). Generic timer counts (CNTFRQ is set by the firmware). |

### Setting up the next machine

**Run the checker first.** It goes through every prerequisite below in
order, stops at the first problem, and prints the fix:

```
python -m pip install pyserial
python scripts/pi4_doctor.py --no-pi                  # PC side only, no Pi needed
python scripts/pi4_doctor.py --boot-test              # then power-cycle the Pi when it says so
```

`--boot-test` sends the test image through the chainloader and waits for
its heartbeat. If that passes, the machine is ready: the port, driver,
wiring, SD card and chainloader all work. The port is detected
automatically; pass `--port COMx` or `--port /dev/ttyUSB0` if more than
one USB-serial adapter is plugged in. `pi4_serial_boot.py` detects the
port the same way.

| Symptom | Cause | Fix |
|---|---|---|
| `pyserial is required` | pyserial missing | `python -m pip install pyserial` |
| no adapter, or `can't open COM7` | adapter not plugged in, or it has a different COM number on this PC (it was COM7, later COM8, even on the same PC) | plug it in; let `--port` auto-detect or pass the right one |
| adapter "PL2303TA DO NOT SUPPORT WINDOWS 11 OR LATER", no COM port | Windows 11 driver block | driver 3.8.28.0 steps below (3.8.43.0 still blocks it) |
| `Access is denied` / busy | PuTTY or another terminal holds the port | close it |
| Linux `Permission denied` on `/dev/ttyUSB0` | not in `dialout` | `sudo usermod -aG dialout $USER`, log in again |
| waits forever at `waiting for the chainloader` | Pi not power-cycled, or still running an earlier image | power-cycle *after* starting the script |
| still nothing after a power-cycle | wiring or SD card | crossed RX/TX (below), common GND, card holds `pi4-serialboot/kernel7l.img` plus the three `config.txt` lines |
| text arrives but no `SBOOT?` | card holds some other kernel | put `experiments/pi4-serialboot/kernel7l.img` on it |
| garbage characters | wrong adapter type or bad ground | 3.3V TTL adapter (not RS-232), check GND |

The details behind each row:

1. `git clone https://github.com/shadowfax80/lk-perf.git` (or `git pull`).
2. **The serial side runs on whichever PC the Pi's USB-serial cable is
   plugged into.** That PC needs Python with `pyserial`. On Windows 11 the
   PL2303TA adapter also needs Prolific driver **3.8.28.0**, because newer
   ones refuse the chip (see the 2026-09-26 update above). You can tell
   it's blocked when Device Manager names the adapter "PL2303TA DO NOT
   SUPPORT WINDOWS 11 OR LATER" and no COM port appears. The fix, from an
   elevated PowerShell:
   - Download the Microsoft-signed package (Update Catalog, "Prolific -
     Ports - 3.8.28.0"):
     `https://catalog.s.download.windowsupdate.com/d/msdownload/update/driver/drvs/2019/04/959b8377-7fe8-4ac6-8893-6a3e95b0e8fe_c88b1f07c9075bbfa998a67df0511e2e6d98af10.cab`
     (its SHA1 is the hex after the `_`).
   - Unpack it: `expand driver.cab -F:* .`
   - Install it: `pnputil /add-driver ser2pl.inf`.
   - `pnputil /enum-drivers`, then delete every *newer* Prolific
     `ser2pl` package with `pnputil /delete-driver oemNN.inf /uninstall`.
     Windows always prefers the newest version, so leaving one behind
     means it wins again.
   - `pnputil /remove-device <USB\VID_067B&PID_2303\...>` then
     `pnputil /scan-devices`. The adapter comes back as "Prolific
     USB-to-Serial Comm Port (COMn)".

   Windows Update may later re-upgrade the driver; the same steps undo
   that. Wiring: adapter
   RX to GPIO14 (pin 8), adapter TX to GPIO15 (pin 10), GND to pin 6.
   115200 8N1.
3. **The build runs on Linux: a RunPod CPU pod, WSL or any Linux box.** The
   repo's `setup.sh` expects apt: it installs `gcc-arm-none-eabi`,
   `pyelftools` (for `scripts/dwarf_unwind.py`), clones upstream LK into
   `build/lk`, and applies `overlay/lk/*.patch`. On RunPod, use the REST
   API (`POST https://rest.runpod.io/v1/pods` with `computeType: "CPU"`,
   `cpuFlavorIds: ["cpu3g"]` -- **not** the GraphQL `podFindAndDeployOnDemand`
   mutation, which doesn't support CPU-only pods cleanly and will
   silently deploy an expensive GPU pod instead if `computeType` is
   omitted) to start the cheapest CPU pod, then SSH in directly.
4. Sanity check before any port work: on the build box, `./setup.sh`, then
   `cd build/lk && make rpi4-test -j$(nproc)`. This confirms upstream LK
   tip plus the overlay still build, so later failures are really about
   the port. There's no QEMU boot step any more -- real verification
   happens by sending the image to the actual Pi over serial
   (`scripts/pi4_serial_boot.py`).

### The round trip once the port builds

```
# build box
cd build/lk && make rpi4-test -j$(nproc)            # -> build-rpi4-test/lk.bin
# serial PC
scp -P <port> root@<pod-ip>:lk-perf/build/lk/build-rpi4-test/lk.bin .
python scripts/pi4_serial_boot.py lk.bin --log lk-rpi4.log      # port auto-detected
# then power-cycle the Pi -- or, if a TARGET=rpi4 LK image is already
# running, add --reboot and no power-cycle is needed (see "Software reboot")
```

`lk.bin` is the raw image LK builds next to `lk.elf`, linked to run at
`KERNEL_LOAD_OFFSET`, which must stay `0x8000` for the chainloader. Keep
`lk.elf` on the build box for `addr2line` and `objdump` when something
crashes. At the measured ~14 KiB/s, a few-hundred-KB image takes 20-30s.

### Step 3 plan: LK `TARGET=rpi4` (AArch32)

Upstream LK checked 2026-09-26: `platform/bcm28xx` supports `rpi2` (BCM2836,
AArch32) and `rpi3` (BCM2837, arm64) only. What's already there and
directly usable:

- **HYP to SVC is already handled.** `arch/arm/arm/start.S` checks for mode
  `0x1a` and calls `arm32_hyp_to_svc` when `ARM_WITH_HYP=1`, which
  `ARM_CPU := cortex-a15` sets. Use `cortex-a15`, the same as the QEMU
  project, so the profiler patches see the same arch code. A72 runs
  ARMv7-A code fine in AArch32.
- **Secondary-core release already matches Pi 4.** The rpi2 path writes the
  entry address to `ARM_LOCAL_BASE + 0x8c + 0x10*i`, which is ARM-local
  mailbox 3, exactly what the Pi 4 armstub spins on. Only `ARM_LOCAL_BASE`
  moves, to `0xFF800000` physical on BCM2711.
- **The PL011 driver (`uart.c`) is reusable.** `uart_init_early()` only sets
  `CR`; it doesn't program baud, pins or clock. When LK is chain-loaded,
  the chainloader has already left PL011 at 115200 on GPIO14/15, so this
  works as-is. For a direct SD-card boot later, add the GPIO ALT0 mux and
  the 48MHz mailbox clock plus `IBRD=26`/`FBRD=3` from
  `experiments/pi4-serialboot/main.c`.
- `dev/timer/arm_generic`: pass freq `0` so it reads CNTFRQ (54MHz), as
  the rpi3 path does. rpi2 hardcodes 1MHz, which would be wrong here.

What has to change:

1. **Peripheral base and map.** BCM2711 low-peripheral mode puts the main
   peripherals at `0xFE000000`, ARM-local at `0xFF800000` and the GIC-400 at
   `0xFF840000`. Map `0xFC000000`-`0xFFFFFFFF` (64MB) as device memory in
   `mmu_initial_mappings`, e.g. at virt `0xE0000000`. Add a `BCM2711`
   branch in `bcm28xx.h`/`platform.c` instead of editing the `BCM2836`
   one.
2. **Interrupt controller: GIC-400, not `intc.c`.** On Pi 4 the firmware
   enables the GIC (`enable_gic` defaults on), so the legacy BCM2836
   controller that `intc.c` drives isn't where interrupts arrive. For
   `TARGET=rpi4`, drop `intc.c` and add `dev/interrupt/arm_gic` with
   `GIC_VERSION=2`, `GICBASE(0)` = virt of `0xFF840000`,
   `GICD_OFFSET=0x1000`, `GICC_OFFSET=0x2000`. Model the defines on
   `platform/qemu-virt-arm`. Interrupt IDs:
   - non-secure physical timer (CNTPNSIRQ) = PPI 14 = **ID 30**
   - PL011 UART0 = SPI 121 = **ID 153**

   Both come from the BCM2711 device tree, so check them against
   `bcm2711.dtsi` when writing the code. Using `arm_gic`/`gic_v2.c` is also
   what the profiler's patch `0001` hooks, so the overlay ports with no
   GIC rework.
3. **Memory.** Start with `MEMBASE 0`, `MEMSIZE 0x10000000` (256MB),
   `KERNEL_BASE 0x80000000`, `KERNEL_LOAD_OFFSET 0x8000`, the same as rpi2.
   The DTB at `0x2eff3b00` is outside that, so nothing to reserve yet.
   Later: read the real size from the DTB passed in `r2`. LK's arm start
   code saves r0-r3 as boot args; confirm the symbol name (`lk_boot_args`)
   in `start.S`. Don't use the rpi3 code's "DTB at `KERNEL_BASE`"
   assumption, which isn't true on Pi 4.
4. **New files.** `target/rpi4/rules.mk` (`PLATFORM := bcm28xx`), a
   `TARGET=rpi4` block in `platform/bcm28xx/rules.mk`, and
   `project/rpi4-test.mk` (copy of `rpi2-test.mk` with `TARGET := rpi4`).
   Keep all of it as a new patch, **`overlay/lk/0004-bcm28xx-add-rpi4.patch`**,
   so `setup.sh` applies it like 0001-0003 and upstream stays un-forked.
5. **SMP off for the first boot.** Build with `WITH_SMP=0` or
   `SMP_MAX_CPUS=1`, get a single core to the shell, and only then turn on
   the mailbox release.

Milestones, each checked through the chainloader log:

- **M1 banner:** LK's `welcome to lk` banner and init log arrive after
  `CRC OK, jumping to 0x00008000`. If the log stops right after that
  line, LK died before the console came up (MMU or early init). The
  cheapest probe: in `start.S`, before the MMU is enabled, store a
  character to the PL011 data register at `0xFE201000`; the chainloader has
  the UART ready. Move the probe forward to bisect.
- **M2 interrupts:** a shell prompt, typing works (UART RX interrupt via
  GIC ID 153; lines typed into `pi4_serial_boot.py` are forwarded), and
  timer-driven sleep/`threads` behave (GIC ID 30).
- **M3 SMP:** all 4 cores up via mailbox 3, confirmed in LK's boot log or
  shell.
- **M4 memory:** real memory size from the DTB.
- **M5 profiler on hardware:** `project/profiler-rpi4.mk` (the rpi4 target
  plus `app/profiler`; `-g` for DWARF CFI, not `-fno-omit-frame-pointer`
  -- see the DWARF-CFI-unwinding step below), with patches 0001-0003
  checked on this build. **Note:** the old `scripts/pc_histogram.py`
  (removed along with all QEMU references -- it pulled sample ring
  buffers out through QEMU's QMP `pmemsave`, which real hardware doesn't
  have) needs a real, from-scratch replacement: on the Pi the buffers
  have to come out over the serial console, e.g. a shell command that
  hex-dumps them, parsed by a host script. Plan that as part of M5.

After M5 the original plan continues: step 5 (DWARF-CFI unwinding) and
step 6 (real PMU events via `PMCEID0`/`PMCEID1`). They're listed under
"Next steps, in order" below.

### Things that would make iteration faster (optional)

- ~~A `reboot` shell command in LK~~ -- **done 2026-09-28**, see
  "Software reboot" below.
- A faster baud rate in the chainloader and script. The UART clock is
  already 48MHz, so 921600 is `IBRD=3`/`FBRD=16`. This needs a
  bootloader update on the SD card.
- JTAG (`enable_jtag_gpio=1`, GPIO22-27) with an FT2232H adapter and
  OpenOCD, for when M1 dies before any output.

## Status before the 2026-09-26 update

**Physical setup, on the *office* PC (write-blocked, see below):**
- Raspberry Pi 4B, official USB-C PSU, microSD card
- SD card flashed with Raspberry Pi OS Lite (32-bit) via Raspberry Pi
  Imager — this is what supplies correctly-matched firmware files
  (`start4.elf`, `fixup4.dat`, `bcm2711-rpi-4-b.dtb`) rather than
  hand-sourcing them
- USB-to-TTL serial adapter (identifies as "Prolific USB-to-Serial",
  COM5 on the office PC, currently `Status: OK` — flagged as a
  chipset to watch since counterfeit PL2303 chips are common and
  sometimes get blocked by later Windows updates, but working today)
- PuTTY installed (via `winget install PuTTY.PuTTY`), confirmed
  launchable pre-configured for serial/COM5/115200

**Blocked**: writing to the SD card from the office PC failed --
`Copy-Item` reported "The media is write protected." Checked the two
standard native Windows write-block mechanisms
(`HKLM:\SYSTEM\CurrentControlSet\Control\StorageDevicePolicies\WriteProtect`
and the `RemovableStorageDevices` GPO key) -- neither is present, so
it's not the common native Windows policy. Likely either the card
adapter's physical write-lock switch, or a third-party endpoint
security/DLP agent intercepting removable-storage writes at a lower
level (common on managed office machines, and not something to probe
or try to work around on a work machine). **Not yet resolved.**

**Verified this SD card is the correct one** (not a mismatched Android
phone card that was briefly plugged in instead) -- confirmed by drive
label `boot`, FAT32, 256MB, containing exactly the expected Pi
firmware file set.

**Session-portability dead end, confirmed via Claude Code's own guide
agent**: a Claude Code session is machine-specific. "Remote Control"
gives *remote access to the original machine's session* -- execution
still happens there, it does not let a different physical machine
(home PC, wife's PC) get tool access through this same session. The
only real fix is starting a **fresh Claude Code session on whichever
machine actually has write access to the SD card**, which then picks up
full context from this repo + memory without needing this specific
conversation to carry over.

**Validation firmware already written, compiled, and disassembly-verified
-- not yet flashed or boot-tested on real hardware.** Source and
prebuilt binary are both in this repo now (see below) specifically so
they're available via `git pull` on whichever machine ends up with
working SD card access.

## The validation firmware (`experiments/pi4-baremetal/`)

Deliberately the smallest possible thing that proves the whole chain
works -- toolchain -> SD card -> firmware -> AArch32 boot -> UART --
before attempting any LK port:

- `start.S`: reads MPIDR, parks cores 1-3 in a `wfe` loop (Pi 4
  firmware releases all 4 Cortex-A72 cores simultaneously in AArch32
  when `arm_64bit=0`, unlike QEMU which only ever starts core 0 unless
  `-smp` is passed -- this parking is not optional), core 0 sets up a
  stack and branches to `main`.
- `main.c`: brings up **PL011** (not the mini-UART -- its clock is set
  deterministically via one mailbox call to exactly 3MHz, rather than
  depending on an inferred `core_freq` side effect that different
  sources documented inconsistently), computes `IBRD=1`/`FBRD=40` for
  115200 baud from that known 3MHz reference, then prints a boot
  banner and an incrementing heartbeat forever.
- `linker.ld`: flat layout, entry at `0x8000` (the standard physical
  load address Pi firmware expects for a 32-bit kernel image).
- Built with the same `arm-none-eabi-gcc` toolchain already used
  throughout this whole project -- nothing new to install.
- `kernel7l.img` (638 bytes) is the prebuilt output, committed
  directly -- copy it straight onto the SD card's boot partition, no
  rebuild required, though the source is there to rebuild if needed.

Disassembly-verified (not just "it compiled"): MPIDR read -> mask ->
park non-zero cores -> stack setup -> branch to `main`, exactly as
designed.

## Status (2026-09-26)

Steps 1-4 below are done: `TARGET=rpi4` boots on real Pi 4B hardware
over the serial chainloader, all the way to an interactive shell
(`entering main console loop`, `help` command list echoed back over
UART RX). Three real bugs were found and fixed getting there --
`ARM_CPU=cortex-a15` doesn't set `ARM_WITH_HYP` (only `cortex-a7`
does, so the Hyp->SVC drop was silently compiling out), a GICD
mapping-size panic (`GICD_MIN_SIZE` vs the real 4KB register block),
and `arm_gic_init_map()` being called too early -- before the VM
subsystem it depends on (`vmm_alloc_physical`) is initialized, which
was silently corrupting kernel BSS. See `overlay/lk/0004-bcm28xx-add-rpi4.patch`
for the fixes and commit `7bcd92a` for the full writeup.

## M3 (SMP) done (2026-09-27)

`WITH_SMP := 1` for `rpi4`; all 4 cores confirmed alive and scheduling
on real hardware (`threads` command shows idle threads for cores 0-3
all in `run`/`rdy` state, and the shell thread itself has been observed
running on cores other than 0, proving genuine scheduler activity, not
just idling). Secondary-core release uses the same generic BCM28xx
mailbox-3 mechanism `rpi2` already had in `platform_early_init` --
unverified on real hardware before now, Stage 4 of the profiler work
only ever verified SMP on QEMU.

Found and fixed a real, pre-existing bug in shared LK code along the
way: `lib/io/console.c`'s `out_count()` only held `print_spin_lock`
around the registered-logger callback list, not around the
`platform_dputc()` loop that actually writes the UART -- and a single
`printf`/`dprintf` call reaches `out_count()` multiple times (once per
literal segment, once per formatted argument, ...). On SMP this let
concurrent cores' output interleave mid-string, producing garbled
console text (confirmed: `ARM: secondary cpu 3 started` from 3
different cores appeared as `ARM: secondary cpu ARM: secondary cpu
ARM: secondary cpu 132 started`). Fixed by moving the lock to
`vfprintf()`, held across the entire formatted-print call via two new
public functions (`console_print_lock`/`console_print_unlock` in
`lib/io.h`). See `overlay/lk/0005-fix-smp-console-output-race.patch`.

## M4 (real memory size) done (2026-09-27)

Reads the real installed RAM from the DTB the firmware hands off
(`lk_boot_args[2]`, the physical DTB address at kernel entry -- the
standard ARM Linux boot convention this firmware follows, same r2 the
chainloader already treats as a DTB pointer), instead of the hardcoded
256MB `MEMSIZE` guess.

This board turned out to be an **8GB unit** with a genuinely
non-contiguous memory map (confirmed via the real `memory@0` node's
`reg` property, `address-cells=2 size-cells=1`, 4 entries): `[0,
~948MB)` (already excluding the GPU carve-out), then `[1GB, ~4GB)`,
then two 2GB banks above the 4GB boundary. Supporting all 8GB properly
needs LPAE (large physical address extension) and multiple PMM arenas
-- real, but out of scope for "read the real size instead of
guessing". M4 uses only the first, low, `addr==0` entry: confirmed via
`pmm arenas` showing `size 0x3b400000` (994,050,048 bytes, ~948MB) --
already a ~4x improvement over the old 256MB guess, fully addressable
in 32-bit space with no LPAE needed.

Two real gotchas hit along the way, worth remembering for any future
DTB parsing on this platform:
- The real node name is `memory@0`, not `memory` -- BCM2837's existing
  FDT-parsing code (which this was modeled on) does an exact `strcmp`
  that would never match this board's actual DTB either.
- `reg` property layout depends on the DTB's actual `#address-cells`/
  `#size-cells` (2/1 here, giving 12-byte entries), not a fixed 16-byte
  assumption -- read them from `fdt_address_cells()`/`fdt_size_cells()`
  rather than hardcoding.

See `overlay/lk/0004-bcm28xx-add-rpi4.patch` for the fix.

## Next steps, in order

Superseded by "What 'close to Linux `perf`' actually needs, revisited
for the target platform" above -- that section is the current, authoritative
ordering (M5 redesigned with a fuller sample record and on-target
unwinding; unwinder correctness; `profiler stat`; `report`/`annotate`;
PMU-event sampling with the interrupts-masked blind spot called out
explicitly). Kept here only for detail not repeated above:

- DWARF CFI was chosen over ARM's own EXIDX (`.ARM.exidx`/`.ARM.extab`)
  deliberately: the target platform's shipped firmware carries no EXIDX (dropped
  from the production build to save flash/RAM) but does carry DWARF
  CFI in its debug-symbol ELF, the same mechanism Trace32 already uses
  there to unwind crash dumps -- and it's also the closer match to
  real `perf`'s own `--call-graph dwarf` (via `libunwind`). No ARM
  unwind-tables needed (`-funwind-tables` on ARM defaults to
  EHABI/EXIDX) -- just `-g`, which makes GCC emit `.debug_frame`
  independent of whether EXIDX is enabled at all.
- PMU-event sampling groundwork: program a counter to overflow after N
  occurrences of the chosen event by presetting it to
  `0xFFFFFFFF - N + 1`, mirroring `perf record -e <event> -c <N>`.
  Reuse patches 0001-0003's existing capture hook (the
  `profiler_on_tick`-style weak hook stashing register state from the
  interrupted frame) for the PMU IRQ vector too -- same ring buffers,
  same per-CPU indexing. Add a build/boot-time mode selector so timer
  and PMU sampling both remain real, working, selectable options.
  Multi-event multiplexing is explicitly out of scope for the first
  working version.
- Whether the PMU interrupt is a PPI (per-core, like the timer) or a
  per-core SPI needs checking in the DTB's `arm-pmu` node via the same
  `lib/fdt` parsing M4 added -- don't assume either way from a
  datasheet.

## Open risks

- ~~Whether BCM2711's exact PMU implementation differs from what's
  assumed~~ -- **resolved**: `profiler stat` read real
  `PMCEID0=0x7fff0f3f`/`PMCR=0x41023001` (6 event counters) on actual
  hardware, matching a real Cortex-A72's PMU.
- ~~Whether the target platform's real core is even Cortex-A-family~~ -- **settled**:
  The target platform is a multi-core Cortex-A55, no FPU/NEON (see
  [[project_lk_perf_no_fpu_neon]]) -- this is why that fidelity work
  happened. The Pi 4B's A72 remains a different, higher-performance
  core than the real target either way (see "Not A55-representative"
  above) -- that's a permanent, accepted mismatch, not an open risk.
- ~~The interrupts-masked blind spot has no documented position yet~~
  -- **resolved**: see "The interrupts-masked blind spot: a position"
  above. Documented and deliberately not fixed on this hardware, with
  a small, real, cheap follow-up identified (a masked-cycles counter).
  The target runs Non-secure SVC with IRQ sampling (corrected
  2026-10-05), so the same position and follow-up apply there.
- The persistent chainloader's own baud rate needs an SD-card reflash
  to improve, deliberately deferred -- see item 6 above.
