# lk-perf

Bare-metal statistical sampling profiler for [LK](https://github.com/littlekernel/lk),
AArch32, targeting an eventual real Cortex-A55 SMP system (ARMv8.2-A) with
QEMU as the first, cheap-to-iterate-on target.

No hardware profiling assist available or assumed: no ETM, no SPE, no
BRBE (SPE/BRBE are AArch64-only architecturally, categorically unavailable
in AArch32 state regardless of silicon). Design and staged plan:
[docs/DESIGN.md](docs/DESIGN.md).

Sibling project to [bolt-aarch32](https://github.com/shadowfax80/bolt-aarch32)
(the AArch32 BOLT backend this profiler is meant to help validate) and
[lk-modloader](https://github.com/shadowfax80/lk-modloader) — same overlay
pattern (pinned LK commit + `setup.sh` + `app/<name>/`), reused directly.

## Quick start

```bash
./setup.sh                          # installs gcc-arm-none-eabi + qemu-system-arm, clones LK (latest, no pin), applies overlay
cd build/lk
make profiler -j$(nproc)
cd ../..
python3 scripts/pc_histogram.py --elf build/lk/build-profiler/lk.elf
```

`pc_histogram.py` boots QEMU, drives `profiler clear`/`start`/`bench`
(`--nest` for a 3-level call chain, `--smp-workload` for one worker
thread per core) over the console, pauses the VM mid-workload via QMP
once enough samples exist, pulls the per-CPU PC/LR/FP/timestamp ring
buffers out, and prints a symbolized self-time histogram plus (Stage 3)
offline FP-chain-unwound call stacks in FlameGraph-compatible folded
format -- one merged file and one per core (Stage 4). At the LK shell
directly:
`profiler <start|stop|status|clear|bench [iters]|nest [iters]|smp [iters]|pmu|fpcheck>`.

## Layout

```
setup.sh              toolchain install + LK clone (latest, no pin) + overlay apply
overlay/lk/*.patch     small, additive core-LK patches (e.g. the GIC tick hook)
app/profiler/          the profiler LK module (grows through the staged plan)
project/profiler.mk    LK project file (app/shell + app/profiler on qemu-virt-arm32)
docs/DESIGN.md         full design: constraints, staged plan, SMP bookkeeping
scripts/               host-side tooling (symbolizer, offline unwinder, flamegraph glue)
```

LK is **not pinned** — deliberately tracks upstream `littlekernel/lk`'s
current default-branch tip on every `setup.sh` run. `setup.sh` prints the
resolved commit each time for traceability, but doesn't enforce it.

## Status

Stages 1–4 verified end-to-end on a fresh QEMU boot:
- Stage 1 (PC histogram): real, distinguishable sample variation between
  two synthetic workload functions.
- Stage 2 (PC+LR): immediate-caller breakdown; also empirically caught
  its own documented limitation (GCC reusing the live lr register as
  scratch mid-function under register pressure, confirmed via objdump).
- Stage 3 (FP-chain offline unwind): correctly recovers the full
  `cmd_profiler;profiler_workload_outer;profiler_workload_mid;
  profiler_workload_inner` call chain for the 3-level `nest` workload.
  See [docs/DESIGN.md](docs/DESIGN.md) for the real, generalizable
  finding this stage surfaced: a walk target's stack must still be live
  when read, which is why the host tooling pauses the VM mid-workload
  via QMP rather than waiting for a completion marker.
- Stage 4 (SMP): `-smp 4` boot verified (`welcome to lk/MP`, all 4 cores
  in `threadstats`) before writing any profiler code. Per-CPU ring
  buffers + CNTPCT timestamps; a concurrent 4-thread workload produced
  independent, near-balanced per-core sample counts (9/9/9/8) with
  correct 6-level FP-chain unwinding on every core, merged into one
  folded-stack output plus one per core.
- Stage 5 (PMU-event-triggered sampling): confirmed real-hardware-only,
  not assumed. `profiler pmu` documents two independently verified
  reasons: this QEMU target's own device tree has no PMU interrupt route
  at all, and PMU coprocessor register access itself faults here (no
  secure-monitor boot stage to clear the relevant trap) -- caught by
  isolating a crash in an earlier, more ambitious version of this
  command before it shipped, not left in a panicking state.

See [docs/DESIGN.md](docs/DESIGN.md) for full details.
