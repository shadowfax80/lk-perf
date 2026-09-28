# Raspberry Pi 4B bring-up plan

Why this exists: `docs/DESIGN.md`'s Stage 5 (PMU-event-triggered sampling)
was confirmed real-hardware-only on QEMU (no PMU IRQ route in the virt
device tree, and PMU register access itself faults without a
secure-monitor boot stage). Real Cortex-A55/production hardware wasn't
available, so a Raspberry Pi 4B (Cortex-A72, BCM2711) was chosen as the
cheapest real-hardware path to validate two specific things QEMU
categorically cannot: DWARF-CFI unwinding robustness, and whether real PMU
event counters (cache misses, branch mispredicts) actually respond to
workload behavior.

**Scope: this project is a PoC for the the target platform's actual perf
use case**, not a general-purpose ARM32 profiler. the target platform's own shipped
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
   - Not done: a `perf script`-compatible text emitter. Lower priority
     than source lines/annotate turned out to be -- FlameGraph
     consumption already works via the existing folded-stack output;
     revisit only if Firefox Profiler/hotspot import is actually needed.
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
   around 590 KB, ~52s to dump at that rate). the target platform's real path is
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
the target platform has no FPU and no SIMD/NEON unit at all, unlike the A72's own
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

**This is a fact about the Pi validation environment, not a
demonstrated fact about the target platform.** A cellular baseband processor
almost certainly has its own real secure-boot chain, for reasons
unrelated to profiling (DRM, crypto, radio certification) -- unlike
this from-scratch bare-metal Pi bring-up, which never had one. Whether
the target platform's actual boot chain grants its own OS/RTOS the secure access
FIQ or pseudo-NMI sampling would need is a genuinely open question,
not something this project can answer from the Pi side. Don't read
"lk-perf hit this limitation on the Pi" as "the target platform also has this
limitation" -- that would need checking on the real target, separately.

**The position:**
1. Document the blind spot as a known, quantified bias in every report
   from this profiler, not a blocker to using it. Every statistical
   sampling profiler has *some* systematic bias; the useful move is
   making this one visible, not pretending it doesn't exist.
2. Don't attempt FIQ/pseudo-NMI on the Pi. It's not reachable from a
   non-secure boot chain here, and even if it were, the result
   wouldn't transfer to the target platform anyway -- the target platform's own GIC
   configuration, secure boot chain, and RTOS are all different from
   LK-on-Pi, so this would need its own, separate implementation on the
   real target regardless of what happened here. Spending real effort
   defeating a non-secure boot chain on validation hardware, for a
   capability that has to be rebuilt from scratch on the actual target
   anyway, is effort spent on the wrong side of the port.
3. What *is* worth building here, being genuinely cheap and actually
   informative: a running counter of total cycles spent with interrupts
   masked (hook the same `spin_lock_irqsave`/IRQ-entry paths already
   instrumented for other reasons), reported alongside every `profiler
   stat`/`dump` -- turning "some unknown fraction of this run was
   invisible to sampling" into a real number, the same way `perf`
   itself reports lost/dropped samples rather than staying silent about
   them. Not built yet; a reasonable next small addition, distinct from
   trying to close the gap itself.
4. When this work reaches the target platform's real target, the first question to
   check there -- before assuming either way -- is whether its boot
   chain grants secure/TrustZone access an RTOS profiler could use for
   FIQ or priority-based sampling. If yes, that closes this gap for
   real, on the target that actually matters, in a way the Pi never
   could validate.

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
# then power-cycle the Pi
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

- A `reboot` shell command in LK: write the BCM2711 PM watchdog, `PM_RSTC`
  and `PM_WDOG` at peripheral base `+0x100000`. Pi then comes back to
  `SBOOT?` without a physical power-cycle, and the host script could
  trigger it itself.
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
  the target platform is a multi-core Cortex-A55, no FPU/NEON (see
  [[project_lk_perf_no_fpu_neon]]) -- this is why that fidelity work
  happened. The Pi 4B's A72 remains a different, higher-performance
  core than the real target either way (see "Not A55-representative"
  above) -- that's a permanent, accepted mismatch, not an open risk.
- ~~The interrupts-masked blind spot has no documented position yet~~
  -- **resolved**: see "The interrupts-masked blind spot: a position"
  above. Documented and deliberately not fixed on this hardware, with
  a small, real, cheap follow-up identified (a masked-cycles counter)
  and the actual open question (the target platform's own secure-boot access)
  correctly placed on the real target, not this one.
- The persistent chainloader's own baud rate needs an SD-card reflash
  to improve, deliberately deferred -- see item 6 above.
