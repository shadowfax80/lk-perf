# Design: bare-metal statistical sampling profiler for LK, AArch32

## Goal

A `perf`-style statistical sampling profiler for LK running in AArch32
state, targeting an eventual real Cortex-A55 SMP system (ARMv8.2-A), with
QEMU (`qemu-system-arm -machine virt -cpu cortex-a15`) as the first,
cheap-to-iterate-on target. Reuses the RunPod/QEMU/QMP infrastructure
already proven in `bolt-aarch32`.

## Hard constraints, stated up front

- **AArch32 execution state**, not AArch64. This rules out **SPE and BRBE
  categorically** — both are architected as AArch64-only; they don't exist
  as a concept while a core executes in AArch32 state, independent of what
  silicon the vendor licensed.
- **No ETM assumed either**, even though ETM (unlike SPE/BRBE) can trace
  A32/T32 execution when present — treat it as absent on the target SoC.
- **No `.ARM.exidx`/`.ARM.extab` assumed present.** Zephyr's own `perf`
  backend (`subsys/profiling/perf/backends/`) has no AArch32 A-profile
  entry at all — only ARM64, Cortex-M (M-profile, `!SMP`), RISC-V, x86 —
  confirming this gap is real, not project-specific.
- **Real SMP**, multiple Cortex-A55 cores. Every LK/QEMU boot this whole
  session (bolt-aarch32) used `-smp 1` — real multi-core LK bring-up is an
  unverified prerequisite this design depends on, not something already
  proven.

Net effect: **fully software-driven sampling**, no hardware trigger, no
hardware metadata capture, no hardware call-stack assist. This is
structurally identical to how Linux `perf` itself falls back when
PEBS/SPE-class hardware isn't available — not a workaround, `perf`'s own
baseline mechanism, cross-compiled for bare metal.

## What's still guaranteed (the baseline to build on)

PMUv3 (cycle counter + configurable event counters via `PMCR`/`PMXEVTYPER`/
`PMXEVCNTR`, CP15) and the ARM generic timer are mandatory architecture
baseline, independent of ETM/SPE/BRBE. `bolt_bench` (bolt-aarch32) already
reads `PMCCNTR`.

## Staged plan

Each stage is independently useful; don't skip ahead.

0. **Pipeline skeleton** (`app/profiler/profiler.c` as committed) — proves
   build → boot → shell command, zero sampling logic.
1. **PC-only histogram.** Periodic timer IRQ → ISR latches PC only → ring
   buffer → QMP `dump-guest-memory` (reuse `dump-bolt-counters.py`'s exact
   mechanism) → host symbolizes and produces a **flat self-time histogram**.
   Honest framing: this is *not* a flame graph — a bare PC sample carries no
   calling-context information, so no amount of offline cleverness can turn
   a PC-only stream into a call stack. It's still directly useful: real
   hot-path identification on a real workload, and a way to choose
   `--funcs-file` candidates for BOLT itself.
2. **PC + LR.** Free to capture (already in the exception frame). Gives one
   extra level of context for near-leaf samples. Caveat, stated plainly:
   LR is not guaranteed to still hold "my caller's return address" once a
   function has made its own calls (AAPCS lets LR be reused as scratch) —
   treat this as a cheap, imperfect upgrade, not ground truth.
3. **Frame-pointer-chain offline unwind — the real target for flame
   graphs**, now that EHABI is ruled out. Build the profiled image with
   `-fno-omit-frame-pointer`. The ISR latches PC/LR (Stage 2) plus FP (r7
   in Thumb, r11 in ARM, picked per-sample from the interrupted SPSR's
   T-bit) via a small additive exceptions.S patch, since the generic IRQ
   entry path doesn't save r4-r11 at all. All unwinding happens
   **offline on the host**: given a QMP memory dump, walk the FP chain by
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

   **The harder, more general finding**: the stack memory this walk reads
   must still belong to a call chain that hasn't returned yet. Once the
   sampled function returns -- which, for a short synthetic test
   workload, is typically well before its own "done" print is even
   visible to the host, since the *next* function called (here, printf,
   built with the same frame-pointer flag) immediately reuses that exact
   stack slot for its own frame -- the recorded fp still points at real
   memory, but that memory's *content* is gone, silently, synchronously,
   inside the guest, before any host-side reaction (even `qmp stop`
   issued the instant a completion marker is observed) can beat it.
   Verified two ways: on-target, `printf`-based readback of the same
   address (going through the exact same shell round-trip) can look
   completely plausible -- another real function's own frame just
   happens to be there -- so a sane-looking result is not by itself proof
   of a correct one. The robust fix, and the one this project's own host
   tooling now uses: don't wait for a completion marker before dumping
   memory at all. Poll a stable, ever-increasing BSS counter
   (`profiler_total`, safe to read via QMP while the guest keeps running,
   unlike reused stack memory) until enough samples exist, then pause the
   whole VM via QMP `stop` while the sampled call chain is still
   genuinely live. This generalizes beyond this test harness: any
   profiling session that reads a walk target's stack after that target
   has already moved on needs the same discipline, not just this repo's
   verification script.
