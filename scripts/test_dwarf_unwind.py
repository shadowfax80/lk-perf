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

from elftools.elf.elffile import ELFFile

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


def build_thumb_edge(tmp: Path) -> Path:
    """Hand-written Thumb .S with explicit .cfi_* directives (see
    testdata/thumb_edge.S) -- built separately from nested.c because it
    needs precise control over two things a C compiler won't reliably
    produce on demand: an r7-based (not SP-based) CFA rule, and a
    function whose very last instruction is a call, so the return
    address lands exactly on the next function's first byte with no
    gap -- the real shape of a call to a noreturn function like
    panic()/assert-fail, which gets no epilogue.
    """
    (tmp / "thumb_edge.S").write_text((TESTDATA / "thumb_edge.S").read_text())
    (tmp / "link.ld").write_text(LINKER_SCRIPT)

    def run(*args):
        subprocess.run(args, check=True, cwd=tmp)

    run(
        "arm-none-eabi-as", "-g", "-mcpu=cortex-a15", "-mthumb",
        "thumb_edge.S", "-o", "thumb_edge.o",
    )
    run(
        "arm-none-eabi-ld", "-T", "link.ld", "thumb_edge.o",
        "-o", "thumb_edge.elf",
    )
    return tmp / "thumb_edge.elf"


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
            # mid_func's and outer_func's frames both spill r4/r5/r6
            # alongside lr (`strd r4,[sp,#-16]!; str r6,[sp,#8]`); the
            # unwinder now resolves every register's rule, not just
            # lr's, so all four spill slots need a value even though
            # only the lr ones are asserted on below -- r4/r5/r6 here
            # are placeholders, never read back by anything this test
            # checks.
            0xC140: 0xAAAA0004,  # mid_func's saved r4
            0xC144: 0xAAAA0005,  # mid_func's saved r5
            0xC148: 0xAAAA0006,  # mid_func's saved r6
            0xC14C: 0x80AC,      # mid_func's own saved lr (-> outer_func)
            0xC150: 0xAAAA0104,  # outer_func's saved r4
            0xC154: 0xAAAA0105,  # outer_func's saved r5
            0xC158: 0xAAAA0106,  # outer_func's saved r6
            0xC15C: 0x8008,      # outer_func's own saved lr (-> _start)
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

        # Two more real bugs, both exercised by the same scenario:
        # (1) after unwinding one level, every register's rule must be
        #     resolved and carried forward, not just SP/LR -- an outer
        #     frame's CFA can depend on any of them (r7 here, the usual
        #     Thumb frame pointer), and that rule can be a frame or more
        #     removed from wherever it was last live.
        # (2) a return address must be looked up as pc-1, not pc, when
        #     finding which row/FDE covers it -- otherwise a call that
        #     is the last instruction of its function (the shape of a
        #     call to a noreturn function, which gets no epilogue) has
        #     its return address land exactly on the next function's
        #     first byte, and looking that address up as-is finds the
        #     wrong (or no) FDE instead of the row active at the call.
        edge_elf = build_thumb_edge(Path(tmpdir))
        with open(edge_elf, "rb") as f:
            edge_symtab = ELFFile(f).get_section_by_name(".symtab")
            addrs = {
                sym.name: strip_isa_bit(sym["st_value"])
                for sym in edge_symtab.iter_symbols()
                if sym.name in ("victim", "caller_func", "panic_stub")
            }
        victim_pc = addrs["victim"]
        panic_stub_addr = addrs["panic_stub"]

        # victim's own CFI never mentions r7 (empty body): r7 is only
        # ever meaningful because it's carried forward, unresolved,
        # from the initial registers all the way to caller_func's row.
        fp_value = 0xC110
        stop_addr = 0x9000  # covered by no FDE -- the unwind must stop here
        edge_memory = {
            fp_value: 0xBBBB0007,      # caller_func's saved r7 (unasserted)
            fp_value + 4: stop_addr,   # caller_func's saved lr -> its
                                       # own caller (no CFI there either)
        }

        def read_memory_edge(addr, size):
            assert size == 4, size
            if addr not in edge_memory:
                raise KeyError(f"unexpected memory read at 0x{addr:x}")
            return edge_memory[addr]

        with DwarfCFIUnwinder(str(edge_elf)) as unwinder:
            edge_chain = unwinder.unwind(
                victim_pc,
                {SP_REG: 0xC100, LR_REG: panic_stub_addr, 7: fp_value},
                read_memory_edge,
            )

        edge_expected = [victim_pc, panic_stub_addr, stop_addr]
        assert edge_chain == edge_expected, (
            f"r7-carry / return-address-minus-one fix regressed: got "
            f"{[hex(p) for p in edge_chain]}, expected "
            f"{[hex(p) for p in edge_expected]} -- without either fix "
            f"this either raises (r7 dropped) or stops after 2 frames "
            f"(return address looked up without the -1 adjustment)"
        )
        print("PASS: r7 survives being carried across a frame, and a "
              "call-as-last-instruction return address unwinds past "
              "its own function's boundary correctly")

        # Real recursion revisits the exact same instruction address at
        # every depth (the same call site each time) -- confirmed a
        # real problem on real hardware, not just theoretical: a
        # pc-only cycle guard stopped a genuine 20-level-deep recursive
        # workload's unwind after just 1-2 frames every time, mistaking
        # "same instruction, deeper recursion" for "stuck in a loop".
        # Reuses mid_func's own real, already-verified CFI row (cfa =
        # sp+16, r14 saved at cfa-4) rather than a new compiled fixture
        # -- feeding it a synthetic chain where every level's own
        # "saved lr" points back to mid_func's own entry again (except
        # the last, which points to STOP_ADDR, an address covered by no
        # FDE) exercises the exact same mechanics real recursion would,
        # without needing a genuinely recursive binary.
        mid_pc = 0x8070  # inside mid_func's "full frame" row (0x8068-0x808c)
        N_LEVELS = 4      # synthetic frames resolving back to mid_pc before stopping
        STOP_ADDR = 0x9000
        sp0 = 0xC100

        recurse_memory = {}
        sp = sp0
        for level in range(N_LEVELS):
            cfa = sp + 16
            recurse_memory[cfa - 16] = 0xAAAA0000 | level  # r4, unasserted
            recurse_memory[cfa - 12] = 0xAAAA0100 | level  # r5, unasserted
            recurse_memory[cfa - 8] = 0xAAAA0200 | level   # r6, unasserted
            recurse_memory[cfa - 4] = mid_pc if level < N_LEVELS - 1 else STOP_ADDR
            sp = cfa

        def read_memory_recurse(addr, size):
            assert size == 4, size
            if addr not in recurse_memory:
                raise KeyError(f"unexpected memory read at 0x{addr:x}")
            return recurse_memory[addr]

        with DwarfCFIUnwinder(str(elf_path)) as unwinder:
            recurse_chain = unwinder.unwind(
                mid_pc, {SP_REG: sp0, LR_REG: mid_pc}, read_memory_recurse
            )

        recurse_expected = [mid_pc] * N_LEVELS + [STOP_ADDR]
        assert recurse_chain == recurse_expected, (
            f"recursion cycle-guard fix regressed: got "
            f"{[hex(p) for p in recurse_chain]}, expected "
            f"{[hex(p) for p in recurse_expected]} -- a pc-only cycle "
            f"guard stops this at length 1 (just [{hex(mid_pc)}], the "
            f"break firing before the first resolved frame is even "
            f"appended), mistaking recursion for a stuck loop"
        )
        print(f"PASS: {N_LEVELS} levels of genuine recursion (same PC, "
              f"advancing CFA each level) unwind correctly instead of "
              f"tripping the cycle guard after 2 frames")


if __name__ == "__main__":
    main()
