ENTRY(monitor_entry)
MEMORY { RAM (rwx) : ORIGIN = 0x41000000, LENGTH = 15M }
SECTIONS {
    .text : {
        KEEP(*(.text.boot.monitor)) KEEP(*(.text.monitor_vectors))
        KEEP(*(.text.boot)) KEEP(*(.text.vectors)) *(.text*)
    } > RAM
    .rodata : { *(.rodata*) } > RAM
    .data : { *(.data*) } > RAM
    .monitor_state : { *(.monitor_state) } > RAM
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
    .monitor_stack (NOLOAD) : ALIGN(16) {
        . += 8192;
        __monitor_stack_top = .;
    } > RAM
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
