@ K3 fixture: hand-written assembly without CFI (like LK's memcpy/memset,
@ spinlocks and cache operations), called from functions that have CFI.
@ Labels after each call give the exact return addresses the test expects.
.syntax unified
.cfi_sections .debug_frame
.text

.arm
.global _start
.type _start, %function
_start:
    .cfi_startproc
    bl caller_a
    b .
    .cfi_endproc
.size _start, .-_start

@ ARM caller with CFI: BL to a no-CFI leaf
.global caller_a
.type caller_a, %function
caller_a:
    .cfi_startproc
    push {r4, lr}
    .cfi_def_cfa_offset 8
    .cfi_offset r4, -8
    .cfi_offset lr, -4
    bl asm_leaf
.global ret_a
ret_a:
    pop {r4, pc}
    .cfi_endproc
.size caller_a, .-caller_a

@ no CFI: never touches LR, so the interrupted LR is its return address
.global asm_leaf
.type asm_leaf, %function
asm_leaf:
    mov r0, r0
.global leaf_body
leaf_body:
    mov r1, r1
    bx lr
.size asm_leaf, .-asm_leaf

@ no CFI and calls out itself: after its own BL returns, LR points back
@ into this same routine, which must not be reported as its caller
.global asm_nonleaf
.type asm_nonleaf, %function
asm_nonleaf:
    push {lr}
    bl asm_leaf
.global ret_inside
ret_inside:
    pop {pc}
.size asm_nonleaf, .-asm_nonleaf

.thumb
@ Thumb caller with CFI: 32-bit BLX (immediate) to the ARM leaf
.global tcaller
.type tcaller, %function
.thumb_func
tcaller:
    .cfi_startproc
    push {r7, lr}
    .cfi_def_cfa_offset 8
    .cfi_offset r7, -8
    .cfi_offset lr, -4
    blx asm_leaf
.global ret_t
ret_t:
    pop {r7, pc}
    .cfi_endproc
.size tcaller, .-tcaller

@ Thumb caller with CFI: 16-bit BLX (register)
.global tcaller_reg
.type tcaller_reg, %function
.thumb_func
tcaller_reg:
    .cfi_startproc
    push {r7, lr}
    .cfi_def_cfa_offset 8
    .cfi_offset r7, -8
    .cfi_offset lr, -4
    ldr r3, =asm_leaf
    blx r3
.global ret_t_reg
ret_t_reg:
    pop {r7, pc}
    .cfi_endproc
    .ltorg
.size tcaller_reg, .-tcaller_reg
