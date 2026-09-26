/*
 * Linked at 0x02000000 (32MB), not 0x8000: the firmware still loads the
 * raw image at 0x8000, and start.S copies [__image_start, __image_end)
 * up here before running anything position-dependent. That leaves
 * 0x8000..0x02000000 free for the payload.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x02000000;
    __image_start = .;

    .text.boot : { *(.text.boot) }
    .text      : { *(.text*) }
    .rodata    : { *(.rodata*) }
    .data      : { *(.data*) }

    . = ALIGN(4);
    __image_end = .;

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) }
}
