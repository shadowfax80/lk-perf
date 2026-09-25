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

`pc_histogram.py` boots QEMU, drives `profiler clear` / `start` / `bench` /
`stop` over the console, pulls the sample ring buffer out via QMP, and
prints a symbolized self-time histogram. At the LK shell directly:
`profiler <start|stop|status|clear|bench [iters]>`.

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

Stage 1 (PC-only histogram) verified: `pc_histogram.py` end-to-end on a
fresh QEMU boot shows real, distinguishable sample variation between two
synthetic workload functions. See [docs/DESIGN.md](docs/DESIGN.md) for
stages 2–5.
