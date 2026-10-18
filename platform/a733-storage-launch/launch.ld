ENTRY(a733_smp_launch)
MEMORY { SRAM (rwx) : ORIGIN = 0x52000, LENGTH = 0x2000 }
SECTIONS {
    .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) } > SRAM
    .data : { *(.data*) } > SRAM
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > SRAM
    ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "launcher must not depend on initialized globals")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) *(.ARM.exidx*) }
}
