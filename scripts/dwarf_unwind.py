"""
DWARF CFI (.debug_frame) based stack unwinder, using pyelftools.

Chosen over ARM's own EXIDX deliberately: this project is a PoC for a
real target platform's actual perf use case, and that target platform's shipped
firmware carries no EXIDX (dropped from the production build) but does
carry DWARF CFI in its debug-symbol ELF -- the same mechanism Trace32
already uses there to unwind crash dumps.

Given a PC + register state (at minimum SP=r13 and LR=r14) and a way to
read target memory, walks the .debug_frame call-frame tables to recover
the caller's PC/SP one level at a time, exactly like a real unwinder
does -- no frame pointer required.

The target platform's workload is ARM/Thumb interworking code (built -mthumb, with
some ARM-mode functions still mixed in), so every address that came
from a *register* rather than straight from the instruction stream may
carry the ARM interworking "ISA bit" (bit 0 set = callee is Thumb) --
that's how BLX/BL set LR so a later `bx lr` switches mode correctly,
and it's also how Thumb function symbols are marked in the ELF symbol
table. FDE address ranges (`initial_location`) are always real, even
instruction addresses, so that bit must be masked off before using any
such value as a lookup key -- never left in, and never trusted as
meaningful beyond "was the target Thumb".
"""
from __future__ import annotations

import os
from typing import Callable, Optional

from elftools.elf.elffile import ELFFile
from elftools.dwarf.callframe import FDE, CFARule, RegisterRule

SP_REG = 13
LR_REG = 14  # also the usual ARM DWARF return-address register

ISA_BIT = 1  # ARM interworking: bit 0 of a register/symbol address means
             # "target is Thumb"; never present in a real instruction
             # address or an FDE's initial_location.


def strip_isa_bit(addr: int) -> int:
    return addr & ~ISA_BIT


class DwarfCFIUnwinder:
    """Loads .debug_frame from an ELF and unwinds PC chains from it."""

    def __init__(self, elf_path: str):
        self._f = open(elf_path, "rb")
        try:
            self._elf = ELFFile(self._f)
            if not self._elf.has_dwarf_info():
                raise ValueError(f"{elf_path}: no DWARF info (build with -g)")
            dwarf = self._elf.get_dwarf_info()
            if not dwarf.has_CFI():
                raise ValueError(
                    f"{elf_path}: no .debug_frame section (build with -g; "
                    f"do not strip debug sections)"
                )
            self._fdes: list[FDE] = []
            for entry in dwarf.CFI_entries():
                if isinstance(entry, FDE):
                    self._fdes.append(entry)
            # Executable bytes, for validating a raw-LR fallback (K3).
            self._path = elf_path
            self._code = [(sec["sh_addr"], sec.data()) for sec in self._elf.iter_sections()
                          if sec["sh_type"] == "SHT_PROGBITS" and sec["sh_flags"] & 0x4]
            # Whether the last unwind() used the raw-LR fallback.
            self.last_lr_fallback = False
        except Exception:
            # __exit__ cannot run if construction fails (e.g. a stale/stripped
            # or malformed ELF supplied to an exporter). Do not leak the file.
            self._f.close()
            raise

    def close(self) -> None:
        self._f.close()

    def __enter__(self) -> "DwarfCFIUnwinder":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _find_fde(self, pc: int) -> Optional[FDE]:
        for fde in self._fdes:
            start = fde["initial_location"]
            end = start + fde["address_range"]
            if start <= pc < end:
                return fde
        return None

    def _code_read(self, addr: int, size: int) -> Optional[int]:
        """Little-endian value of `size` code bytes at `addr`, or None if the
        bytes are not all inside one executable section."""
        for base, data in self._code:
            if base <= addr and addr + size <= base + len(data):
                return int.from_bytes(data[addr - base:addr - base + size], "little")
        return None

    def _follows_call(self, lr: int) -> bool:
        """True if `lr` is the return address of a call instruction: the
        instruction just before it (in the ISA that LR's bit 0 names) is a
        branch with link -- A32 BL/BLX(imm)/BLX(Rm), T32 BL/BLX(imm) (32-bit)
        or BLX(Rm) (16-bit)."""
        addr = strip_isa_bit(lr)
        if lr & 1:
            if addr % 2:
                return False
            hw = self._code_read(addr - 2, 2)
            if hw is not None and (hw & 0xFF87) == 0x4780:      # BLX Rm
                return True
            hw1, hw2 = self._code_read(addr - 4, 2), self._code_read(addr - 2, 2)
            return (hw1 is not None and hw2 is not None and (hw1 & 0xF800) == 0xF000
                    and (hw2 & 0xC000) == 0xC000)                # BL / BLX imm
        if addr % 4:
            return False
        w = self._code_read(addr - 4, 4)
        if w is None:
            return False
        return (((w >> 28) != 0xF and (w & 0x0F000000) == 0x0B000000)   # BL<c>
                or (w & 0xFE000000) == 0xFA000000                        # BLX imm
                or (w & 0x0FFFFFF0) == 0x012FFF30)                       # BLX<c> Rm

    def _lr_caller(self, leaf_pc: int, lr: Optional[int]) -> Optional[int]:
        """K3: the caller of a leaf that has no CFI, taken from the live LR,
        or None when LR is not believable: it must be the return address of a
        call instruction, and must not point back into the leaf's own
        function (a routine that made its own call leaves LR there)."""
        if not lr or not self._follows_call(lr):
            return None
        caller = strip_isa_bit(lr)
        leaf_fn = find_function(self._path, leaf_pc)
        caller_fn = find_function(self._path, caller - 1)   # return address: look up pc-1
        if leaf_fn is not None and caller_fn is not None and leaf_fn[1] == caller_fn[1]:
            return None
        return caller

    def _row_for_pc(self, fde: FDE, pc: int) -> Optional[dict]:
        table = fde.get_decoded().table
        row = None
        for candidate in table:
            if candidate["pc"] > pc:
                break
            row = candidate
        return row

    def _resolve_cfa(self, row: dict, registers: dict[int, int]) -> int:
        cfa_rule: CFARule = row["cfa"]
        if cfa_rule.expr is not None:
            raise NotImplementedError(
                "DW_CFA_def_cfa_expression (DWARF expression CFA rules) "
                "not needed by this project's simple stack layouts; not "
                "implemented"
            )
        if cfa_rule.reg not in registers:
            raise ValueError(
                f"CFA rule needs register {cfa_rule.reg}, not available"
            )
        return registers[cfa_rule.reg] + cfa_rule.offset

    def _resolve_register(
        self,
        row: dict,
        regnum: int,
        cfa: int,
        registers: dict[int, int],
        read_memory: Callable[[int, int], int],
    ) -> Optional[int]:
        rule = row.get(regnum)
        if rule is None:
            # No rule recorded for this register at this PC: it was
            # never disturbed by this frame, so its value is whatever
            # it already was (this is exactly the leaf-function case --
            # e.g. LR is still live in the register, never spilled).
            return registers.get(regnum)
        if rule.type == RegisterRule.SAME_VALUE:
            return registers.get(regnum)
        if rule.type == RegisterRule.OFFSET:
            return read_memory(cfa + rule.arg, 4)
        if rule.type == RegisterRule.VAL_OFFSET:
            return cfa + rule.arg
        if rule.type == RegisterRule.REGISTER:
            return registers.get(rule.arg)
        if rule.type == RegisterRule.UNDEFINED:
            return None
        raise NotImplementedError(
            f"register rule type {rule.type!r} not implemented "
            f"(EXPRESSION/VAL_EXPRESSION/ARCHITECTURAL not needed by "
            f"this project's simple stack layouts)"
        )

    def unwind(
        self,
        pc: int,
        registers: dict[int, int],
        read_memory: Callable[[int, int], int],
        max_frames: int = 64,
        stack_top: int | None = None,
        lr_fallback: bool = True,
    ) -> list[int]:
        """Return the list of PCs in this call chain, innermost first.

        `registers` must at least have SP_REG (13); LR_REG (14) is
        needed unless the first frame's FDE has no rule for it (a true
        leaf). Include any other live registers the caller has (r4-r11
        in particular) -- Thumb code commonly defines the CFA via r7
        rather than SP+offset, and that rule can be several frames
        removed from the sample, so a register only needed two or more
        levels up must still be threaded through from the very start.

        `read_memory(addr, size)` reads `size` bytes of target memory at
        `addr` and returns them as an unsigned little-endian int --
        backed by a live serial dump on real hardware, or a raw stack
        snapshot in a test/offline case.

        `stack_top`, when known, is the highest address of the stack the
        sample was taken on. A frame whose CFA (its caller's SP) reaches it
        has no caller on this stack: the walk stops there instead of
        following whatever stale return address sits at the stack top
        (LK's initial thread frame, for example).

        `lr_fallback` (K3): when the sampled PC itself has no CFI --
        hand-written assembly such as memcpy, memset or a spinlock -- the
        live LR is used as the caller if `_lr_caller` accepts it, and the
        walk ends there: without CFI the leaf's stack adjustment is
        unknown, so no frame beyond the caller can be recovered.
        `last_lr_fallback` tells whether this happened.
        """
        self.last_lr_fallback = False
        cur_pc = strip_isa_bit(pc)
        chain = [cur_pc]
        regs = dict(registers)
        # Cycle guard keys on (pc, cfa), not pc alone -- confirmed a
        # real problem on real hardware, not just theoretical: genuine
        # recursion revisits the exact same instruction address at
        # every depth (the same `bl` call site each time), which a
        # pc-only guard can't tell apart from an actual CFI-driven
        # infinite loop. cfa (the resolved frame's own base, becoming
        # the next frame's SP) is different at every real recursion
        # depth -- each level occupies distinct stack memory -- so
        # (pc, cfa) only repeats when the walk has truly stopped making
        # progress, which is the only time stopping is actually right.
        seen_frames = {(cur_pc, registers.get(SP_REG))}
        # Only the initial, actually-executing PC is looked up as-is.
        # Every PC after that came from an LR value, which points to
        # the instruction *after* a call -- looking that address up
        # directly finds the wrong FDE whenever the call was the last
        # instruction of its function (the common shape for a call to
        # a noreturn function like panic()/assert-fail, which gets no
        # epilogue), landing exactly on the next function's boundary
        # instead. Look up pc-1 instead for every such frame, while
        # still reporting/symbolizing the real (unadjusted) address.
        is_return_address = False

        for _ in range(max_frames):
            lookup_pc = cur_pc - 1 if is_return_address else cur_pc
            fde = self._find_fde(lookup_pc)
            if fde is None:
                if lr_fallback and len(chain) == 1:
                    caller = self._lr_caller(cur_pc, registers.get(LR_REG))
                    if caller is not None:
                        chain.append(caller)
                        self.last_lr_fallback = True
                break
            row = self._row_for_pc(fde, lookup_pc)
            if row is None:
                break

            cfa = self._resolve_cfa(row, regs)
            if stack_top is not None and cfa >= stack_top:
                break  # this frame is the root of its stack

            # Resolve every general-purpose register's rule for this
            # row, not just LR -- an outer frame's CFA rule may depend
            # on any of them (e.g. r7), and a register this frame never
            # touches must still carry its live value forward rather
            # than being dropped. SP itself is excluded: DWARF defines
            # the CFA *as* the caller's SP, unconditionally, regardless
            # of any rule the row may separately state for r13.
            new_regs = {SP_REG: cfa}
            for regnum in range(15):
                if regnum == SP_REG:
                    continue
                value = self._resolve_register(row, regnum, cfa, regs, read_memory)
                if value is not None:
                    new_regs[regnum] = value

            new_lr = new_regs.get(LR_REG)
            if new_lr is None or new_lr == 0:
                break

            # The mixed ARM/Thumb workload means new_lr may carry the ISA
            # bit (set by BL/BLX when the call target -- now the caller
            # we're unwinding into -- is Thumb); strip it before using
            # this as a PC for FDE lookup or as a chain/symbolize key.
            # The raw (unmasked) value is still what a real `bx lr` would
            # use, but nothing here executes code, so only the masked
            # form is ever meaningful.
            new_pc = strip_isa_bit(new_lr)

            regs = new_regs
            cur_pc = new_pc
            is_return_address = True

            frame_key = (cur_pc, cfa)
            if frame_key in seen_frames:
                break  # genuine cycle: same pc *and* same cfa -- no progress
            seen_frames.add(frame_key)
            chain.append(cur_pc)

        return chain


