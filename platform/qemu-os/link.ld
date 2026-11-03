ENTRY(_start)
/* Code and read-only data share the one block that the platform maps read-only
 * and executable. Data follows. The scenario page, the launch contract and
 * the sealed session are loaded at fixed addresses above RAM by the check, as
 * the stage-2 loader places them on the board. */
MEMORY {
    CODE (rx) : ORIGIN = 0x41000000, LENGTH = 2M
    RAM (rw) : ORIGIN = 0x41200000, LENGTH = 10M
    TABLES (rw) : ORIGIN = 0x41c00000, LENGTH = 2M
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
    /* Weak references to absent functions load their address from here; without a place the linker puts the
     * orphan after the table block, which EL1 cannot read. */
    .got : { *(.got*) } > RAM
    /* Static constructors, which runtime_init() of an image with the C library's start-up runs after the admission. */
    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array))
        __init_array_end = .;
    } > RAM
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
    .tables (NOLOAD) : ALIGN(4096) { *(.tables) } > TABLES
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
