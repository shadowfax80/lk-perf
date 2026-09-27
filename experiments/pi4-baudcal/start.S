/*
 * Entry for the UART baud-calibration test payload (Raspberry Pi 4B,
 * BCM2711, AArch32). Loaded via the existing pi4-serialboot chainloader
 * at 0x8000 -- unlike the loader itself, this doesn't need to relocate
 * (it's not resident, just a one-shot test run), so this is a plain
 * BSS-clear-and-jump, matching pi4-baremetal's entry.
 */
.section ".text.boot"
.global _start
_start:
    ldr     r3, =__bss_start
    ldr     r4, =__bss_end
    mov     r5, #0
1:  cmp     r3, r4
    strlo   r5, [r3], #4
    blo     1b

    ldr     sp, =_stack_top
    bl      boot_main               @ boot_main(r0, r1, r2), never returns
hang:
    b       hang

.section ".bss"
.align 4
.space 0x4000                       @ 16KB stack, core 0 only
.global _stack_top
_stack_top:
