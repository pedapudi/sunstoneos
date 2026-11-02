ENTRY(_start)
/* Code and read-only data share the one block that the platform maps read-only
 * and executable. Data follows. The scenario page, the launch contract and
 * the sealed session are loaded at fixed addresses above RAM by the check, as
 * the stage-2 loader places them on the board. */
MEMORY {
    CODE (rx) : ORIGIN = 0x41000000, LENGTH = 2M
    RAM (rw) : ORIGIN = 0x41200000, LENGTH = 10M
}
SECTIONS {
    .text : { KEEP(*(.text.boot)) KEEP(*(.text.vectors)) *(.text*) } > CODE
    .rodata : { *(.rodata*) } > CODE
    sunstone_manifest : ALIGN(8) {
        __start_sunstone_manifest = .;
        KEEP(*(sunstone_manifest))
        __stop_sunstone_manifest = .;
    } > CODE
    sunstone_region_initializers : ALIGN(8) {
        __start_sunstone_region_initializers = .;
        KEEP(*(sunstone_region_initializers))
        __stop_sunstone_region_initializers = .;
    } > CODE
    sunstone_large_regions : ALIGN(8) {
        __start_sunstone_large_regions = .;
        KEEP(*(sunstone_large_regions))
        __stop_sunstone_large_regions = .;
    } > CODE
    __ram_start = ORIGIN(RAM);
    .data : { *(.data*) } > RAM
    .bss (NOLOAD) : ALIGN(64) {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM
    .stacks (NOLOAD) : ALIGN(16) {
        __exception_stacks = .;
        . += 8 * 32768;
    } > RAM
    /* The C library's linked heap (runtime/newlib.c): small, so that a test can take a byte from it. */
    .heap (NOLOAD) : ALIGN(64) {
        __heap_start = .;
        . += 4096;
        __heap_end = .;
    } > RAM
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
