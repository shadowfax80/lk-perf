# lk-perf

Bare-metal statistical sampling profiler for [LK](https://github.com/littlekernel/lk),
AArch32 SMP -- a standalone tool for finding real hotspots/bottlenecks in
LK workloads, developed and validated on real Raspberry Pi 4B hardware
(Cortex-A72, BCM2711; QEMU was used only for early staged development,
not for this project's hardware-validation work). See
[docs/RPI4_BRINGUP.md](docs/RPI4_BRINGUP.md) for current status.

For the current implementation's architecture, data formats, operational
workflow, and correctness boundaries, see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The staged status and quick-start
text below retain earlier milestones; the architecture document explicitly
identifies current behavior, including the payload's 6,000,000-baud console
and the host tools' required baud override.

## Standard-tool export and known limitations

Export a single completed dump as timestamped `perf script` text for Perfetto:

```bash
python3 scripts/pi4_perf_export.py capture.log build/lk/build-rpi4-test/lk.elf \
  --output capture.perf --mode timer
```

This preserves individual samples, target timestamps, CPU fields, and mapped
thread identities. A JSON sidecar records hashes, supplied capture metadata,
quality counts, and limitations. See [docs/EXPORT.md](docs/EXPORT.md) for PMU
usage and the verified importer contract. This does not generate `perf.data`.

See the [real Pi FlameGraph and Perfetto demo](docs/results/lk_perf_demo_20261005/README.md)
for a four-core workload capture, interactive SVG, importable profile, saved
SQL analysis, and an offline replay with the matching image/ELF.

**Known limitations:** missing stack frames cannot be reconstructed from
incomplete captured context, and execution while IRQs are masked is invisible
to both sampling modes. Exporting or changing viewers cannot recover either.
Scheduling, wakeups, blocking reasons/durations, and CPU-frequency history
are also absent from capture; analyzing them requires additional event
instrumentation rather than stack samples alone.

**Scope: a PoC for a real target platform's actual perf use case.**
Stack unwinding uses DWARF CFI (`.debug_frame`), not ARM's own EXIDX --
the target platform's shipped firmware carries no EXIDX (dropped from the production
build to save flash/RAM) but does carry DWARF CFI in its debug-symbol
ELF, the same mechanism Trace32 already uses there to unwind crash
dumps. Matching that here means the validation transfers directly to
the real target, rather than validating a format the target platform doesn't use.

No hardware profiling assist available or assumed: no ETM, no SPE, no
BRBE (SPE/BRBE are AArch64-only architecturally, categorically unavailable
in AArch32 state regardless of silicon). Design and staged plan:
[docs/DESIGN.md](docs/DESIGN.md).

Uses the same overlay pattern (pinned LK commit + `setup.sh` +
`app/<name>/`) as [bolt-aarch32](https://github.com/shadowfax80/bolt-aarch32)
and [lk-modloader](https://github.com/shadowfax80/lk-modloader), reused
directly -- unrelated sibling projects, not something this profiler
validates or depends on.

## Quick start

```bash
./setup.sh                          # installs gcc-arm-none-eabi + pyelftools, clones LK (latest, no pin), applies overlay
cd build/lk
make rpi4-test -j$(nproc)
cd ../..
python3 scripts/pi4_serial_boot.py build/lk/build-rpi4-test/lk.bin --port COM5
```

That builds and sends the current `TARGET=rpi4` image over the serial
chainloader to a real Pi 4B (see `docs/RPI4_BRINGUP.md` for the physical
setup and current milestone status). The chainloader's own handshake
and transfer run at 115200 (fixed, resident on the SD card), but once
LK boots the console runs at the calibrated 3,000,000 baud, and
`pi4_serial_boot.py` follows automatically -- pass `--post-jump-baud
115200` when loading an older payload that doesn't reprogram its own
UART. To check the DWARF-CFI unwinder works independently of any
hardware:

```bash
python3 scripts/test_dwarf_unwind.py
```

At the LK shell, once booted:
`profiler <start|stop|status|clear|bench [iters]|nest [iters]|smp [iters]|pmu>`.
Live sample extraction over serial (replacing the old QEMU/QMP-based
`pc_histogram.py`, removed along with all QEMU references) is M5 in
`docs/RPI4_BRINGUP.md` -- not yet implemented.

## Layout

```
setup.sh              toolchain install + LK clone (latest, no pin) + overlay apply
overlay/lk/*.patch     small, additive core-LK patches (e.g. the GIC tick hook)
app/profiler/          the profiler LK module (grows through the staged plan)
project/rpi4-test.mk   LK project file (app/shell + app/profiler + app/love on TARGET=rpi4)
docs/DESIGN.md         full design: constraints, staged plan, SMP bookkeeping
docs/RPI4_BRINGUP.md   real-hardware track on Raspberry Pi 4B: status + LK port plan
experiments/pi4-*/     standalone Pi 4B images (validation image, serial chainloader)
scripts/               host-side tooling: dwarf_unwind.py (DWARF-CFI offline unwinder,
                       see test_dwarf_unwind.py), pi4_serial_boot.py/pi4_doctor.py for
                       the Pi over serial
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

Real hardware (Raspberry Pi 4B, AArch32): a bare-metal image boots and
prints over PL011, and a serial chainloader now loads images over the
console cable, so the SD card no longer moves. Next is the LK
`TARGET=rpi4` port. **To pick the work up, start with "Start here" in
[docs/RPI4_BRINGUP.md](docs/RPI4_BRINGUP.md).**

See [docs/DESIGN.md](docs/DESIGN.md) for full details.
