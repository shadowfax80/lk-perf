# Design: bare-metal statistical sampling profiler for LK, AArch32

## Goal

A `perf`-style statistical sampling profiler for LK running in AArch32
SMP, for finding real hotspots and bottlenecks in bare-metal AArch32
workloads. Currently scoped as a PoC for the the target platform's own
perf use case (see `README.md` and `docs/RPI4_BRINGUP.md`) -- a
standalone tool, not something built to validate or support any other
project.

**Development history note**: the staged plan below (Stages 0-4) was
originally designed and verified on QEMU (`qemu-system-arm -machine
virt -cpu cortex-a15`), starting 2026-09-25 -- a fast, cheap way to
prove the sampling/unwinding pipeline before real hardware access
existed. That QEMU-based development is now historical. This project
has since moved to real Raspberry Pi 4B hardware exclusively; QEMU is
not used or referenced as a target going forward. **`docs/RPI4_BRINGUP.md`
is the current, authoritative status** -- read that for what's actually
running today. This document is kept for the technical design
reasoning behind the staged plan (the unwinding approach, the SMP
bookkeeping, real bugs found and fixed), which remains valid regardless
of which environment first proved it out.

## Hard constraints, stated up front

- **AArch32 execution state**, not AArch64. This rules out **SPE and BRBE
  categorically** — both are architected as AArch64-only; they don't exist
  as a concept while a core executes in AArch32 state, independent of what
  silicon the vendor licensed.
- **No ETM assumed either**, even though ETM (unlike SPE/BRBE) can trace
  A32/T32 execution when present — treat it as absent on the target SoC.
- **Stack unwinding uses DWARF CFI (`.debug_frame`), not ARM's own EXIDX
  (`.ARM.exidx`/`.ARM.extab`).** Chosen deliberately, not for lack of an
  EXIDX implementation: the the target platform's shipped firmware
  carries no EXIDX (dropped from the production build to save flash/RAM)
  but does carry DWARF CFI in its debug-symbol ELF, the same mechanism
  Trace32 already uses there to unwind crash dumps -- matching that here
  means the validated approach actually transfers to the real target. See
  `scripts/dwarf_unwind.py`.
- **Real SMP**, multiple cores. Verified on real Pi 4B hardware (M3 in
  `docs/RPI4_BRINGUP.md`) — not just QEMU's `-smp 4`.

Net effect: **fully software-driven sampling**, no hardware trigger, no
hardware metadata capture, no hardware call-stack assist. This is
structurally identical to how Linux `perf` itself falls back when
PEBS/SPE-class hardware isn't available — not a workaround, `perf`'s own
baseline mechanism, cross-compiled for bare metal.

## What's still guaranteed (the baseline to build on)

PMUv3 (cycle counter + configurable event counters via `PMCR`/`PMXEVTYPER`/
`PMXEVCNTR`, CP15) and the ARM generic timer are mandatory architecture
baseline, independent of ETM/SPE/BRBE.

## Staged plan (historical: Stages 0-4 developed and verified on QEMU)

Each stage is independently useful; don't skip ahead. See the
development history note above -- this section describes work
originally proven on QEMU; current real-hardware status is in
`docs/RPI4_BRINGUP.md`.

0. **Pipeline skeleton** (`app/profiler/profiler.c` as committed) — proves
   build → boot → shell command, zero sampling logic.
1. **PC-only histogram.** Periodic timer IRQ → ISR latches PC only → ring
   buffer → offline extraction → host symbolizes and produces a **flat
   self-time histogram**. Honest framing: this is *not* a flame graph — a
   bare PC sample carries no calling-context information, so no amount
   of offline cleverness can turn a PC-only stream into a call stack.
   It's still directly useful for real hot-path identification on a real
   workload.
2. **PC + LR.** Free to capture (already in the exception frame). Gives one
   extra level of context for near-leaf samples. Caveat, stated plainly:
   LR is not guaranteed to still hold "my caller's return address" once a
   function has made its own calls (AAPCS lets LR be reused as scratch) —
   treat this as a cheap, imperfect upgrade, not ground truth.
