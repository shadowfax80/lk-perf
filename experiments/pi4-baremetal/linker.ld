/*
 * Flat, single-segment layout for a raw AArch32 kernel image.
 * 0x8000 is the standard physical load address Raspberry Pi firmware
 * uses for a 32-bit kernel image (kernel7l.img).
 */
ENTRY(_start)

SECTIONS
{
    . = 0x8000;

    .text.boot : { *(.text.boot) }
    .text      : { *(.text*) }
    .rodata    : { *(.rodata*) }
    .data      : { *(.data*) }

    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note*) }
}
