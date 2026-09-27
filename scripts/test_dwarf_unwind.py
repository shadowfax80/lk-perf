#!/usr/bin/env python3
"""
Regression test for dwarf_unwind.py's DWARF-CFI unwinder.

Compiles scripts/testdata/nested.c fresh -- fully optimized, with
-fomit-frame-pointer (an FP-chain walker could not unwind this; DWARF
CFI doesn't need frame pointers at all) -- links it into a real ELF,
and unwinds a hand-traced, ground-truth call chain:

    leaf_func (mid-loop) -> mid_func (1st call site)
                          -> outer_func (1st call site)
                          -> back into _start (main was tail-called,
                             so it has no frame of its own; _start's
                             own hand-written asm has no CFI, so the
                             unwind correctly stops there)

The exact PC/SP/LR register state and the two stack memory values
needed to recover the outer frames were computed by hand from the
linked binary's own disassembly (see the comments below) -- this is a
ground truth, not a value taken from a prior run of this same code.

Requires arm-none-eabi-gcc and arm-none-eabi-ld on PATH.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from dwarf_unwind import DwarfCFIUnwinder, symbolize, SP_REG, LR_REG, strip_isa_bit

TESTDATA = Path(__file__).parent / "testdata"

LINKER_SCRIPT = """
ENTRY(_start)
SECTIONS
{
    . = 0x8000;
    .text : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    . = ALIGN(4);
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
}
"""

START_S = """
.section ".text.boot"
.global _start
_start:
    ldr     sp, =_stack_top
    bl      main
hang:
    b       hang

.section ".bss"
.align 4
.space 0x4000
.global _stack_top
_stack_top:
"""


def build(tmp: Path) -> Path:
    (tmp / "start.S").write_text(START_S)
    (tmp / "link.ld").write_text(LINKER_SCRIPT)

    def run(*args):
        subprocess.run(args, check=True, cwd=tmp)

    run(
        "arm-none-eabi-gcc", "-c", "-g", "-O2", "-fomit-frame-pointer",
        "-mcpu=cortex-a15", "-marm", "-ffreestanding",
        str(TESTDATA / "nested.c"), "-o", "nested.o",
    )
    run(
        "arm-none-eabi-gcc", "-c", "-g", "-mcpu=cortex-a15", "-marm",
        "start.S", "-o", "start.o",
    )
    run(
        "arm-none-eabi-ld", "-T", "link.ld", "start.o", "nested.o",
        "-o", "nested.elf",
    )
    return tmp / "nested.elf"


def main():
    with tempfile.TemporaryDirectory() as tmpdir:
        elf_path = build(Path(tmpdir))

        # Ground truth, hand-traced from the linked binary's own
        # disassembly (arm-none-eabi-objdump -d nested.elf):
        #
        #   _start:      ldr sp,=_stack_top (sp=0xc160); bl main
        #   main:        mov r0,#1; b outer_func  (tail call, lr=0x8008
        #                unchanged -- the address right after _start's
        #                own `bl main`)
        #   outer_func:  strd r4,[sp,#-16]! (sp=0xc150); str r6,[sp,#8];
        #                str lr,[sp,#12] (stores 0x8008 at 0xc15c);
        #                bl mid_func (1st call, lr=0x80ac)
        #   mid_func:    strd r4,[sp,#-16]! (sp=0xc140); str r6,[sp,#8];
        #                str lr,[sp,#12] (stores 0x80ac at 0xc14c);
        #                bl leaf_func (1st call, lr=0x8078)
        #   leaf_func:   sub sp,sp,#8 (sp=0xc138); never touches lr
        #                (a true leaf) -- sampled mid-loop at pc=0x8038
        pc, sp, lr = 0x8038, 0xC138, 0x8078
        mock_memory = {
            0xC14C: 0x80AC,  # mid_func's own saved lr (-> outer_func)
            0xC15C: 0x8008,  # outer_func's own saved lr (-> _start)
        }

        def read_memory(addr, size):
            assert size == 4, size
            if addr not in mock_memory:
                raise KeyError(f"unexpected memory read at 0x{addr:x}")
            return mock_memory[addr]

        with DwarfCFIUnwinder(str(elf_path)) as unwinder:
            chain = unwinder.unwind(pc, {SP_REG: sp, LR_REG: lr}, read_memory)

        print("PC chain:", [hex(p) for p in chain])
        print("Symbolized:", symbolize(str(elf_path), chain))

        expected = [0x8038, 0x8078, 0x80AC, 0x8008]
        assert chain == expected, (
            f"MISMATCH: got {[hex(p) for p in chain]}, "
            f"expected {[hex(p) for p in expected]}"
        )
        print("\nPASS: matches hand-traced ground truth "
              "(leaf_func -> mid_func -> outer_func -> _start)")

        # the target platform's workload is ARM/Thumb interworking code: BL/BLX sets
        # LR with the ISA bit (bit 0) when the call target is Thumb, and
        # Thumb function symbols carry the same bit in the ELF. This
        # binary is built -marm, so none of its real addresses have that
        # bit -- simulate it by OR-ing every register/memory address in
        # the exact same scenario with 1, exactly as a real capture off
        # Thumb code would, and confirm the unwinder still recovers the
        # identical (masked) chain rather than stopping short or missing
        # the FDE at each hop.
        thumb_pc, thumb_sp, thumb_lr = pc | 1, sp, lr | 1
        thumb_memory = {addr: val | 1 for addr, val in mock_memory.items()}

        def read_memory_thumb(addr, size):
            assert size == 4, size
            if addr not in thumb_memory:
                raise KeyError(f"unexpected memory read at 0x{addr:x}")
            return thumb_memory[addr]

        with DwarfCFIUnwinder(str(elf_path)) as unwinder:
            thumb_chain = unwinder.unwind(
                thumb_pc, {SP_REG: thumb_sp, LR_REG: thumb_lr}, read_memory_thumb
            )
        assert thumb_chain == expected, (
            f"ISA-bit masking broke unwinding: got "
            f"{[hex(p) for p in thumb_chain]}, expected "
            f"{[hex(p) for p in expected]}"
        )
        assert all(strip_isa_bit(p) == p for p in thumb_chain), (
            "unwind() leaked the ISA bit into a chain PC"
        )
        print("PASS: ISA-bit (Thumb interworking) addresses unwind "
              "identically once masked")


if __name__ == "__main__":
    main()
