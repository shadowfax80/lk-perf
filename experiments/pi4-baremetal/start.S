/*
 * Minimal AArch32 boot stub for Raspberry Pi 4B (BCM2711).
 *
 * With arm_64bit=0 in config.txt, Pi 4's firmware releases all 4
 * Cortex-A72 cores simultaneously in AArch32 SVC mode at this entry
 * point (0x8000) -- unlike QEMU's -cpu cortex-a15, which only ever
 * starts core 0 unless -smp is passed. Every other core must be
 * parked here, or all 4 will race into main() on the same stack.
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