_symtab_cache: dict[str, tuple[float, list[tuple[int, str, int, bool]]]] = {}


def _load_symtab(elf_path: str) -> list[tuple[int, str, int, bool]]:
    """(addr, name, size, is_thumb) for every STT_FUNC symbol, sorted by
    addr ascending. Cached per (path, mtime) -- symbolize()/
    find_function() each used to reopen the ELF and rebuild this same
    table from scratch on *every* call, which made a full --folded or
    --annotate pass over a real capture (thousands of samples) take
    tens of minutes (measured: ~88ms/call). Keyed by mtime, not just
    path, so a stale cache can't survive a rebuild -- this project has
    already been bitten once by a stale-lk.elf pitfall (see
    docs/RPI4_BRINGUP.md's M5 annotate section).
    """
    mtime = os.path.getmtime(elf_path)
    cached = _symtab_cache.get(elf_path)
    if cached is not None and cached[0] == mtime:
        return cached[1]
    funcs = []
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")
        if symtab is not None:
            for sym in symtab.iter_symbols():
                if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"] != 0:
                    is_thumb = bool(sym["st_value"] & ISA_BIT)
                    funcs.append((strip_isa_bit(sym["st_value"]), sym.name,
                                  sym["st_size"], is_thumb))
    funcs.sort(key=lambda t: t[0])
    _symtab_cache[elf_path] = (mtime, funcs)
    return funcs


