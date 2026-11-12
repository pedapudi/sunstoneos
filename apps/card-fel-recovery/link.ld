ENTRY(_start)
PHDRS { gate PT_LOAD FLAGS(5); profile PT_LOAD FLAGS(4); }
SECTIONS {
 . = 0x52000;
 .card_gate : { KEEP(*(.card_gate)) } :gate
 ASSERT(SIZEOF(.card_gate) <= 512,"Card gate and policy word exceed the appended sector")
 . = 0x52400;
 .rodata.profile : { KEEP(*(.rodata.profile)) } :profile
 ASSERT(SIZEOF(.rodata.profile) == 96,"Recovery profile geometry")
 ASSERT(. <= 0x52c00,"Recovery image leaves its SRAM reservation")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) }
}
