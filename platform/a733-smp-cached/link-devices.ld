ENTRY(a733_smp_entry)
PROVIDE(_freertos_vector_table = a733_cached_el1_vectors);
MEMORY {
    CODE (rx) : ORIGIN = 0x41000000, LENGTH = 2M
    RAM (rw) : ORIGIN = 0x41200000, LENGTH = 14M
    MONITOR (rw) : ORIGIN = 0x42000000, LENGTH = 2M
    DEVICE_WB (rw) : ORIGIN = 0x48000000, LENGTH = 768M
    DEVICE_NC (rw) : ORIGIN = 0x78000000, LENGTH = 16M
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
    .tables 0x421f0000 (NOLOAD) : ALIGN(4096) {
        __tables_start = .; *(.bss.a733_cached_tables) __tables_end = .;
    } > MONITOR
    .bss (NOLOAD) : ALIGN(4096) {
        __bss_start = .; *(.bss*) *(COMMON)
        . = ALIGN(16); __bss_end = .;
    } > RAM
    __heap_end = __bss_end;
    /* These immutable ranges require a matching, externally qualified v2 handoff. */
    __device_memory_profile = 1;
    .device_pool (NOLOAD) : {
        __device_pool_start = .; . += LENGTH(DEVICE_WB); __device_pool_end = .;
    } > DEVICE_WB
    .uncached_pool (NOLOAD) : {
        __uncached_pool_start = .; . += LENGTH(DEVICE_NC); __uncached_pool_end = .;
    } > DEVICE_NC
    ASSERT(!DEFINED(__device_pool_size), "large profile has a fixed write-back reservation")
    ASSERT(!DEFINED(__uncached_pool_size), "large profile has a fixed uncached reservation")
    ASSERT(__device_pool_start == 0x48000000 && __device_pool_end == 0x78000000 &&
           __uncached_pool_start == 0x78000000 && __uncached_pool_end == 0x79000000,
           "device reservations differ from the admitted profile")
    ASSERT(__bss_end <= 0x42000000, "EL1 allocations exceed RAM")
    ASSERT(__a733_smp_boot_stack_top <= 0x421f0000, "monitor allocations exceed RAM")
    ASSERT(__tables_end <= 0x42200000, "tables exceed reservation")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