def symbolize(elf_path: str, pcs: list[int]) -> list[str]:
    """Nearest-function-at-or-below-PC lookup, one name per PC, grouped
    at FUNCTION granularity -- not the exact instruction. Two PCs
    inside the same function always symbolize to the identical string,
    so Counter-based aggregation over this output (the leaf-hotspot
    ranking, folded FlameGraph stacks) groups by function the way
    `perf report` does, not by address.

    Review finding #4, fixed: this used to bake the exact offset and
    raw PC into every name (`func+0xOFF (0xPC)`), so two samples in the
    same hot function almost never produced the same string -- the
    flat report showed dozens of ~1-sample rows instead of one real
    hotspot, and the folded output almost never merged two samples
    into the same flame-graph stack at all. Per-instruction detail is
    what `pi4_pc_histogram.py --annotate` is for; this is the coarser,
    function-level view everything else actually needs.
    """
    funcs = _load_symtab(elf_path)
    names = []
    for raw_pc in pcs:
        pc = strip_isa_bit(raw_pc)
        best_name = None
        for addr, name, _size, _is_thumb in funcs:
            if addr <= pc:
                best_name = name
            else:
                break
        names.append(best_name if best_name is not None else f"0x{pc:x}")
    return names


def find_function(elf_path: str, pc: int) -> Optional[tuple[str, int, int, bool]]:
    """Nearest-function-at-or-below-PC, with its real size and ISA mode
    -- for disassembling exactly one function's address range (M5
    `profiler annotate`), not just naming it. Returns
    (name, start_addr, size, is_thumb), or None if no STT_FUNC symbol
    covers this address at all.
    """
    pc = strip_isa_bit(pc)
    best = None
    for addr, name, size, is_thumb in _load_symtab(elf_path):
        if addr <= pc:
            best = (name, addr, size, is_thumb)
        else:
            break
    return best


def _load_line_table(elf_path: str) -> list[tuple[int, Optional[str], Optional[int]]]:
    """Sorted (address, filename, line) rows flattened across every
    CU's `.debug_line` program. A (address, None, None) row marks an
    `end_sequence` -- DWARF's own boundary marker for "no line info
    covers anything from here until the next sequence starts" (e.g. a
    gap between translation units, or past the last real instruction).
    Small embedded binaries: a linear scan over everything is simpler
    and fast enough than a proper per-CU low_pc/high_pc lookup first.
    """
    rows: list[tuple[int, Optional[str], Optional[int]]] = []
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        if not elf.has_dwarf_info():
            return rows
        dwarf = elf.get_dwarf_info()
        for cu in dwarf.iter_CUs():
            lp = dwarf.line_program_for_CU(cu)
            if lp is None:
                continue
            file_entries = lp["file_entry"]
            # DWARF5 file-table indices are 0-based; earlier versions 1-based.
            base = 0 if lp.header.version >= 5 else 1
            for entry in lp.get_entries():
                state = entry.state
                if state is None:
                    continue
                if state.end_sequence:
                    rows.append((state.address, None, None))
                    continue
                idx = state.file - base
                if 0 <= idx < len(file_entries):
                    filename = file_entries[idx].name.decode("utf-8", "replace")
                else:
                    filename = "?"
                rows.append((state.address, filename, state.line))
    return _sort_line_rows(rows)


def _sort_line_rows(rows):
    """K9: an end_sequence and the first row of the next sequence often
    share an address (one function ends where the next begins; 26 function
    starts in the LK image). Sort the end_sequence first, so a lookup at
    that address finds the function's first line whatever order the CUs
    came in -- before, those starts resolved to no line at all."""
    return sorted(rows, key=lambda r: (r[0], r[1] is not None))


def resolve_lines(elf_path: str, pcs: list[int]) -> list[Optional[str]]:
    """Best-effort "file:line" per PC from `.debug_line` (M5
    `profiler report`'s source-line attribution), or None where this
    ELF has no line info for that address -- a hand-written .S with no
    `-g`, or a real `end_sequence` gap (see `_load_line_table`).
    """
    rows = _load_line_table(elf_path)
    return _lookup_lines(rows, pcs)


def _lookup_lines(rows, pcs: list[int]) -> list[Optional[str]]:
    results: list[Optional[str]] = []
    for raw_pc in pcs:
        pc = strip_isa_bit(raw_pc)
        best: Optional[tuple[Optional[str], Optional[int]]] = None
        for addr, filename, line in rows:
            if addr <= pc:
                best = (filename, line)
            else:
                break
        results.append(f"{best[0]}:{best[1]}" if best and best[0] is not None else None)
    return results
