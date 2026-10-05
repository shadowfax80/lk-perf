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
| Claude | 2026-10-05 | K6 (IRQ-masked accounting): edits, LK build and setup.sh in the shared checkout |

## Pi state (last release, copied from bolt-aarch32)

| Released by | When | Board state |
|---|---|---|
| Codex | 2026-10-05 | lk-perf shell at 6000000 baud, both samplers stopped, 3200 samples retained, COM5 closed; no watchdog command issued. Recheck before use; reboot at 6 Mbaud to recover the loader |

## Claims (consolidated TODO)

One shared list, seeded on 2026-10-05 from RPI4_BRINGUP's "Outstanding work"
backlog (code-review findings) and ARCHITECTURE.md §11.2, checked against the
current source. *Owner* is empty until someone claims it.

### Open, in suggested order

| Order | ID | Item | Priority | Owner | Status | Notes |
|---|---|---|---|---|---|---|
| 1 | K1 | Stack-copy bounds: the fixed 128-byte snapshot can read above a shallow thread's stack allocation | P1 | — | Open | Bound the copy to the thread's actual stack (ARCHITECTURE §11.2) |
| 2 | K2 | Self-describing capture: run-id, build-id, mode/event, per-CPU totals and overwrite/loss counts in a dump header/footer; parser keeps only the latest run | P1 | — | Open | Review findings #6 and #11 (labelling); logs append and mix runs today (ARCHITECTURE §7.5) |
| 3 | K3 | No-CFI fallback: use the raw LR as the caller when hand-written assembly has no `.debug_frame` | P1 | — | Open | Finding #13; callers of memcpy/memset/spinlocks are undercounted |
| 4 | K4 | `profiler stat` counts its own `printf` output (counters start before the status prints) | P2 | — | Open | Finding #10; confirmed still present in `profiler.c` |
| 5 | K5 | `setup.sh` re-run fails on a file left by overlay patch 0004 (`gic.h`) | P2 | — | Open | Finding #14; scoped clean of that one path before the reset |
| 6 | K6 | IRQ-masked blind spot, route 1: per-core masked-cycle accounting with masking sites; tag samples delayed by masking and attribute them to the masked region; shorten lk-perf's own masked console printing | P1 | Claude | In progress | User request 2026-10-05 (raised to P1). Routes 2 (cross-core PC sampling via debug registers) and 3 (priority-mask pseudo-NMI) not started |
| 7 | K7 | Full `profiler stat`: any command, all 6 counters, derived IPC | P3 | — | Open | Review Phase 3 item 3 |
| 8 | K8 | Scheduling, wakeup, blocking and CPU-frequency capture | P3 | — | Open | Documented limitation; needs target event instrumentation |
| 9 | K9 | Small fixes: stale `profiler pmu` message; `resolve_lines()` misses a function's first line when two `.debug_line` rows share an address; DESIGN.md's per-core cache-line claim | P3 | — | Open | Fold into the next commit touching the same file |
| 10 | T4 | Port to the real A55 target and validate there | P2 | User | Out of scope here | Buffer/RAM budget, toolchain re-check of `test_dwarf_unwind.py` addresses |

### Done (recent)

| ID | Item | Owner | Evidence |
|---|---|---|---|
| — | Real four-core FlameGraph and Perfetto demo | Codex | [results/lk_perf_demo_20261005](results/lk_perf_demo_20261005/README.md): 3200 samples, 800/core, zero integrity rejections |
| — | perf-script export with metadata sidecar | Codex | `scripts/pi4_perf_export.py`, [EXPORT.md](EXPORT.md), 26 exporter tests |
| — | Architecture documentation | Codex | [ARCHITECTURE.md](ARCHITECTURE.md) |
| — | `stat` no longer reprograms the PMU-sampling counter; PMSELR kept in IRQ paths | Claude | `fd68193` |
| — | Dump integrity (sequence + checksum), 6 Mbaud UART | Claude | `732f462`, `f8f9abb` |

Earlier milestones (M1–M5, DWARF unwinder, review Phases 1–2) are recorded in
[RPI4_BRINGUP.md](RPI4_BRINGUP.md).

## Handoff log

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
