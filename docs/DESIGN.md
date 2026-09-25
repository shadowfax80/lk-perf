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
   `-fno-omit-frame-pointer`. ISR still only latches PC/SP/LR (kept
   minimal, same discipline as bolt-aarch32's counter-bump-stub atomicity
   lessons). All unwinding happens **offline on the host**: given a QMP
   memory dump, walk the FP chain by reading two words at a fixed offset
   per frame, repeat until FP is zero/out of range. Must check ARM-vs-Thumb
   mode per frame (r7 in Thumb, r11 in ARM per AAPCS) — reuses the same
   `$a`/`$t` mapping-symbol logic already implemented in
   `fix-kernel-elf-sections.py` (bolt-aarch32). Simpler to implement than
   an EHABI bytecode interpreter would have been; the cost moved to one
   deliberate, bounded build-flag tradeoff instead.
4. **SMP.** See below — mostly bookkeeping around stages 1-3, not new
   unwind logic.
5. **(Later, real hardware only) PMU-event-triggered sampling** instead of
   fixed-period timer — arm the interrupt on a PMU overflow (e.g.
   `L1I_CACHE_REFILL`, `BR_MIS_PRED`) instead of the generic timer. This is
   the "poor-man's SPE" technique real `perf` itself uses on hardware
   without full statistical-profiling support. On **real** Cortex-A55
   silicon (unlike QEMU TCG, which models no cache at all) this would
   actually respond to BOLT's code-layout changes -- the first design in
   this whole line of work that could show a real, hardware-grounded
   before/after number for BOLT's benefit.

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

- Real SMP LK bring-up on this target (secondary core release, per-core
  stacks, scheduler runqueues) -- prerequisite, not yet demonstrated here.
- Whether QEMU's `cortex-a15` TCG model implements meaningful PMU event
  counters (`L1I_CACHE_REFILL` etc.) at all -- relevant only once stage 5
  is reached; irrelevant to stages 0-4, which only need the generic timer.
- Whether the eventual real hardware target genuinely lacks ETM (a SoC
  choice) as opposed to it being disabled/fused off -- doesn't change the
  design, worth confirming before spending effort on an ETM path later.
