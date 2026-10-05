# Claude ↔ Codex handoff (lk-perf)

Two agents work on this repo, **Claude** (Claude Code) and **Codex**, possibly
at the same time. This file is the single source of truth for the work queue,
ownership and shared resources. Read it before starting work and update it
before stopping. `CLAUDE.md` and `AGENTS.md` point here. The same scheme runs
in [bolt-aarch32](https://github.com/shadowfax80/bolt-aarch32/blob/main/docs/HANDOFF.md),
which shares the Pi with this repo.

## Rules

1. **One writer per live tree.** The shared WSL checkout
   `/home/user/lk-perf` (working tree and index), its generated LK tree
   `build/lk` (reset by `setup.sh`) and every build output under `build/` are
   covered by the *Live-tree lock*. Only the holder may edit, build, run
   `setup.sh` or commit there. Acquire and release the lock by committing
   **and successfully pushing** the table update; a local commit alone grants
   nothing. Without the lock, work in your own clone (Claude:
   `C:\Users\User\CURSOR\ClaudeProjects\lk-perf`; Codex: its own) and only
   push; the lock holder fast-forwards the shared checkout.
2. **Shared pool; claim before work.** Open items belong to no agent. Before
   starting, set *Owner* to yourself and *Status* to "In progress" in one
   commit and push it. Do not start an item the other agent has claimed; to
   take it over, ask the user and record it in the *Handoff log*.
3. **One commit (or a short series) per item, with its check.** Host-side
   changes come with a regression in `scripts/test_*.py`; target changes with
   the console command and log that show the behaviour. Run
   `python3 scripts/test_dwarf_unwind.py` and `python3 scripts/test_perf_export.py`
   before pushing any change to `scripts/`.
4. **Evidence, not claims.** *Done* names the commit, the tests and, for
   hardware claims, the image/ELF hashes and the log under `docs/results/`.
   Hardware claims come from the Pi only; QEMU is a debug aid.
5. **One shared Pi.** The Pi 4B on COM5 is shared with bolt-aarch32. Its
   reservation lives in **one** table, bolt-aarch32's
   [Pi reservation](https://github.com/shadowfax80/bolt-aarch32/blob/main/docs/HANDOFF.md#pi-reservation).
   Publish a reservation there (pushed commit) before opening COM5 for
   uploads, commands, sampling or reboots; release it with the board state
   (payload, baud, samplers, watchdog, COM5 closed). Copy the release line
   into this file's *Pi state* section.
6. **Hand off on stop.** Add a *Handoff log* entry (newest first) and push:
   what changed, what was verified, live-tree and Pi state, what to do next.
   Hardware findings and their detail still go into
   [RPI4_BRINGUP.md](RPI4_BRINGUP.md) (the evidence record); status and the
   queue live only here.
7. **Project rules still apply** (see [CLAUDE.md](../CLAUDE.md)): commits as
   `Somraj Mani <somraj.mani@gmail.com>`; LK changes only as
   `overlay/lk/NNNN-*.patch`; no FPU/NEON (`-mfpu=none`); ask the user before
   SD-card writes, driver installs, admin/UAC actions or anything that costs
   money; never write the RunPod key anywhere.

## Picking up shared work

Fetch and fast-forward your clone, then read this file's claims, the lock and
bolt-aarch32's Pi reservation before claiming anything. Preserve dirty trees
and evidence; never reset, stash or rerun `setup.sh` on the shared checkout to
synchronise it. If a claim push is rejected, fetch, re-read ownership and
retry only for what is still free. Recorded WSL/Pi state is a last-observed
snapshot, not a live guarantee.

## Resuming (no session context needed)

- **Design and contracts:** [ARCHITECTURE.md](ARCHITECTURE.md) (capture,
  dump, unwind and correctness boundaries), [EXPORT.md](EXPORT.md)
  (perf-script/Perfetto export), [DESIGN.md](DESIGN.md) (original stages).
- **Evidence and history:** [RPI4_BRINGUP.md](RPI4_BRINGUP.md), results in
  [results/](results/).
- **Target:** Cortex-A55, AArch32, SMP, always **Non-secure SVC** (the same
  state as the Pi), no FPU/NEON, PMU sampling via IRQ. The IRQ-masked blind
  spot applies on both.
- **Commands** (build in WSL with the lock, serial from Windows `python3`):
  - Build: `./setup.sh`, then `make rpi4-test -j$(nproc)` in `build/lk` (README quick start; `setup.sh` resets `build/lk`).
  - Run/capture: `python3 scripts/pi4_run.py build/lk/build-rpi4-test/lk.bin --post-jump-baud 6000000 --log <new log> --reboot -- "<commands>"`
    (lifecycle examples in ARCHITECTURE.md §10).
  - Report: `python3 scripts/pi4_pc_histogram.py <log> <matching lk.elf> --folded <out>`,
    then `perl scripts/flamegraph.pl`.
  - Export: `python3 scripts/pi4_perf_export.py` ([EXPORT.md](EXPORT.md)).
  - Tests: `python3 scripts/test_dwarf_unwind.py`, `python3 scripts/test_perf_export.py`.

## Live-tree lock

| Holder | Since | Purpose |
|---|---|---|
| — (free) | 2026-10-05 | Released by Claude after K10/K11; shared checkout at the K10/K11 commit, untracked `scripts/flamegraph.pl` (Codex) left in place |

## Pi state (last release, copied from bolt-aarch32)

| Released by | When | Board state |
|---|---|---|
| Claude | 2026-10-05 | lk-perf K10/K11 image (lk.bin `866ffee1…`) at the shell, 6000000 baud, pseudo-NMI off, samplers stopped, OS lock cleared on all cores by `dbgpcsr` (harmless; reset restores it), COM5 closed. Recheck before use; `--reboot` at 6 Mbaud returns it to the loader |

## Claims (consolidated TODO)

One shared list, seeded on 2026-10-05 from RPI4_BRINGUP's "Outstanding work"
backlog (code-review findings) and ARCHITECTURE.md §11.2, checked against the
current source. *Owner* is empty until someone claims it.

### Open, in suggested order

| Order | ID | Item | Priority | Owner | Status | Notes |
|---|---|---|---|---|---|---|
| 1 | K4 | `profiler stat` counts its own `printf` output (counters start before the status prints) | P2 | — | Open | Finding #10; confirmed still present in `profiler.c` |
| 2 | K5 | `setup.sh` re-run fails on a file left by overlay patch 0004 (`gic.h`) | P2 | — | Open | Finding #14; scoped clean of that one path before the reset |
| 3 | K11 | IRQ-masked blind spot, route 2: cross-core PC sampling through the debug PC-sample registers (`EDPCSR`), unaffected by the sampled core's IRQ mask | P2 | — | Blocked | Feasibility done (Claude, K11): A72 implements `EDPCSR` and the CPU reaches every core's debug block, but the SoC disables non-invasive debug (`DBGAUTHSTATUS` 0xaa), so `EDPCSR` reads `ffffffff`. Untested lever: `enable_jtag_gpio=1` in `config.txt` (SD-card change, needs user approval). On the target: `profiler dbginfo`; [results](results/k11_edpcsr_feasibility_20261005/README.md) |
| 4 | K7 | Full `profiler stat`: any command, all 6 counters, derived IPC | P3 | — | Open | Review Phase 3 item 3 |
| 5 | K8 | Scheduling, wakeup, blocking and CPU-frequency capture | P3 | — | Open | Documented limitation; needs target event instrumentation |
| 6 | K9 | Small fixes: stale `profiler pmu` message; `resolve_lines()` misses a function's first line when two `.debug_line` rows share an address; DESIGN.md's per-core cache-line claim | P3 | — | Open | Fold into the next commit touching the same file |
| 7 | T4 | Port to the real A55 target and validate there | P2 | User | Out of scope here | Buffer/RAM budget, toolchain re-check of `test_dwarf_unwind.py` addresses; pseudo-NMI (K12) needs the target GIC: GICv2 as on the Pi, or GICv3 `ICC_PMR` sysreg variant |

### Done (recent)

| ID | Item | Owner | Evidence |
|---|---|---|---|
| K10 | Timer-mode sampling on the profiler's own per-core virtual timer: fixed grid, one sample at a random point of each period, lost periods counted, period selectable (`start [period_us]`), dump format 3; timer samples also reach masked code under pseudo-NMI | Claude | Pi `masktest` 50% masked, about 4000 samples per run: 49.0% (1 ms), 50.9% (10 ms, commensurate), 50.4% pseudo-NMI with 0 delayed; was 65% on LK's tick; `test_irqmask_report.py` 23; [results/k10_timer_sampling_20261005](results/k10_timer_sampling_20261005/README.md) |
| K3 | No-CFI fallback: caller of assembly without CFI taken from a validated LR (follows a call, outside the leaf) | Claude | Pi `memtest`: all 460 `memcpy`/`memset` samples attributed to their true callers (previously no caller); earlier captures unchanged; `nocfi.S` unwinder fixture, exporter tests 27; [results/k3_nocfi_fallback_20261005](results/k3_nocfi_fallback_20261005/README.md) |
| K2 | Self-describing capture: session header (run, dump, image hash, modes/event/period), per-core counts, footer; host picks the latest dump and refuses a mismatched ELF | Claude | Pi: image hash stable and equal to the ELF's (0.7 ms); two-dump log split; exact transfer loss (7/8458) and overwrite counts; wrong ELF refused; older capture exported byte-identically; `test_irqmask_report.py` 20; [results/k2_session_20261005](results/k2_session_20261005/README.md) |
| K1 | Stack-copy bounds: copy stops at the top of the sampled stack; `slen` field; unwinder stops at the stack top | Claude | Pi: four-core workload had 20 bytes above SP (108 bytes read past the stack before), now bounded with a clean root; idle samples bounded at the boot-stack top; `test_dwarf_unwind.py` stack-top case, `test_irqmask_report.py` 14; [results/k1_stack_bounds_20261005](results/k1_stack_bounds_20261005/README.md) |
| K12 | IRQ-masked blind spot, route 3: GIC-priority pseudo-NMI sampling (Pi prototype, opt-in) | Claude | Overlay `0014`, `profiler nmion|nmioff`, `MASKNMI` dump line; ground truth 50.5% of PMU samples inside masked code, 0 delayed (default mode: 0%); stress + 6-min soak clean; `test_irqmask_report.py` 10; [results/k12_pseudo_nmi_20261005](results/k12_pseudo_nmi_20261005/README.md) |
| K6 | IRQ-masked blind spot, route 1: masked-time accounting, delayed-sample attribution, unbiased PMU reload, console printing without IRQ masking | Claude | Overlays `0012`/`0013`, `profiler maskon|maskoff|mask|masktest`, extended dump; `scripts/test_irqmask_report.py` (9) + existing suites; Pi ground truth: accounting 49.6% vs 50%, samples 48.2% vs 50%, 574/574 delayed samples attributed; [results/k6_irqmask_20261005](results/k6_irqmask_20261005/README.md) |
| — | Real four-core FlameGraph and Perfetto demo | Codex | [results/lk_perf_demo_20261005](results/lk_perf_demo_20261005/README.md): 3200 samples, 800/core, zero integrity rejections |
| — | perf-script export with metadata sidecar | Codex | `scripts/pi4_perf_export.py`, [EXPORT.md](EXPORT.md), 26 exporter tests |
| — | Architecture documentation | Codex | [ARCHITECTURE.md](ARCHITECTURE.md) |
| — | `stat` no longer reprograms the PMU-sampling counter; PMSELR kept in IRQ paths | Claude | `fd68193` |
| — | Dump integrity (sequence + checksum), 6 Mbaud UART | Claude | `732f462`, `f8f9abb` |

Earlier milestones (M1–M5, DWARF unwinder, review Phases 1–2) are recorded in
[RPI4_BRINGUP.md](RPI4_BRINGUP.md).

## Handoff log

### 2026-10-05 — Claude: K10 done, K11 feasibility done (blocked on the Pi); lock and Pi released

- K10: timer mode no longer samples LK's tick (LK re-arms it from the
  handling time, so samples phase-locked onto masking: 65% vs 50%). The
  profiler owns the per-core virtual timer (PPI 27). It keeps a fixed grid
  with one deadline at a random offset in each period (stratified sampling,
  so no aliasing with periodic workloads) and counts periods lost entirely
  inside masked code (`tmissed`). `profiler start [period_us]`, default 10 ms.
  Under `nmion` the timer gets the sampling priority too.
- Pi ground truth, about 4000 samples per run: 49.0% (1 ms), 50.9% (10 ms,
  exactly commensurate with the 1 ms workload), 50.4% under pseudo-NMI with
  0 of 15999 delayed; accounting 50.0%. Dump format 3 (`tperiod`, `tmixed`,
  `tmissed`); host parses 2 and 3; exporter records the timer source and
  period. Tests 23/27/6 PASS, K3 archive identical, no FPU.
- K11: `profiler dbginfo/dbgrom/dbgpcsr`. The A72 implements EDPCSR, the ROM
  table (`0xff820000`) and all four core debug blocks are reachable, and
  accesses work. But `DBGAUTHSTATUS` is 0xaa (non-invasive debug disabled), so
  EDPCSR reads `ffffffff`. Not fixable from software. K11 is now Blocked;
  `enable_jtag_gpio=1` is an untested lever that needs SD-card approval.
- Lock free, Pi released. Next: K4/K5 (small), K7.

### 2026-10-05 — Claude: K3 done (no-CFI fallback); lock and Pi released

- 33 functions in the LK image have no CFI: memcpy/memset/bzero/bcopy,
  spinlocks, cache operations, `arch_idle`, the context switch. Samples in
  them had no caller.
- Unwinder: at a sampled PC without CFI, the interrupted LR becomes the
  caller if the instruction before it (in LR's ISA, read from the ELF) is a
  call (A32 BL/BLX, T32 BL/BLX imm or BLX Rm) and LR is outside the leaf's
  own routine. The stack ends there. The report counts these samples, the
  exporter records `lr_fallback_samples`, and `--no-lr-fallback` restores
  the old behaviour.
- New `profiler memtest` ground truth: on the Pi, 299 memcpy samples went to
  `profiler_copy_a` and 161 memset samples to `profiler_fill_b`, versus no
  caller before. Earlier captures report identically.
- Tests: `nocfi.S` fixture in `test_dwarf_unwind.py`. The Codex exporter
  test for no-CFI code was updated on purpose (its LR is a real return
  address, so a caller is now expected) and a negative case was added; 27
  pass. No FPU.
- Lock free, Pi released. Next: K10 (timer phase lock), K11 (route 2), K4/K5.

### 2026-10-05 — Claude: K2 done (self-describing capture); lock and Pi released

- Feasibility first, on the Pi. An FNV-1a hash of the read-only image
  `[_start, __rodata_end)` is stable at run time (after workloads, sampling,
  pseudo-NMI, dumps) and equal to the host's hash of the ELF; it costs 0.7 ms.
- Target:
  - `profiler clear` starts a session (run id = CNTPCT); `start`/`pmustart`
    record the modes and PMU event/period, flagging a mid-session config
    change.
  - Each dump: checksummed `DUMPBEGIN` (format, run, dump number, image
    range and hash, modes, event, period), per-core `DUMPCPU` (taken,
    retained, overwritten, PMU lost) and `DUMPEND` (exact record count).
  - New `profiler buildid` command.
- Host:
  - both tools split a log into dumps and use the latest (`--dump N`), and
    refuse an ELF that does not hash to the image;
  - the report prints a session summary with exact transfer loss;
  - the exporter records the target identity, rejects a `--period` the
    target contradicts, and counts trailing loss and overwrite.
- Pi:
  - two-dump log split correctly (`mixed=1` flagged);
  - exact transfer loss 7/8458;
  - overwrite of about 1942 samples per core counted;
  - wrong (K1) ELF refused.
- Older-format captures are unchanged: Codex's demo export is
  byte-identical. Tests: K6/K1/K2 20, exporter 26, unwinder pass. No FPU.
- Lock free, Pi released. Next: K3 (no-CFI fallback), K10 or K11.

### 2026-10-05 — Claude: K1 done (stack-copy bounds); lock and Pi released

- Target: the 128-byte stack copy stops at the top of the stack the
  interrupted SP is on. That is the thread's recorded stack, or the per-core
  boot stack for idle/bootstrap threads; with neither, nothing is copied. The
  rest of the slot is zeroed and records carry `slen` (checksummed; older
  dumps still parse).
- Host: only `slen` bytes are read. When the copy ended at a stack top, the
  unwinder (new `stack_top` argument) stops at the frame whose CFA reaches
  it, so a stale return address at the top of an LK thread stack no longer
  adds a fake root frame.
- Pi:
  - every four-core-workload sample had 20 bytes above SP, so it had read
    108 bytes past its stack;
  - now bounded, with chain `initial_thread_func;profiler_smp_worker;...`;
  - idle samples bounded exactly at cpu0's boot-stack top;
  - pseudo-NMI ground truth unchanged (50.7%, 0 delayed).
- Tests: unwinder stack-top case, K6/K1 suite 14, exporter 26. No LK overlay
  change; no FPU instructions. Also fixed a K12 missing-initializer warning.
- Pi left with pseudo-NMI **off**. Lock free. Next: K2 (self-describing
  dump), K3 (no-CFI fallback) or K10/K11.

### 2026-10-05 — Claude: architecture doc updated for K6/K12; published as a web page

- `docs/ARCHITECTURE.md` now describes the code at `84c25a9`: new §4.6 (IRQ
  masking: accounting, attribution, pseudo-NMI, with diagrams and hazard
  rules), updated capture fields, buffer sizing (173 bytes/slot), compensated
  PMU reload, reports, workflow, limits, evidence and source map.
- Published as a private claude.ai page (link in the session); docs-only, no
  lock or Pi use.

### 2026-10-05 — Claude: K12 done (route 3, GIC-priority pseudo-NMI on the Pi); lock and Pi released

- Overlay `0014`: opt-in masking by GIC priority. In this mode thread-context
  `arch_disable_ints` raises `GICC_PMR` instead of setting CPSR.I. The PMU SPIs
  get the top Non-secure priority, so PMU samples land inside masked code.
- Safety:
  - every read or change of a core's mask state runs with CPSR.I briefly set;
  - the IRQ path keeps the interrupted context's mask state on the thread's
    stack and restores it at exit;
  - an interrupt racing the mask write reads as spurious and stays pending;
  - the PMU handler never reschedules, locks or prints.
- `profiler nmion|nmioff` switches every core through an IPI.
  `nmion` checks that priorities and the mask read back, else refuses.
  GIC-400 facts found: the Non-secure PMR keeps 4 bits; PPIs 16–24 are absent.
- Pi results:
  - ground truth: 50.5% of samples inside the masked code, 0 delayed
    (default mode 0%);
  - four-core workload unchanged;
  - stress with mode switching, and a 6-minute soak (944,589 samples), clean.
  Two attempts lost the shell prompt to host-side USB-serial drops on long
  output; LK was alive both times (probed).
- Overlays 0001–0014 replay byte for byte; host tests pass (K6 suite now 10);
  no FPU instructions; both archived images rebuild reproducibly.
- Not covered: IRQ handlers, timer-mode samples. The target needs its GIC
  version (T4 note). Pi left in pseudo-NMI mode (state above).
- Lock free. Next: K1 (stack bounds), K10 (timer phase lock) or K11 (route 2).

### 2026-10-05 — Claude: K6 done (IRQ-masked blind spot, route 1); lock and Pi released

- **Accounting** (overlay `0012`, `arch/arm/arm/irqmask.c`): per-core masked
  and IRQ-handler time, region counts, longest region and site, 64-site table,
  hooked in `arch_disable_ints`/`arch_enable_ints`, the GIC entry and the IRQ
  exception exit. `profiler maskon|maskoff|mask`; `start`/`pmustart` enable it;
  `dump` freezes it and prints checksummed MASKINFO/MASKCPU/MASKSITE lines.
- **Attribution:** samples carry `src/lat/msite/mgap` (checksummed; older dumps
  still parse). IRQs taken at the same unmask chain to the masking region.
  The report flags delayed samples (4x the 5th-percentile latency) and adds
  `[irq-masked: <site>]` folded frames.
- **Fixed on the way:** PMU reload now credits events counted while pending
  (`pmu_missed` counts whole lost periods). Without it the grid phase-locked to
  masking (999/1000 samples at the unmask point in the ground-truth test).
- **Console** (overlay `0013`): thread-context printing uses a mutex instead
  of an IRQ-masking spinlock. Masked spans of up to 350 us per printed line
  are gone.
- **Verified on the Pi** (`masktest` = 50% masked by construction):
  accounting 49.6%, PMU samples 48.2% masked-side, all 574 delayed samples
  attributed to `profiler_masked_spin`; four-core workload unchanged.
  Overlays 0001–0013 replay on a fresh LK `88a8efae` clone byte for byte. Host
  tests: dwarf unwind, exporter 26, new K6 9 — all pass. No FPU instructions.
- **Not fixed:** timer mode still phase-locks (LK re-arms the tick from the
  handling time) -> K10. Routes 2 and 3 queued as K11/K12.
- Lock free; Pi released in bolt-aarch32 (state above). Next: K1 (stack bounds)
  or K10/K11.

### 2026-10-05 — Codex: synchronized and adopted the shared handoff

- Fetched GitHub and confirmed Claude's coordination commit `c7b0145`.
  Read `AGENTS.md`, `CLAUDE.md`, this queue/lock, and the current board-wide
  Pi reservation in bolt-aarch32. Created the independent Codex clone at
  `C:\Users\User\CURSOR\CodexProjects\lk-perf` for work without the WSL lock.
- Sync/documentation only: no K item claimed, no implementation, tests,
  build, sampler, watchdog, or serial action. Existing shared WSL checkout
  was already at `c7b0145`; preserved its untracked `scripts/flamegraph.pl`
  and generated LK/build evidence. Live-tree lock remains free.
- Pi remains unreserved; last released state is the snapshot above, not
  re-probed during this sync. This log was committed from the Codex clone,
  without editing the shared WSL tree or Claude's checkout.
- Next: claim K1 (first suggested item) and push before implementation;
  acquire/push the lock before shared-tree work and use the single BOLT Pi
  reservation for hardware. A future lock holder can fast-forward WSL to
  include this documentation entry.

### 2026-10-05 — Claude: handoff scheme introduced

- Added this file, `AGENTS.md`, and pointers in `CLAUDE.md` and
  RPI4_BRINGUP.md. The queue above replaces RPI4_BRINGUP's "Outstanding
  work" list as the live backlog (that section stays as history).
- The Pi stays reserved through bolt-aarch32's single table, as Codex already
  did for the demo; bolt-aarch32's rule now says the table is board-wide.
- No source, build or Pi change. Lock free. Next: claim from the queue.
