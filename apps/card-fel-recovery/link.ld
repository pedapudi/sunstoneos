ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); gate PT_LOAD FLAGS(5); request PT_LOAD FLAGS(6); result PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x41000000;
 .text : { *(.text.entry) *(.text*) *(.rodata*) } :code
 ASSERT(. <= 0x41001000,"Recovery entry exceeds its page")
 . = 0x41001000;
 .card_gate : { KEEP(*(.card_gate)) } :gate
 ASSERT(SIZEOF(.card_gate) <= 512,"Card gate exceeds its appended sector")
 . = 0x41002000;
 .data : { *(.data*) } :request
 ASSERT(SIZEOF(.data) == 32,"Recovery request geometry")
 . = 0x42000000;
 .result (NOLOAD) : { *(.result) } :result
 ASSERT(SIZEOF(.result) == 64,"Recovery report geometry")
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.ARM.exidx*) }
}
