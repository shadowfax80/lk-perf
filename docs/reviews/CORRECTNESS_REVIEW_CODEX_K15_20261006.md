# Codex correctness review through K15 — 2026-10-06

Implementation baseline: `a547f94a5a228b925f1182ee309f1ebfbb84eca5`,
including LK overlays 0001–0016. Paired BOLT baseline: `d6aa4bb`, overlays
0001–0072. CR1 completes the read-only review and documentation deliverable;
implementation follow-ups remain open in [HANDOFF](../HANDOFF.md).

## Assessment and checks

K1–K15 resolved substantial defects: bounded stack snapshots, image/session
identity, checked no-CFI LR fallback, unbiased timer grid, masked-time
accounting, opt-in pseudo-NMI, all-core stat, scheduler events and gap-aware
image hashing. Their recorded Pi results remain scoped evidence. The next
priority is to make incomplete/lost/concurrent captures and conflicting PMU
use explicit rather than deriving apparently exact results from them.

Review covered capture/stack/IRQ paths, timer and PMU start/stop/reload,
pseudo-NMI assumptions, scheduler hooks/rings/name lifetimes/frequency
polling, session checksums and counts, DWARF/symbol attribution, perf text
and systrace publication, setup and handoff/evidence contracts.

| Verification performed | Result |
|---|---|
| Host discovery, `python3 -m unittest discover -s scripts -p 'test_*.py' -v` | 56 tests OK, one optional external consumer skipped: 24 capture/mask, 27 exporter, 5 scheduler |
| `python3 scripts/test_dwarf_unwind.py` | Seven checks PASS |
| Real optional `ExportTests.test_real_perfetto_importer` | **FAIL**, stale expected leaf-only `no_cfi`; actual recovered `caller;no_cfi` agrees with K3 and the existing fallback test |
| Joint adversarial probes | Checksum-valid synthetic records reproduce output overwrite, unsupported scheduler certainty, count underflow and mixed-source weighting; BOLT collector/suite problems also reproduced |

