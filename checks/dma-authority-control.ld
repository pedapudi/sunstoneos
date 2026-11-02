ENTRY(dma_authority_entry)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
    . = 0x41000000;
    .text : { KEEP(*(.text.boot)) KEEP(*(.text.vectors)) *(.text*) } :code
    .rodata : ALIGN(16) { *(.rodata*) KEEP(*(.probe_marker)) } :code
    . = ALIGN(4096);
    .data : { *(.data*) *(.got*) } :data
    .monitor_state : { *(.monitor_state) } :data
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(16); __bss_end = .;
    } :data
    .stack (NOLOAD) : ALIGN(16) { . += 8192; __stack_top = .; } :data
    ASSERT(__stack_top <= 0x41f00000,"probe exceeds image reservation")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
