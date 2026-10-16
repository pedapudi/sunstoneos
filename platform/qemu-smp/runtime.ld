ENTRY(_start)
MEMORY {
    CODE (rx) : ORIGIN = 0x41000000, LENGTH = 2M
    RAM (rw) : ORIGIN = 0x41200000, LENGTH = 14M
}
SECTIONS {
    .text : { KEEP(*(.text.boot)) KEEP(*(.text.vectors)) *(.text*) } > CODE
    .rodata : { *(.rodata*) } > CODE
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array))
        __init_array_end = .;
    } > CODE
    .data : { *(.data*) } > RAM
    .bss (NOLOAD) : ALIGN(64) {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM
    .stacks (NOLOAD) : ALIGN(16) {
        __exception_stacks = .;
        . += 2 * 32768;
    } > RAM
    .heap (NOLOAD) : ALIGN(64) {
        __heap_start = .;
        . += 4M;
        __heap_end = .;
    } > RAM
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