4. **SMP.** See below — mostly bookkeeping around stages 1-3, not new
   unwind logic. **Verified**, not just designed: `-smp 4` boot reaches
   "welcome to lk/MP" with all 4 cores up (confirmed via `threadstats`
   before writing any profiler code), and a concurrent-worker test
   (`profiler smp`, one thread per core) captured independent,
   near-perfectly-balanced per-core sample counts (35 total, 9/9/9/8)
   with the FP-chain walker correctly recovering a 6-level chain on every
   core, merged into one correct folded-stack output. The one thing
   Stage 3 got right "for free" and Stage 4 had to fix for real: the
   exceptions.S FP capture (profiler_fp_r7/r11) was a *single* global
   through Stage 3, fine for one core but a genuine cross-core race once
   multiple PPIs fire concurrently -- fixed by indexing it per-CPU in
   assembly using the *exact* MPIDR-masking instruction
   `arch_curr_cpu_num()` itself compiles to (`bic r3, r3, #0xff000000`,
   read from this build's own `lk.elf.debug.lst`, not re-derived from the
   architecture manual by hand), so the assembly-computed index can never
   disagree with the C-side per-CPU array indexing.
5. **(Later, real hardware only) PMU-event-triggered sampling** instead of
   fixed-period timer — arm the interrupt on a PMU overflow (e.g.
   `L1I_CACHE_REFILL`, `BR_MIS_PRED`) instead of the generic timer. This is
   the "poor-man's SPE" technique real `perf` itself uses on hardware
   without full statistical-profiling support. On **real** Cortex-A55
   silicon (unlike QEMU TCG, which models no cache at all) this would
   actually respond to BOLT's code-layout changes -- the first design in
   this whole line of work that could show a real, hardware-grounded
   before/after number for BOLT's benefit.

   **Confirmed real-hardware-only, two independent ways, not assumed**
   (`profiler pmu` in `app/profiler/profiler.c`):
   1. No PMU interrupt route exists on this QEMU target at all. Dumping
      this exact `qemu-system-arm -machine virt -cpu cortex-a15`
      invocation's own generated device tree
      (`-machine dumpdtb=...` + `dtc`) shows the `pmu {};` node present
      but **empty** -- no `compatible`, no `interrupts` property. There
      is no GIC IRQ number to register an overflow handler against.
   2. PMU coprocessor register access itself is unsafe here, not just
      the interrupt path. Even the single already-public, already-proven
      LK accessor `arch_cycle_count()` (reads PMCCNTR, used throughout
      bolt-aarch32's `bolt_bench`) reliably faults with an "undefined
      abort" the instant it executes on this bare-metal image --
      verified directly by isolating it in its own test build, not
      inferred from the interrupt finding. Real hardware/firmware
      normally clears the NSACR PMU-access trap during a secure-world
      boot stage before handing off to the kernel; this minimal image
      has none. An earlier, more complete version of this command
      (PMCR/PMCEID/PMSELR/PMXEVTYPER register probing, meant to at least
      answer "does this TCG model count cache events at all") hit the
      same fault and was removed rather than shipped in a state that
      panics the target -- see git history if reviving this on a
      firmware-backed target later.

## SMP-specific design

The unwind algorithm itself (stage 3) is per-sample, per-core-independent
-- one shared binary means identical FP-chain conventions everywhere; each
thread's stack is separate memory. SMP is bookkeeping, not new logic:

- **Per-CPU timer.** The generic timer's IRQ is a GIC **PPI** (Private
  Peripheral Interrupt, banked per-core already -- confirmed from
  bolt-aarch32's own boot logs: `Generic timer register irq 27 on cpu 0`).
  Arm it independently on each secondary core at bring-up; no SPI
  routing/affinity work needed.
- **Per-CPU ring buffers, no locking in the ISR.** A shared buffer with a
  lock is disqualified -- taking a lock inside a sampling interrupt is
  exactly the kind of perturbation that corrupts the measurement, and
  reintroduces the atomicity concerns from bolt-aarch32's Thumb-boundary
  counter-stub bug. Each core writes only its own buffer, cache-line
  separated to avoid false sharing.
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
  path once SMP bring-up exists; a subtle asymmetry in a secondary core's
  entry code would show up as core-specific sample corruption, easy to
  misdiagnose as an unwinder bug instead of a bring-up bug.
- **Per-core stack bounds needed by the offline walker.** N cores means N
  live stack regions simultaneously; the host-side walker needs a
  base/size table per CPU (link-time constants from LK's own build) to
  sanity-check "is this FP still in range" per sample -- a single global
  bound (fine in single-core) is wrong in SMP.
- **Visualization: generate both merged and per-core flame graphs** from
  the same dataset -- cheap, just a different group-by when building the
  collapsed-stack input. Merged (all cores aggregated) is what actually
  matches what BOLT's own function/block reordering optimizes for, since
  that's a global layout decision independent of which core executes it.
  Per-core is the diagnostic view ("which core is the bottleneck").
  If LK's SMP scheduler allows thread migration between cores (not yet
  verified against LK's actual scheduler source), track thread ID
  separately from CPU ID too.

**Status**: per-CPU timer/ring-buffers/CNTPCT-timestamp/merged+per-core
output are implemented and verified (`app/profiler/profiler.c`,
`scripts/pc_histogram.py`). Per-core stack-bounds sanity-checking and
thread-migration tracking are **not** implemented -- real gaps, not
overlooked; the plausibility-gated FP-chain walk (Stage 3) already
rejects most corruption without needing stack bounds, which is why this
wasn't blocking, but a bounds table would catch a stricter class of
"looks-valid-but-isn't" corruption that the .text-range check alone
can't.

## What's reused from bolt-aarch32, not reinvented

- QMP `dump-guest-memory` + host-side extraction, the exact mechanism in
  `dump-bolt-counters.py`.
- `$a`/`$t` mapping-symbol ARM/Thumb disambiguation, from
  `fix-kernel-elf-sections.py`.
- The address-translation lookup (virtual address -> offset into a dumped
  RAM blob), from `ram-dump-to-fdata.py`.
- `stackcollapse.py`-style output formatting feeding standard FlameGraph
  tooling (this is literally Zephyr's own `subsys/profiling/perf` output
  convention -- reuse it rather than invent a new format).
- Reference to check an offline FP-chain/EHABI implementation against:
  LLVM's own `libunwind` (`Unwind-EHABI.cpp`) -- not needed for stage 3
  (FP-chain, not EHABI) but worth knowing it exists if EHABI ever becomes
  available on a future SoC revision.

## Open questions / not yet verified

- ~~Real SMP LK bring-up on this target~~ **Verified** (Stage 4): `-smp 4`
  reaches "welcome to lk/MP", all 4 cores show up in `threadstats`, and
  concurrent per-core sampling + FP-chain unwinding works correctly.
- Per-core stack bounds for the offline walker, and thread-migration
  tracking -- not implemented (see SMP-specific design section above).
- ~~Whether QEMU's `cortex-a15` TCG model implements meaningful PMU event
  counters~~ **Moot, confirmed** (Stage 5): PMU coprocessor register
  access itself faults on this bare-metal image (no secure-monitor boot
  stage to clear the NSACR trap), and there's no PMU IRQ route in this
  QEMU target's device tree either way -- see Stage 5 above. Answering
  "does it count cache events meaningfully" needs real hardware or a
  firmware-backed QEMU boot, not this minimal image.
- Whether the eventual real hardware target genuinely lacks ETM (a SoC
  choice) as opposed to it being disabled/fused off -- doesn't change the
  design, worth confirming before spending effort on an ETM path later.
