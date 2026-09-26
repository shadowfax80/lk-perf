/*
 * Minimal AArch32 boot stub for Raspberry Pi 4B (BCM2711).
 *
 * With arm_64bit=0 in config.txt, the firmware's 32-bit armstub
 * normally enters only core 0 here (0x8000), in HYP mode, not SVC.
 * Cores 1-3 stay in the armstub, spinning on their ARM-local mailbox 3
 * until the kernel writes an entry address there -- that is the
 * secondary-release mechanism an LK port has to use, and LK's entry
 * path has to drop from HYP to SVC. The MPIDR check below is kept as a
 * safety net (a custom armstub or kernel_old=1 can release all cores
 * at once); it costs nothing if only core 0 arrives.
 */

.section ".text.boot"
.global _start

_start:
    mrc     p15, 0, r0, c0, c0, 5   @ MPIDR
    and     r0, r0, #3              @ core ID, bits [1:0]
    cmp     r0, #0
    bne     park

    ldr     sp, =_stack_top
    bl      main
hang:
    b       hang

park:
    wfe
    b       park

.section ".bss"
.align 4
.space 0x4000                       @ 16KB stack, core 0 only
_stack_top:
