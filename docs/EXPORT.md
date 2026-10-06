# Standard-tool profile export

For an end-to-end worked example using real Pi data, see the
[FlameGraph and Perfetto demo](results/lk_perf_demo_20261005/README.md).

[`scripts/pi4_perf_export.py`](../scripts/pi4_perf_export.py) exports one completed
lk-perf dump as timestamped **`perf script`-compatible text**, plus a JSON metadata
sidecar. The text is suitable for profile viewers that import this format.
Perfetto Trace Processor import is verified by an automated consumer test.
The 2026-10-06 [joint review](reviews/CORRECTNESS_REVIEW_CODEX_K15_20261006.md)
found that the optional real-consumer test has a stale K3 stack expectation
(K25). Historical import receipts still describe their tested versions;
the current optional test does not pass until that expectation is reconciled.
This is not a Linux `perf.data` writer: `perf report`, `perf annotate`, and
Hotspot's native `perf.data` input remain separate interfaces.

## Known limitations

1. **Missing stack frames cannot be recovered by the exporter.** The target
   captures only 128 bytes of stack and SP/LR plus one of r7/r11. Missing live
   registers, absent/unsupported CFI, and inlining or tail-call elimination can
   remove calling context. A handled unwind exception keeps that sample as
   leaf-only; other walks can stop early without an exception. A displayed
   root is therefore not proof that the true root was recovered.
2. **Default IRQ sampling misses masked execution.** A sample delivered after
   unmasking cannot reconstruct that execution. The exporter cannot correct
   it; it accepts dumps carrying the
   K6 `src/lat/msite/mgap` fields (and K1 `slen`), and `pi4_pc_histogram.py` reports the
   masked time and the delayed samples (ARCHITECTURE.md §11.1). In
   pseudo-NMI mode (`profiler nmion`, K12), both the dedicated timer (K10)
   and PMU samples reach masked thread code directly; IRQ handlers stay
   unsampled. This opt-in Pi/GIC path is not an A55 validation claim.
3. **Stack-sample export contains no scheduler/frequency events.** Sample gaps
   cannot establish blocking, preemption or wakeup latency. Since K8, LK
   separately captures switch, wakeup, thread-name and polled clock/throttling
   events as `SCHED*`; use the scheduler systrace route below for those.
   Frequency polling can miss transitions between polls. Lost/overwritten
   scheduler events leave intervals unknown; current accounting needs K17.
4. **Mixed sources/configurations cannot be treated as one weighted event.**
   Current export checks that the requested mode is present, but does not
   split timer/PMU records or reject mid-run event/period changes (K18).
   Capture one source with an unchanged configuration; inspect the target
   mixed flags and per-record sources before interpreting weights.