3. **Frame-pointer-chain offline unwind — the original target for flame
   graphs**, before the pivot to DWARF-CFI-based unwinding (see the DWARF
   CFI constraint above). Build the profiled image with
   `-fno-omit-frame-pointer`. The ISR latches PC/LR (Stage 2) plus FP (r7
   in Thumb, r11 in ARM, picked per-sample from the interrupted SPSR's
   T-bit) via a small additive exceptions.S patch, since the generic IRQ
   entry path doesn't save r4-r11 at all. All unwinding happens
   **offline on the host**: given a memory dump, walk the FP chain by
   reading two words at a fixed offset per frame ([fp+0]=caller's saved
   fp, [fp+4]=saved return address, confirmed via objdump against this
   toolchain's actual Thumb `-fno-omit-frame-pointer` prologue, not
   assumed), repeat until FP is non-increasing or the candidate return
   address falls outside the ELF's own `.text` range. That range check is
   load-bearing, not defensive fluff: not every such frame has a valid
   `[fp+4]` slot at all -- GCC only spills `{fp, lr}` as a pair when it
   actually needs to (a real call to preserve lr across, or register
   pressure in an otherwise-leaf loop); a pure leaf with neither keeps lr
   live and returns via `bx lr` instead, pushing `{fp}` alone, so
   `[fp+4]` there is unrelated stack content, not a return address --
   confirmed via objdump on this project's own two-leaf-function bench
   workload. Implemented Stage 2's LR read as the fallback for exactly
   this case, not a redundant leftover.

   **The harder, more general finding, still relevant to any unwinder
   including the current DWARF-CFI one**: the stack memory a walk reads
   must still belong to a call chain that hasn't returned yet. Once the
   sampled function returns -- which, for a short synthetic test
   workload, is typically well before its own "done" print is even
   visible to the host, since the *next* function called (here, printf,
   built with the same frame-pointer flag) immediately reuses that exact
   stack slot for its own frame -- the recorded fp still points at real
   memory, but that memory's *content* is gone, silently, synchronously,
   before any host-side reaction can beat it. Verified two ways:
   on-target, `printf`-based readback of the same address (going through
   the exact same shell round-trip) can look completely plausible --
   another real function's own frame just happens to be there -- so a
   sane-looking result is not by itself proof of a correct one. The
   robust fix: don't wait for a completion marker before dumping memory
   at all. Poll a stable, ever-increasing BSS counter (`profiler_total`)
   until enough samples exist, then capture memory while the sampled
   call chain is still genuinely live. This generalizes beyond this
   specific test harness: any profiling session that reads a walk
   target's stack after that target has already moved on needs the same
   discipline.
4. **SMP.** Mostly bookkeeping around stages 1-3, not new unwind logic.
   **Verified on QEMU at the time**: `-smp 4` boot reached "welcome to
   lk/MP" with all 4 cores up (confirmed via `threadstats` before writing
   any profiler code), and a concurrent-worker test (`profiler smp`, one
   thread per core) captured independent, near-perfectly-balanced
   per-core sample counts (35 total, 9/9/9/8) with the FP-chain walker
   correctly recovering a 6-level chain on every core, merged into one
   correct folded-stack output. Independently re-verified on real Pi 4B
   hardware since (M3 in `docs/RPI4_BRINGUP.md`) -- all 4 cores confirmed
   alive and scheduling via the `threads` shell command. The one thing
   Stage 3 got right "for free" and Stage 4 had to fix for real: the
   exceptions.S FP capture (profiler_fp_r7/r11) was a *single* global
   through Stage 3, fine for one core but a genuine cross-core race once
   multiple PPIs fire concurrently -- fixed by indexing it per-CPU in
   assembly using the *exact* MPIDR-masking instruction
   `arch_curr_cpu_num()` itself compiles to (`bic r3, r3, #0xff000000`,
   read from this build's own `lk.elf.debug.lst`, not re-derived from the
   architecture manual by hand), so the assembly-computed index can never
   disagree with the C-side per-CPU array indexing.

## PMU-event-triggered sampling (real hardware only)

Instead of a fixed-period timer, arm the sampling interrupt on a PMU
counter overflow (e.g. `L1I_CACHE_REFILL`, `BR_MIS_PRED`) -- the
"poor-man's SPE" technique real `perf` itself uses on hardware without
full statistical-profiling support. This is real, hardware-grounded
sampling behavior; QEMU's TCG model doesn't implement meaningful cache
timing at all, which is why this was always going to need real
silicon, confirmed two independent ways during the QEMU-era design work:

1. No PMU interrupt route existed on the QEMU target used at the time.
   Dumping that exact `qemu-system-arm -machine virt -cpu cortex-a15`
   invocation's own generated device tree showed the `pmu {};` node
   present but **empty** -- no `compatible`, no `interrupts` property.
   There was no GIC IRQ number to register an overflow handler against.
2. PMU coprocessor register access itself was unsafe there, not just the
   interrupt path. Even the single already-public, already-proven LK
   accessor `arch_cycle_count()` (reads `PMCCNTR`) reliably faulted with
   an "undefined abort" the instant it executed on that bare-metal image
   -- real hardware/firmware normally clears the NSACR PMU-access trap
   during a secure-world boot stage before handing off to the kernel;
   that minimal QEMU image had none.

