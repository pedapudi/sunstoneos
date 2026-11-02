ENTRY(_start)
SECTIONS {
    . = 0x40000000;
    .text : { *(.text.boot) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    .bss : { *(.bss .bss.*) *(COMMON) }
}
