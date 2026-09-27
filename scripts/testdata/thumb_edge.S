.syntax unified
.cfi_sections .debug_frame
.thumb

.section .text.boot
.thumb_func
.global _start
_start:
    ldr     sp, =_stack_top
    bl      caller_func
hang:
    b       hang

.section .text

.thumb_func
.global victim
victim:
    @ Trivial leaf: no CFI directives at all beyond .cfi_startproc, so
    @ its row never mentions r7 -- this stands in for "sampled inside
    @ some leaf that doesn't touch r7", proving r7 must be carried
    @ forward from HERE, not re-derived at the frame that needs it.
    .cfi_startproc
    bx      lr
    .cfi_endproc

.thumb_func
.global caller_func
caller_func:
    .cfi_startproc
    push    {r7, lr}
    .cfi_def_cfa_offset 8
    .cfi_offset r7, -8
    .cfi_offset lr, -4
    mov     r7, sp
    .cfi_def_cfa_register r7
    bl      panic_stub
    .cfi_endproc
    @ Deliberately no epilogue after the bl -- mirrors a real call to a
    @ noreturn function (panic()/assert-fail): the compiler emits
    @ nothing more, so the return address sits exactly on the next
    @ function's first byte, with no gap.

.thumb_func
.global panic_stub
panic_stub:
    @ Intentionally NOT wrapped in .cfi_startproc/.cfi_endproc: no FDE
    @ covers this address at all. A naive (non -1) lookup landing here
    @ -- exactly caller_func's end boundary -- must fail to find any
    @ FDE, where the -1-adjusted lookup correctly still finds
    @ caller_func's own final row instead.
    bx      lr

.section .bss
.align 4
.space 0x4000
.global _stack_top
_stack_top:
