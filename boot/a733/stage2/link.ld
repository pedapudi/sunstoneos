ENTRY(a733_cold_image)
PHDRS {
    image PT_LOAD FLAGS(5);
    storage PT_LOAD FLAGS(6);
}
/* The cold-entry header, gate and report stay at their recorder addresses.
 * Stage-2 storage follows the cold report inside the monitor reservation;
 * boot0 copies only the flat image, so cold-entry.S zeroes this storage. */
SECTIONS {
    . = 0x47000000;
    .text : { KEEP(*(.text.cold_header)) *(.text*) *(.rodata*) } :image
    ASSERT(. <= 0x47008000, "stage-2 code overlaps the cold report")
    .data : { *(.data*) } :image
    ASSERT(SIZEOF(.data) == 0, "stage-2 flat image has no initialized data")
    . = 0x47008000;
    .bss (NOLOAD) : {
        KEEP(*(.bss.cold_report))
        ASSERT(. == 0x47008200, "cold report ABI differs");
        a733_stage2_bss_start = .;
        KEEP(*(.bss.stage2_report))
        ASSERT(. == 0x47008400, "stage-2 report ABI differs");
        *(.bss* COMMON)
        . = ALIGN(16);
        a733_stage2_bss_end = .;
        . += 0x4000;
        a733_stage2_stack_top = .;
    } :storage
    ASSERT(. <= 0x47400000, "stage-2 monitor exceeds reservation")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
