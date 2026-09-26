/*
 * Serial chainloader entry for Raspberry Pi 4B (BCM2711), AArch32.
 *
 * The firmware loads this image at 0x8000 -- exactly where the payload
 * has to go -- so the first job is to copy ourselves up to the link
 * address (0x02000000, see linker.ld) and continue from there. Until
 * that jump everything here must be position-independent.
 *
 * Firmware entry state: r0=0, r1=machine type, r2=ATAGS/DTB pointer.
 * r0-r2 are left untouched all the way into boot_main(), which hands
 * them on to the payload unchanged.
 *
 * Normally only core 0 arrives (the 32-bit armstub holds cores 1-3 on
 * their ARM-local mailboxes). If others do arrive, every core runs the
 * copy: identical bytes to identical addresses, so concurrent copies
 * are harmless, and cores 1-3 then park in the *relocated* image, where
 * the payload download can't overwrite the loop they're spinning in.
 */

.section ".text.boot"
.global _start

_start:
    /* Force both caches off unconditionally. Without this, a payload
     * large enough to overlap cache lines from this loader's own
     * earlier execution at this same address (before it relocated
     * away) can be fetched stale after the jump -- confirmed: a known-
     * good small payload padded to ~93KB reproduces total silence
     * after a clean CRC-verified transfer and jump, while the same
     * bytes unpadded work. The receive loop's data writes always land
     * correctly (that path is architecturally uncached with the MMU
     * off), so this is instruction-side staleness, not a transfer bug.
     *
     * Pi 4 enters us in HYP, where HSCTLR -- not SCTLR -- controls
     * caching of our own fetches and of a payload that stays in HYP.
     * SCTLR still matters for a payload that drops to SVC (LK does),
     * so clear both. HSCTLR is only accessible from HYP (UNDEFINED
     * from PL1), hence the mode check. */
    mrs     r7, cpsr
    and     r7, r7, #0x1f
    cmp     r7, #0x1a               @ HYP?
    bne     3f
    mrc     p15, 4, r7, c1, c0, 0   @ HSCTLR
    bic     r7, r7, #(1 << 12)      @ I bit
    bic     r7, r7, #(1 << 2)       @ C bit
    mcr     p15, 4, r7, c1, c0, 0   @ HSCTLR
    isb
3:  mrc     p15, 0, r7, c1, c0, 0   @ SCTLR
    bic     r7, r7, #(1 << 12)      @ I bit
    bic     r7, r7, #(1 << 2)       @ C bit
    mcr     p15, 0, r7, c1, c0, 0   @ SCTLR
    isb

    adr     r3, _start              @ where we are running (0x8000)
    ldr     r4, =__image_start      @ where we are linked
    ldr     r5, =__image_end
1:  ldr     r6, [r3], #4
    str     r6, [r4], #4
    cmp     r4, r5
    blo     1b

    mov     r6, #0
    dsb
    mcr     p15, 0, r6, c7, c5, 0   @ ICIALLU
    mcr     p15, 0, r6, c7, c5, 6   @ BPIALL
    dsb
    isb
    ldr     pc, =relocated

relocated:
    mrc     p15, 0, r6, c0, c0, 5   @ MPIDR
    and     r6, r6, #3
    cmp     r6, #0
    bne     park

    ldr     r3, =__bss_start
    ldr     r4, =__bss_end
    mov     r6, #0
2:  cmp     r3, r4
    strlo   r6, [r3], #4
    blo     2b

    ldr     sp, =_stack_top
    bl      boot_main               @ boot_main(r0, r1, r2), never returns
hang:
    b       hang

park:
    wfe
    b       park

/*
 * void jump_to_payload(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t entry)
 *
 * The payload was written through the data side; drop any stale
 * instruction-side state for that range (our own pre-relocation copy
 * ran from 0x8000) before branching to it. Mode is left as the firmware
 * entered us (HYP on Pi 4), same as a direct SD-card boot would.
 */
.text
.global jump_to_payload
jump_to_payload:
    mov     r12, #0
    dsb
    mcr     p15, 0, r12, c7, c5, 0  @ ICIALLU
    mcr     p15, 0, r12, c7, c5, 6  @ BPIALL
    dsb
    isb
    bx      r3

.section ".bss"
.align 4
.space 0x4000                       @ 16KB stack, core 0 only
.global _stack_top
_stack_top:
