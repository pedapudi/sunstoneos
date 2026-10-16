ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x40080000, LENGTH = 127M }
SECTIONS {
    .text : { KEEP(*(.text.boot)) KEEP(*(.text.vectors)) *(.text*) } > RAM
    .rodata : { *(.rodata*) } > RAM
    .data : { *(.data*) } > RAM
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } > RAM
    .exception_stack (NOLOAD) : ALIGN(16) {
        . += 32768;
        __exception_stack_top = .;
    } > RAM
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
