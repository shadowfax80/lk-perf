# Perf-script exporter verification — 2026-10-05

Host implementation and verification by Codex, starting from lk-perf `47d366a`.
No target C code, LK overlay, serial chainloader, or Pi state changed. This is
an offline exporter milestone, not new hardware sampling validation.

## Results

| Check | Result |
|---|---|
| Exporter regression suite with external importer enabled | 26 tests passed, no skips or failures |
| Existing DWARF regressions | Nested-chain, Thumb ISA-bit, r7/noreturn-boundary, and recursive-frame cases passed |
| Real consumer | Perfetto Trace Processor `v58.2-add693d8b`, RPC API 14 |
| Consumer sample count | Four input records imported as four CPU-profile samples |
| Timestamps | Sorted target times 1000000, 1000001, 2000000, 2000001 microseconds preserved within 1 ns after importer conversion |
| Imported synthetic TIDs / PID | TIDs 2, 1, 2, 1 in time order; PID 1 throughout |
| Imported stacks in time order | `arm_leaf`; `root;caller;leaf`; `no_cfi`; `root;caller;leaf` |
| Importer errors | SQL query of nonzero error-severity stats returned zero rows |
| CPU identity | Checked in emitted text; no assertion that Perfetto exposes a CPU timeline for this text format |

The fixture is [perf_export.S](../../scripts/testdata/perf_export.S), assembled
and linked with ARM GNU tools during the test. Two real Thumb CFI frames,
an ARM leaf lacking the caller's needed r7 context, and a sized symbol with
no CFI exercise full-chain and leaf-only export paths. Test stack bytes are
controlled snapshots; no target execution is claimed.

The suite additionally checks incomplete/multiple/reordered dumps, checksum
corruption, damaged sequence fields, short stack snapshots, uncountable
trailing loss, CRLF/noise, oversized timestamps, global ordering, migrating
thread identity, symbol extents/return boundaries, required PMU metadata,
known-limitations sidecars, protected output paths, cleanup after invalid
ELF input, and the actual command-line success/failure paths.

Commands run in WSL:

```bash
python3 /home/user/lk-perf/scripts/test_perf_export.py \
  --trace-processor /home/user/lk-perf/build/export-verification/trace_processor -v
python3 /home/user/lk-perf/scripts/test_dwarf_unwind.py
```

Trace Processor was obtained from the
[official wrapper](https://get.perfetto.dev/trace_processor), which downloaded
the native binary to its normal user cache. It is not vendored. The upstream
[format documentation](https://perfetto.dev/docs/getting-started/other-formats)
and parser sources were inspected to choose sample headers, leaf-to-root
frames, mapping syntax, six-digit fractional timestamps, blank terminators,
and a separate metadata sidecar instead of custom comments. Consumer SQL
assertions live in [test_perf_export.py](../../scripts/test_perf_export.py).

## What this does not establish

- New Pi/target capture correctness or a match between any deployed binary
  and a supplied ELF. Input/output hashes identify files, not the running image.
- Firefox/Hotspot acceptance, native `perf.data` compatibility, or graphical
  browser interactions. The actual tested consumer is Perfetto's CLI engine.
- Recovery of missing stack frames or execution while IRQs are masked.
  These remain [known limitations](../EXPORT.md#known-limitations), are
  included in every sidecar, and are printed by the exporter.
- Exact sample-loss totals, ring-overwrite accounting, or a fully synchronized
  target snapshot. A required `SAMPLE done` marker is not a counted footer.
