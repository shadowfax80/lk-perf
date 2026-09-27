"""
DWARF CFI (.debug_frame) based stack unwinder, using pyelftools.

Chosen over ARM's own EXIDX deliberately: this project is a PoC for the
the target platform's actual perf use case, and the target platform's shipped
firmware carries no EXIDX (dropped from the production build) but does
carry DWARF CFI in its debug-symbol ELF -- the same mechanism Trace32
already uses there to unwind crash dumps.

Given a PC + register state (at minimum SP=r13 and LR=r14) and a way to
read target memory, walks the .debug_frame call-frame tables to recover
the caller's PC/SP one level at a time, exactly like a real unwinder
does -- no frame pointer required.
"""
from __future__ import annotations

from typing import Callable, Optional

from elftools.elf.elffile import ELFFile
from elftools.dwarf.callframe import FDE, CFARule, RegisterRule

SP_REG = 13
LR_REG = 14  # also the usual ARM DWARF return-address register


class DwarfCFIUnwinder:
    """Loads .debug_frame from an ELF and unwinds PC chains from it."""

    def __init__(self, elf_path: str):
        self._f = open(elf_path, "rb")
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
    ) -> list[int]:
        """Return the list of PCs in this call chain, innermost first.

        `registers` must at least have SP_REG (13); LR_REG (14) is
        needed unless the first frame's FDE has no rule for it (a true
        leaf). `read_memory(addr, size)` reads `size` bytes of target
        memory at `addr` and returns them as an unsigned little-endian
        int -- backed by a live serial dump on real hardware, or a raw
        stack snapshot in a test/offline case.
        """
        chain = [pc]
        regs = dict(registers)
        cur_pc = pc

        for _ in range(max_frames):
            fde = self._find_fde(cur_pc)
            if fde is None:
                break
            row = self._row_for_pc(fde, cur_pc)
            if row is None:
                break

            cfa = self._resolve_cfa(row, regs)
            new_lr = self._resolve_register(row, LR_REG, cfa, regs, read_memory)
            if new_lr is None or new_lr == 0:
                break

            # After unwinding one level: SP becomes the CFA (the DWARF
            # definition of CFA *is* "caller's SP at the call site"),
            # and the return address becomes the new PC.
            regs = {SP_REG: cfa, LR_REG: new_lr}
            cur_pc = new_lr

            if cur_pc in chain:
                break  # cycle guard
            chain.append(cur_pc)

        return chain


def symbolize(elf_path: str, pcs: list[int]) -> list[str]:
    """Best-effort nearest-symbol-at-or-below-PC lookup, for readable
    output. Not a substitute for real DWARF line-table lookups, just
    enough to identify which function each frame is in."""
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")
        funcs = []
        if symtab is not None:
            for sym in symtab.iter_symbols():
                if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"] != 0:
                    funcs.append((sym["st_value"], sym.name))
        funcs.sort()

    names = []
    for pc in pcs:
        best_addr, best_name = None, None
        for addr, name in funcs:
            if addr <= pc:
                best_addr, best_name = addr, name
            else:
                break
        if best_name is None:
            names.append(f"0x{pc:x}")
        else:
            names.append(f"{best_name}+0x{pc - best_addr:x} (0x{pc:x})")
    return names
