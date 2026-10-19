/* Extend the returning primary preparation probe with actual EL3 cache
 * adoption. The reused helper restores every changed EL3 control. Its stack
 * and report remain Secure Device memory; no Normal-memory writes occur. */
.section .text.primary_cache_probe,"ax"
.global primary_cache_probe, primary_cache_returned, primary_cache_park
primary_cache_probe:
    ldr x2, =0x42004000
    ldr x0, [x2, #24]
    ldr x1, [x2, #88]
#ifdef SUNSTONE_PRIMARY_EL1
    bl primary_local_cache_probe
#else
    bl native_el3_cache_probe
#endif
primary_cache_returned:
#ifdef SUNSTONE_PRIMARY_EL1
    cmp w0, #2
    b.eq .Lprimary_restored
#endif
    cmp w0, #1
    b.ne primary_cache_park
.Lprimary_restored:
    // Replay the displaced literal load before the original restoration path.
    ldr x16, =0x42018000
    b primary_cache_original_next
primary_cache_park:
    wfi
    b primary_cache_park
.ltorg
.section .note.GNU-stack,"",%progbits