The shared [replay script, JSON and consumer log](https://github.com/shadowfax80/bolt-aarch32/tree/main/docs/results/cr1_review_20261006)
use real parsers and assembled ELF fixtures; only serial transport is replaced.
The overwrite probe runs the actual CLI on a disposable ELF. The optional
consumer uses the real installed Perfetto v58.2 Trace Processor. No board
probe, target rebuild, pseudo-NMI stress or scheduler hardware rerun occurred.
Existing Pi milestones are not promoted to all-edge-case certification.

## Findings and acceptance criteria

This is a dated findings table. Ownership/status and resume order are
maintained only in [HANDOFF](../HANDOFF.md#claims-consolidated-todo).

| ID | Priority | Finding | Acceptance criteria |
|---|---|---|---|
| K16 | P1 | Scheduler systrace output can destroy an input or earlier output | Protect log/ELF and existing destinations including aliases; validate before publication, use exclusive/staged writes and clean failed outputs; actual-CLI tests for ELF/log alias, symlink and existing output |
| K17 | P1 | Scheduler parsing/accounting does not establish a complete coherent window | Validate version/ranges/CPU inventory, run/footer/count relationships and valid sequence ordering; mark unknown time across overwrite/loss; record initial/final running identities or bound analysis to observed intervals; gap-free/lost/overwritten/truncated/mismatched/duplicate/zero-window fixtures and real consumer agreement on known intervals |
| K18 | P1 | Sample export accepts impossible counts and weights mixed sources as one event | Common strict session/record contract with observed per-core and footer bounds; retain `src` and configuration changes; reject or explicitly split incompatible source/period/event mixtures; no negative loss; preserve quantified corruption/loss and legacy behavior with explicit weaker guarantees; share contract with BOLT R31 |
| K19 | P1 | Clear/dump and scheduler boundaries are not enforced producer barriers | Explicit state/session-generation model; stop all writers before clear/snapshot; serialize scheduler hooks and join/quiesce frequency producer; refuse or safely freeze active dumps; stress all-core start/stop/clear/dump and immediate scheduler restart, with/without pseudo-NMI |
| K20 | P1 | PMU sharing remains incomplete after K15 | Own/reserve counters and shared control state or reject conflicts; restore supported predecessor configuration/count/interrupt state; prevent `pmustop` and nested counter resets during stat; strict numeric/event validation before register writes; all-core preconfigured-user and conflicting-command tests on the Pi |
| K21 | P2 | Wait-queue/thread attribution uses an overlapping guessed object range | Capture an exact join/wait-queue ownership relation or actual ABI bounds; avoid an arbitrary first matching `thread_t`; include pointer reuse with same name, lost death/name events and signed priorities in identity tests |
| K22 | P2 | Multiple masked 32-bit event wraps can collapse into one overflow flag | State/enforce safe service interval or report unreliable counts; distinguish ordinary supported wrap extension from unobservable multi-wrap intervals; masked-workload boundary tests and cross-check independent counts |
| K23 | P2 | Unwind depth does not explain why frames stopped | Structured per-sample stop reasons and aggregate confidence; distinguish stack-top/128-byte truncation, missing/unsupported CFI/registers, checked LR heuristic and exception fallback; alternate-CFA/large-frame fixtures; capture more registers only under a measured safe context contract |
| K24 | P2 | Rate/overhead/loss bounds are not calibrated for general workloads | Controlled sampling-rate sweep, handler cycle/instruction cost, missed-period/PMU-overflow accounting and bounded confidence; include pseudo-NMI and scheduler-hook load; no claim that arbitrary rates or workloads are unbiased |
| K25 | P2 | Optional real-consumer test is stale after K3 | Reconcile expected caller frame with the real assembled call site, then pass the complete exporter suite with actual Trace Processor; retain version and SQL output; do not mask a failure by skipping the test |
| K26 | P2 | Generated sidecar limitation strings contradict newer capabilities | Scope IRQ blindness by default versus pseudo-NMI mode; distinguish stack export from K8 SCHED capture and polling from complete frequency transitions; metadata/documentation regression checks for legacy and current captures |

### K16: verified CLI can overwrite its verified ELF

[`pi4_sched_report.py`](../../scripts/pi4_sched_report.py) checks the ELF image
hash, analyses the capture, then calls `write_systrace`, which opens the
destination with `"w"`. `--systrace` may name the input ELF or raw log. The
real-CLI probe supplies a matching assembled ELF and a valid checksummed
capture, sets output to the ELF, and exits 0 with that ELF replaced by
`# tracer: nop` text. The sample exporter already guards its input/output
paths; scheduler export needs an equally explicit publication contract.

### K17: checksum integrity is not scheduler accounting integrity

`parse_sched` validates individual line checksums but not header/footer run
agreement, sequence uniqueness, CPU/count inventory or timestamp/window
semantics. A checksummed footer from run `0x9999` is accepted with a header
from run `0x1234`; a repeated valid event yields two accepted events against
a one-event footer, with no rejection. Missing footer only warns before
analysis/export. Sorting by timestamp does not recover a missing switch.

`analyse` assigns the interval from header start to the first retained
switch's outgoing thread. With nine overwritten events, start tick 1000 and
first retained switch tick 9000, it assigns all **8000 unobserved ticks** to
that thread. The thread could have started much later. It also extends the
last known running thread to header stop after possible trailing loss.
Cores with no observed switches have no initial running identity; they
cannot be declared idle merely because no switch was recorded. Loss warnings
must accompany unknown intervals, not unsupported full-window totals or
latencies. Perfetto agreeing with a report over the same incomplete stream
does not independently establish the missing history.

### K18: counts and event weights can contradict the records

[`pi4_perf_export.py`](../../scripts/pi4_perf_export.py) accepts two distinct,
checksum-valid increasing-sequence samples when CPU/header/footer metadata
says one retained sample. `lost_or_rejected_samples` becomes **-1**. Existing
session checks compare retained counts with the footer, but not with all
observed sample records and their CPU/sequence bounds.

The same exporter accepts `modes=timer|pmu`, `mixed=1`, `tmixed=1` under
`--mode pmu --period 1000000`. The probe includes `src=t` and `src=p`; both
export headers become `1000000 cpu-cycles`. Target metadata does record the
mixed flags, but that does not make the emitted per-sample event/weight true.
The current strict reader discards source/delay/site fields after checking
their checksum. A changed PMU event/period cannot be reconstructed from a
final header alone; refuse that unsupported interpretation or capture the
configuration epoch. A timer-only or PMU-only legacy stream needs an explicit
operator responsibility, not invented target verification.

The paired BOLT R31 finding demonstrates why this contract must be reused:
its permissive collector publishes an incomplete dump even though the
strict exporter rejects a missing footer.

### K19/K20/K22: target findings, not fresh hardware reproductions

Static source inspection establishes the missing synchronization/restoration
steps; their consequences need controlled hardware closure:

- `clear` resets sample counters and arrays while producers may be enabled.
  `dump` snapshots heads/totals but does not stop sample producers before
  serial printing. A valid checksum cannot prove one record generation.
- Timer stop **does** disarm synchronously through all-core callbacks in
  the current source; the old architecture claim that it merely changes a
  volatile flag was stale and has been corrected. This does not guard active
  clear/dump or create a complete capture state machine.
- Scheduler enable resets per-core totals before taking `thread_lock`.
  Disable clears a flag but neither joins the frequency thread nor waits for
  a hook that already passed its enable check. `profiler_freq_record` can
  finish a mailbox request and write after disable. Immediate restart can
  interleave old producer activity with new totals/window metadata.
- Stat saves PMCR and counter-enable bits (K15), but resets event type/count,
  cycle value and interrupt state without restoring all previous users'
  state. `pmustop` clears PMCR.E globally and is not refused while stat is
  counting. Guarding `pmustart` alone does not enforce exclusive ownership.
- Event wrap extension increments once per PMOVSR bit serviced. If more
  than one 32-bit wrap happens before the bit is serviced, the hardware flag
  cannot encode how many wraps occurred. The published 2.8-wrap normal-IRQ
  workload does not establish correctness under long masked intervals.

### K21/K23/K24: attribution and quality boundaries

The scheduler owner lookup accepts the first known thread base within
`THREAD_T_SIZE=0x200`, described as a generous bound. Bases `0x80001000` and
`0x80001100` overlap under that rule: a queue at second-base + 0x20 is
attributed to the first thread. This is reproduced independently of losses.
Use an actual ownership relation rather than a broader guessed interval.
Lost death/name records also prevent proof of thread generations.

The 128-byte stack window, SP/LR and one selected r7/r11 do not support every
CFI rule or every root. K1 bounds copying and K3 validates a plausible call
predecessor, but a raw LR is still a bounded heuristic, not a full execution
history. Existing depth/exception counters do not explain every early stop.
No-FPU scope remains deliberate; it does not eliminate missing GPRs.

K10's timer grid and K6's credited PMU reload address demonstrated bias, not
every rate and workload. Sampling and scheduler hooks perturb execution,
ring overwrite removes old observations, missed triggers can change
statistical confidence, and priority routing has board-specific assumptions.
Frequency events are polled VideoCore observations, not a complete record
of every transition. Default IRQ delivery remains blind inside masked code;
K12 reaches masked thread code for both sample sources, but not IRQ handlers.

## Documentation reconciliation and pickup

[EXPORT](../EXPORT.md) now distinguishes legacy/current dump identity and
counts, documents latest-dump selection, K8 scheduling export and K12's
qualified visibility, and states current K16/K17/K18 restrictions.
[ARCHITECTURE](../ARCHITECTURE.md) now describes synchronous timer stop and
K7's actual counter allocation. Exported sidecar strings themselves remain
an implementation follow-up (K26); no script/target code was changed here.

K11 remains blocked by the Pi's debug authentication; T4 remains user-owned.
The authoritative Pi snapshot is BOLT's 2026-10-06 G1 release, newer than
lk-perf's copied K8 snapshot. Both live-tree locks and the Pi reservation are
free; no live state probe or serial action was made. Pick K16–K20 before
expanding profiler features. Claim from the shared queue, reserve the live
tree for target/build mutations, and use the single BOLT reservation for Pi
work. Preserve K1–K15 and B1/B2 as dated milestones with their scope.
