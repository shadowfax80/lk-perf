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
./setup.sh                          # installs gcc-arm-none-eabi + qemu-system-arm, clones/pins LK, applies overlay
cd build/lk
make profiler -j$(nproc)
qemu-system-arm -machine virt -cpu cortex-a15 -smp 1 -m 512 -nographic \
  -kernel build-profiler/lk.elf
```

At the LK shell: `profiler` (stage 0 — proves the pipeline; no sampling yet).

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

Stage 0 only (pipeline skeleton). See [docs/DESIGN.md](docs/DESIGN.md) for
stages 1–5.
