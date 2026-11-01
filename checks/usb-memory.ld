ENTRY(a733_cached_memory_prepare)
SECTIONS {
 . = 0x41000000;
 .text : { *(.text*) }
 .rodata : { *(.rodata*) }
 . = 0x41200000;
 .data : { *(.data*) }
 .bss (NOLOAD) : { *(.bss.fixture*) *(COMMON) }
 __bss_end = .; __heap_end = .; __device_pool_start = .; __device_pool_end = .;
 __uncached_pool_start = 0x42200000; __uncached_pool_end = 0x42a00000;
 . = 0x421f0000;
 .tables (NOLOAD) : { __tables_start = .; *(.bss.a733_cached_tables) __tables_end = .; }
 ASSERT(__tables_end <= 0x42200000, "tables exceed monitor reservation")
 /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
