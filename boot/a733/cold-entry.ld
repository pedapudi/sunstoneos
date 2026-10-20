ENTRY(a733_cold_image)
PHDRS {
    image PT_LOAD FLAGS(5);
    report PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x47000000;
    .text : { KEEP(*(.text.cold_header)) *(.text*) *(.rodata*) } :image
    ASSERT(. <= 0x47008000, "cold entry overlaps its report")
    . = 0x47008000;
    .bss (NOLOAD) : { *(.bss.cold_report) } :report
    ASSERT(SIZEOF(.bss) == 512, "cold report ABI differs")
    ASSERT(. <= 0x47400000, "cold monitor exceeds reservation")
    /DISCARD/ : { *(.comment) *(.note*) }
}
