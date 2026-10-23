/* Memory qualification image layout: platform/a733/link.ld with the monitor's
 * recovery pages at fixed addresses.
 *
 * The secure timer and the exit SMC run the monitor with its MMU off, so
 * recovery needs only these four pages: vector table; timer, exception,
 * renewal and exit code with the set/way cache routines and literal pools;
 * monitor state; monitor stack. Each page holds code or data in bytes
 * 0x000-0x7bf (0x7ff for the vector table) and beacon words at 0x7c0-0xfff,
 * which apps/memtest/beacons.py writes after the link. The window scan of
 * docs/memory-qualification.md reads lines 31 and 63 of every tested page and
 * stops when one returns a beacon. Entry code and monitor_init run once before
 * any write outside the protected window and stay outside these pages. */
ENTRY(monitor_entry)
MEMORY { RAM (rwx) : ORIGIN = 0x41000000, LENGTH = 15M }
SECTIONS {
    /* reset64 0x41000000 starts the monitor entry, as in other images. */
    .text.entry 0x41000000 : {
        KEEP(*(.text.boot.monitor)) KEEP(*(.text.boot))
    } > RAM
    ASSERT(. <= 0x41001000, "entry code exceeds its page")

    .recovery_vectors 0x41001000 : {
        __memtest_recovery_start = .;
        KEEP(*(.text.monitor_vectors))
        ASSERT(. == 0x41001800, "monitor vector table must fill lines 0-31")
        QUAD(0)
        . = 0x41002000;
    } > RAM
    .recovery_code 0x41002000 : {
        KEEP(*(.text.monitor_recovery))
        */platform/a733/monitor.c.obj(.text.monitor_exception .text.finish)
        */arch/aarch64/cache.S.obj(.text)
        __memtest_recovery_code_end = .;
        . = 0x410027c0;
        QUAD(0)
        . = 0x41003000;
    } > RAM
    ASSERT(__memtest_recovery_code_end <= 0x410027c0, "recovery code exceeds lines 0-30")
    .recovery_state 0x41003000 : {
        *(.monitor_state)
        __memtest_recovery_state_end = .;
        . = 0x410037c0;
        QUAD(0)
        . = 0x41004000;
    } > RAM
    ASSERT(__memtest_recovery_state_end <= 0x410037c0, "monitor state exceeds lines 0-30")
    .recovery_stack 0x41004000 : {
        . = 0x410047c0;
        __monitor_stack_top = .;
        QUAD(0)
        . = 0x41005000;
        __memtest_recovery_end = .;
    } > RAM

    /* Host-written run contract; the image's bytes here are zero. */
    .memtest_contract 0x41005000 : { KEEP(*(.memtest_contract)) } > RAM
    ASSERT(SIZEOF(.memtest_contract) == 128, "run contract is 128 bytes")

    .text : ALIGN(4096) {
        KEEP(*(.text.vectors)) *(.text*)
    } > RAM
    .rodata : { *(.rodata*) } > RAM
    .data : { *(.data*) } > RAM
    .init_array : ALIGN(8) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array))
        __init_array_end = .;
    } > RAM
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
    .heap (NOLOAD) : ALIGN(16) {
        __heap_start = .;
        . += DEFINED(__runtime_heap_size) ? __runtime_heap_size : 0;
        __heap_end = .;
    } > RAM
    .device_pool (NOLOAD) : {
        __device_pool_start = .;
        __device_pool_end = .;
    } > RAM
    .uncached_pool (NOLOAD) : {
        __uncached_pool_start = .;
        __uncached_pool_end = .;
    } > RAM
    ASSERT(__bss_end <= 0x41f00000, "image exceeds its RAM reservation")
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