Current status and concrete implementation plan for real hardware: see
"PMU event validation" and "PMU-event-driven sampling" in
`docs/RPI4_BRINGUP.md`'s "Next steps, in order".

## SMP-specific design

The unwind algorithm itself is per-sample, per-core-independent -- one
shared binary means identical unwind conventions everywhere; each
thread's stack is separate memory. SMP is bookkeeping, not new logic:

- **Per-CPU timer.** The generic timer's IRQ is a GIC **PPI** (Private
  Peripheral Interrupt, banked per-core already). Arm it independently on
  each secondary core at bring-up; no SPI routing/affinity work needed.
- **Per-CPU ring buffers, no locking in the ISR.** A shared buffer with a
  lock is disqualified -- taking a lock inside a sampling interrupt is
  exactly the kind of perturbation that corrupts the measurement. Each
  core writes only its own buffer, cache-line separated to avoid false
  sharing.
- **Tag every sample with CPU ID and a timestamp.**
  **Use `CNTPCT` (the generic timer's counter), not `PMCCNTR`, for the
  timestamp** -- this is the one correction worth flagging explicitly.
  `PMCCNTR` is a per-core PMU cycle counter, free-running independently
  since each core's own reset, with no architectural guarantee of
  cross-core phase alignment. `CNTPCT` is specifically architected as a
  single, SoC-wide **coherent** time base, visible identically to every
  core -- that's its designed purpose. Using `PMCCNTR` to interleave
  samples from different cores would silently produce a wrong merged
  ordering. This only affects the *merge/visualization* step; any single
  sample still unwinds correctly regardless of which counter tagged it.
- **Correctly capture the *interrupted* context**, not the ISR's own live
  registers -- from the exception frame, before the ISR's own prologue
  runs. Must be verified independently on every core's own exception-entry
  path; a subtle asymmetry in a secondary core's entry code would show up
  as core-specific sample corruption, easy to misdiagnose as an unwinder
  bug instead of a bring-up bug.
- **Per-core stack bounds needed by the offline walker.** N cores means N
  live stack regions simultaneously; the host-side walker needs a
  base/size table per CPU (link-time constants from LK's own build) to
  sanity-check "is this recovered frame still in range" per sample -- a
  single global bound (fine in single-core) is wrong in SMP.
- **Visualization: generate both merged and per-core flame graphs** from
  the same dataset -- cheap, just a different group-by when building the
  collapsed-stack input. Merged (all cores aggregated) gives a global,
  code-layout-wide view of hotspots, independent of which core executes
  them. Per-core is the diagnostic view ("which core is the bottleneck").
  If LK's SMP scheduler allows thread migration between cores (not yet
  verified against LK's actual scheduler source), track thread ID
  separately from CPU ID too.

**Status**: per-CPU timer/ring-buffers/CNTPCT-timestamp/merged+per-core
output were implemented and verified on QEMU during Stage 4
(`app/profiler/profiler.c` plus the since-removed QEMU-only
`pc_histogram.py`). Per-core stack-bounds sanity-checking and
thread-migration tracking are **not** implemented -- real gaps, not
overlooked; the plausibility-gated FP-chain walk (Stage 3) already
rejects most corruption without needing stack bounds, which is why this
wasn't blocking, but a bounds table would catch a stricter class of
"looks-valid-but-isn't" corruption that the .text-range check alone
can't.

## Techniques reused from prior related work

- `$a`/`$t` mapping-symbol ARM/Thumb disambiguation.
- Address-translation lookup (virtual address -> offset into a dumped
  RAM blob).
- `stackcollapse.py`-style output formatting feeding standard FlameGraph
  tooling (this is literally Zephyr's own `subsys/profiling/perf` output
  convention -- reuse it rather than invent a new format).

## Open questions / not yet verified

- ~~Real SMP LK bring-up~~ **Verified twice**: on QEMU during Stage 4
  (`-smp 4` reaches "welcome to lk/MP", concurrent per-core sampling +
  FP-chain unwinding works correctly), and independently on real Pi 4B
  hardware since (M3 in `docs/RPI4_BRINGUP.md`).
- Per-core stack bounds for the offline walker, and thread-migration
  tracking -- not implemented (see SMP-specific design section above).
- Whether the eventual real hardware target genuinely lacks ETM (a SoC
  choice) as opposed to it being disabled/fused off -- doesn't change the
  design, worth confirming before spending effort on an ETM path later.
