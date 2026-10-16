ENTRY(a733_smp_entry)
PROVIDE(_freertos_vector_table = a733_cpu_continue_el1_vectors);
MEMORY {
    CODE (rx) : ORIGIN = 0x41000000, LENGTH = 2M
    RAM (rw) : ORIGIN = 0x41200000, LENGTH = 14M
    MONITOR (rw) : ORIGIN = 0x42000000, LENGTH = 2M
}
SECTIONS {
    .text : {
        KEEP(*(.text.a733_smp_boot)) *(.text*)
    } > CODE
    .rodata : { *(.rodata*) } > CODE
    .data : { *(.data*) } > RAM
    .result 0x42000000 (NOLOAD) : {
        __a733_smp_result = .; . += 0x4000; __a733_smp_result_end = .;
    } > MONITOR
    .contract 0x42004000 (NOLOAD) : {
        __a733_smp_contract = .; . += 0x1000; __a733_smp_contract_end = .;
    } > MONITOR
    .monitor_bss (NOLOAD) : ALIGN(4096) {
        __monitor_bss_start = .;
        *(.bss.a733_cpu_continue_monitor)
        *(.bss.a733_cpu_mailboxes) *(.bss.a733_cpu_entry_stacks)
        *(.bss.a733_smp_monitor)
        . = ALIGN(16); __monitor_bss_end = .;
    } > MONITOR
    .boot_stack (NOLOAD) : ALIGN(4096) {
        . += 16384; __a733_smp_boot_stack_top = .;
    } > MONITOR
    .bss (NOLOAD) : ALIGN(4096) {
        __bss_start = .; *(.bss*) *(COMMON)
        . = ALIGN(16); __bss_end = .;
    } > RAM
    ASSERT(__bss_end <= 0x42000000, "EL1 allocations exceed RAM")
    ASSERT(__a733_smp_boot_stack_top <= 0x42200000, "monitor allocations exceed RAM")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
