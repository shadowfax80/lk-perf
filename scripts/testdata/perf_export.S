/* Offline export fixture. Every sampled/return site is named; no hardcoded
 * compiler addresses. Two real Thumb frames plus an ARM leaf exercise missing
 * r7 context. This image is not loaded onto hardware. */
.syntax unified
.cfi_sections .debug_frame
.text
.thumb

.global root
.type root, %function
.thumb_func
root:
    .cfi_startproc
    push {r7, lr}
    .cfi_def_cfa_offset 8
    .cfi_offset r7, -8
    .cfi_offset lr, -4
    mov r7, sp
    .cfi_def_cfa_register r7
    bl caller
.global root_return
root_return:
    pop {r7, pc}
    .cfi_endproc
.size root, .-root

.global caller
.type caller, %function
.thumb_func
caller:
    .cfi_startproc
    push {r7, lr}
    .cfi_def_cfa_offset 8
    .cfi_offset r7, -8
    .cfi_offset lr, -4
    mov r7, sp
    .cfi_def_cfa_register r7
    bl leaf
.global caller_return
caller_return:
    pop {r7, pc}
    .cfi_endproc
.size caller, .-caller

.global leaf
.type leaf, %function
.thumb_func
leaf:
    .cfi_startproc
    nop
    bx lr
    .cfi_endproc
.size leaf, .-leaf

.arm
.balign 4
.global arm_leaf
.type arm_leaf, %function
arm_leaf:
    .cfi_startproc
    nop
    bx lr
    .cfi_endproc
.size arm_leaf, .-arm_leaf

/* Sized symbol without CFI. It provides a known missing-frame scenario. */
.global no_cfi
.type no_cfi, %function
no_cfi:
    bx lr
.size no_cfi, .-no_cfi
