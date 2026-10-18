ENTRY(boot_checkpoint)
SECTIONS {
    . = 0x41020000;
    .text : { *(.text .text.*) *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
    ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "Overlay requires explicit data initialization")
    ASSERT(. <= 0x41024000, "Overlay exceeds saved reservation")
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