Metadata sidecars and stderr also carry limitations. Some generated strings
still describe the earlier IRQ-only/no-scheduler implementation; K26 tracks
their capability-aware correction. This page describes current scope.
See [architecture correctness boundaries](ARCHITECTURE.md#11-correctness-boundaries-and-failure-behavior)
for additional capture constraints.

## Generate an export

Stop both sampling modes before clearing/dumping. Finish serial capture. The
exporter selects the latest current-format dump by default; `--dump 0` selects
the first. Each selected dump must be complete, with `SAMPLE done` and valid
session metadata. For legacy logs without `DUMPBEGIN`, use exactly one dump.
Supply the
**unstripped ELF from the same build as the running payload**.

```bash
python3 scripts/pi4_perf_export.py capture-timer-001.log \
  build/lk/build-rpi4-test/lk.elf \
  --output capture-timer-001.perf --mode timer \
  --image build/lk/build-rpi4-test/lk.bin --run-id timer-001
```

Outputs:

- `capture-timer-001.perf`: sample headers and leaf-to-root call chains.
- `capture-timer-001.perf.metadata.json`: format/schema version, export identity,
  optional capture ID, input/output SHA-256 hashes, capture labels, thread
  mapping, quality counts, and known limitations.

Use a new output name. Existing output/sidecar files and input paths are
protected from overwrite. On an ordinary export failure, files created by
that attempt are removed; interrupted processes can still leave files to
inspect before choosing another output name.

For a PMU capture made with `profiler pmustart 0x11 1000000`:

```bash
python3 scripts/pi4_perf_export.py capture-pmu-001.log \
  build/lk/build-rpi4-test/lk.elf \
  --output capture-pmu-001.perf --mode pmu \
  --event cpu-cycles --period 1000000 --run-id pmu-001
```

PMU mode requires both event and period. Event names are simple labels, such
as `cpu-cycles` or `event-0x08`; they are not resolved through Linux's PMU event
database. Use the actual event/period from the capture. Timer mode defaults to
`timer-tick` with header weight 1 per accepted sample; it does not infer a
frequency or convert sample counts into nanoseconds. With no mode supplied,
the exporter labels samples `lk-sample` and records mode `unknown`.

Current K2/K10 dumps carry a checksummed header with run/dump IDs, image
range/hash, modes and configuration, per-core counts and a footer. The
exporter checks the ELF against the target's 32-bit FNV hash of the stated
read-only image range and records the target metadata. This detects stale
pairing within that range; it is not a cryptographic attestation of the whole
payload, all debug information or firmware state. Host SHA-256 hashes identify
the supplied files. Legacy dumps carry no such target identity and rely on
operator pairing. Event names remain operator labels in both formats.

Do not describe a mixed-source/configuration capture as a single known event;
the current exporter can still produce that misleading output (K18).
`export_id` identifies this conversion;
`capture_run_id` is the optional user-provided ID and is null when unspecified.

## Text format and identity

Example shape (illustrative):

```text
lk-80002000 1/1 [002] 1.000001: 1 timer-tick:
        80008102 leaf_function (lk.elf)
        800080f0 caller_function (lk.elf)

```

- The first line is a sample header; the file contains no custom comment
  header because Perfetto's perf-text reader expects a sample on the first line.
  Metadata lives in the sidecar rather than between records.
- Samples are sorted globally by integer timestamp, then CPU and source
  sequence. The exporter formats microseconds as seconds with exactly six
  fractional digits without a floating-point conversion.
- Timestamps retain the target's generic-timer epoch. They are not host wall
  time or Linux boot time. Cross-system trace merging requires a separately
  established clock relationship. Timestamps beyond the signed 64-bit
  nanosecond importer range are rejected.
- CPU numbers remain in `[CPU]` fields. Some viewers/importer versions ignore
  them; preservation in the file does not guarantee a per-core UI.
- PID 1 represents the firmware. Positive synthetic TIDs are assigned to
  sorted distinct `thread_t*` pointer values, so a thread migrating between
  cores keeps its export TID. The `lk-<pointer>` comm and sidecar retain the
  original pointer. These are not Linux identities or real LK thread names;
  pointer reuse can merge different thread lifetimes.
- Frames are leaf-to-root, matching `perf script`, with a blank line after
  every sample, including the last. ARM/Thumb address bit 0 is stripped.
- A return-address frame uses PC-1 to locate its caller symbol while retaining
  the original normalized address in the output. Function extents are checked;
  out-of-range and zero-sized symbols are emitted as `[unknown]` rather than
  assigning the nearest unrelated function.

The exporter uses the existing snapshot-backed DWARF unwinder; it does not
change target capture or introduce an on-device dependency. ELF basenames
and symbol whitespace/delimiters are sanitized for the text interface. The
sidecar preserves artifact identity through hashes and records unwind depth
counts, but does not claim complete unwinds or precise event attribution.

## Input validation and loss reporting

The exporter requires one selected completed dump. It rejects sequence resets,
duplicate/reordered valid records, multiple completion markers, samples after
completion, and logs with no accepted samples. Corrupted/malformed sample
records are rejected individually so usable records can still be exported.
Width and 128-byte snapshot checks supplement the target field checksum.

The sidecar distinguishes checksum rejections, malformed records, observed
sequence gaps, accepted counts per CPU, and leaf-only exception fallbacks.
Gap counts can overlap rejected records: **do not sum them as an exact loss
total**. A checksum-damaged sequence is not trusted for run-boundary detection.

For current-format captures, valid header/footer agreement and retained-count
sums are required; sidecars report expected records, retained/overwritten
per-core counts and accepted-record loss. The review found that observed
record counts are not fully reconciled: an impossible extra valid record can
produce negative loss (K18). Treat those fields as diagnostics until the
strict structural contract is complete. Legacy `SAMPLE done` alone cannot
quantify trailing loss/overwrite, which remain unknown.

Neither format enforces producer freeze: stop before clear/dump; K19 adds a
formal lifecycle. Unwind completeness remains `not_proven`. The histogram
parser is more permissive than the exporter; checksum-valid parsing alone
does not establish a coherent session or complete stack.

## Scheduler timeline export (K8)

Record a separate scheduler window and stop before dumping:

```text
profiler sched on 100
profiler schedtest
profiler sched off
profiler schedump
```

```bash
python3 scripts/pi4_sched_report.py capture-sched.log matching-lk.elf \
  --systrace fresh-capture.systrace
```

This route emits `sched_switch`, `sched_waking` and `cpu_frequency` systrace
events for Perfetto, not perf-script stack samples. `100` is the clock polling
interval in milliseconds. Preserve the raw log and matching ELF.

**Current restrictions:** choose a fresh output path different from all input
paths and aliases. This CLI can overwrite its input ELF/log or an existing
output (K16). It also lacks strict session/count validation and can assign
unobserved overwritten time to a thread (K17); do not interpret lost windows
as exact full-window CPU/blocking/latency totals. Target scheduler stop/reset
still needs producer quiescence (K19). The K8 hardware demo verifies its
specific gap-free workload and importer contract, not these edge cases.

## View and verify

Open the `.perf` file in [Perfetto](https://ui.perfetto.dev/). Its
[perf-text importer](https://perfetto.dev/docs/getting-started/other-formats)
can expose individual samples and time-range flame graphs. Import does not
create scheduling events, recover missing frames, or add samples from masked
execution that the selected capture mode did not observe. The viewer does not automatically consume lk-perf's sidecar;
retain and inspect it alongside the profile.

[Firefox Profiler](https://github.com/firefox-devtools/profiler/blob/main/docs-user/guide-perf-profiling.md)
also documents perf-script text import. This exporter follows that shape,
but the verification recorded here is **Perfetto CLI import**, not a Firefox
or graphical UI acceptance test. Existing `.folded` exports remain available
for FlameGraph/Speedscope and aggregate profile viewing.

Offline regressions require Python/pyelftools plus the ARM GNU assembler/linker:

```bash
python3 scripts/test_perf_export.py -v
python3 scripts/test_dwarf_unwind.py
```

To include the actual Perfetto consumer test, acquire its official standalone
Trace Processor wrapper in a disposable tooling directory, then pass its path:

```bash
python3 scripts/test_perf_export.py \
  --trace-processor build/export-verification/trace_processor -v
```

The external importer is optional and not bundled or automatically installed
by the exporter. Without it, the consumer test is explicitly skipped. The
consumer test builds a controlled assembly ELF and verifies sample counts,
timestamps, synthetic PID/TIDs, frame names, root-to-leaf chain order, and
absence of importer errors through SQL. It does not depend on a mocked
consumer or fabricated CFI decoder. The stack snapshots are synthetic offline
test data, not newly acquired hardware samples.

Current verification and scope are recorded in
[PERF_EXPORT_VERIFICATION.md](results/PERF_EXPORT_VERIFICATION.md).
