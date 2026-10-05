# K3: caller recovery for code without CFI, on the real Pi (2026-10-05)

Captured by Claude on the Raspberry Pi 4B (A72, AArch32, Non-secure SVC) at
6 Mbaud. Image `lk.bin` SHA-256
`052a2e9117971cb17d5f137a3eac26e3ba9604db0bcc4b980579533d213d4ae5`, matching
`lk.elf` `bc81dbe1a3805d28db999d53bbea572673eee86d4ca43336e30ecc477cf13016`
(both archived).

## The gap

33 functions in the LK image have no `.debug_frame` entry, among them
`memcpy`/`memmove`, `memset`, `bzero`, `bcopy`, `arch_spin_lock`/`unlock`/
`trylock`, the cache operations, `arch_idle` and the context switch. A sample
in one of them used to stop at the leaf: the time showed up, but not who
called it.

## The fix (host unwinder only)

When the sampled PC has no CFI, `DwarfCFIUnwinder.unwind` takes the caller
from the interrupted LR, but only if:

- the instruction just before LR, in the instruction set LR's bit 0 names,
  is a call: A32 `BL`, `BLX` (immediate or register), T32 `BL`/`BLX`
  (immediate, 32-bit) or `BLX` (register, 16-bit), read from the ELF; and
- LR does not point back into the leaf's own routine (a routine that made
  its own call leaves LR there).

The stack then ends at that caller: without CFI the leaf's stack adjustment
is unknown, so no frame beyond it can be recovered. The report counts these
samples, the exporter records them as `lr_fallback_samples`, and
`--no-lr-fallback` restores the old leaf-only behaviour.

## Results

Workload `profiler memtest 300000`: `profiler_copy_a` calls `memcpy` and
`profiler_fill_b` calls `memset` (both via Thumb `BLX` to the ARM assembly),
sampled with PMU cycles, period 1000000.

| Folded stack | Before (`--no-lr-fallback`) | After |
|---|---|---|
| `memcpy` with no caller | 299 | 0 |
| `memset` with no caller | 161 | 0 |
| `profiler_copy_a;memcpy` | 0 | **299** |
| `profiler_fill_b;memset` | 0 | **161** |
| `cmd_profiler;profiler_copy_a` / `;profiler_fill_b` (CFI path) | 2 / 2 | 2 / 2 |

All 460 samples in code without CFI got their true caller; none were
attributed to the wrong one. Earlier captures (the K1 four-core workload and
pseudo-NMI ground truth), whose samples are in code with CFI, report
identically. 16 of 480 records were lost in transfer (K2 footer count).

Tests: `test_dwarf_unwind.py` adds the `testdata/nocfi.S` fixture (A32 BL,
T32 BLX immediate and register accepted; LR not after a call, zero LR, and
LR inside the leaf's own routine rejected; fallback off). The exporter test
for code without CFI now expects the checked caller, with a new case for an
LR that does not follow a call.
