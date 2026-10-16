ENTRY(monitor_entry)
MEMORY { RAM (rwx) : ORIGIN = 0x41000000, LENGTH = 15M }
SECTIONS {
    .text : {
        KEEP(*(.text.boot.monitor)) KEEP(*(.text.monitor_vectors))
        KEEP(*(.text.boot)) KEEP(*(.text.vectors)) *(.text*)
    } > RAM
    .rodata : { *(.rodata*) } > RAM
    .data : { *(.data*) } > RAM
    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array))
        __init_array_end = .;
    } > RAM
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
    .heap (NOLOAD) : ALIGN(16) {
        __heap_start = .;
        . += DEFINED(__runtime_heap_size) ? __runtime_heap_size : 0;
        __heap_end = .;
    } > RAM
    /* An absent pool preserves the previous image extent and section layout. */
    .device_pool (NOLOAD) : {
        . = DEFINED(__device_pool_size) && __device_pool_size ? ALIGN(4096) : .;
        __device_pool_start = .;
        . += DEFINED(__device_pool_size) ? __device_pool_size : 0;
        __device_pool_end = .;
    } > RAM
    ASSERT(!DEFINED(__device_pool_size) || (__device_pool_size % 4096) == 0,
           "device pool must contain complete pages")
    ASSERT(!DEFINED(__device_pool_size) || __device_pool_size <= 2097152,
           "device pool exceeds allocation registry capacity")
    ASSERT(__device_pool_end <= ORIGIN(RAM) + LENGTH(RAM),
           "device pool exceeds image RAM reservation")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
