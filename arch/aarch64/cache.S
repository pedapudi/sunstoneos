// Set/way maintenance while MMU and data caching are disabled at EL3.
// Only the boot CPU may be running while these whole-cache operations execute.
.section .text, "ax"
.global invalidate_data_cache
.global clean_data_cache
invalidate_data_cache:
    mov x15, #0
    b .Lcache
clean_data_cache:
    mov x15, #1
.Lcache:
    dsb sy
    mrs x0, clidr_el1
    ubfx x1, x0, #24, #3      // Levels of coherency.
    mov x2, #0
    mrs x14, csselr_el1
.Llevel:
    cmp x2, x1
    b.hs .Ldone
    add x3, x2, x2, lsl #1
    lsr x3, x0, x3
    and x3, x3, #7
    cmp x3, #2               // No data/unified cache at types 0 or 1.
    b.lo .Lnext
    lsl x3, x2, #1
    msr csselr_el1, x3
    isb
    mrs x4, ccsidr_el1
    and x5, x4, #7
    add x5, x5, #4           // Log2 cache-line bytes.
    mrs x6, id_aa64mmfr2_el1
    ubfx x6, x6, #20, #4
    cbnz x6, .Lextended
    ubfx x6, x4, #3, #10     // Ways minus one.
    ubfx x7, x4, #13, #15    // Sets minus one.
    b .Lways
.Lextended:
    ubfx x6, x4, #3, #21
    ubfx x7, x4, #32, #24
.Lways:
    clz w8, w6
    mov x9, x6
.Lway:
    mov x10, x7
.Lset:
    lsl x11, x9, x8
    orr x11, x11, x3
    lsl x12, x10, x5
    orr x11, x11, x12
    cbnz x15, .Lclean
    dc isw, x11
    b .Lstep
.Lclean:
    dc cisw, x11
.Lstep:
    subs x10, x10, #1
    b.ge .Lset
    subs x9, x9, #1
    b.ge .Lway
.Lnext:
    add x2, x2, #1
    b .Llevel
.Ldone:
    msr csselr_el1, x14
    dsb sy
    isb
    ret
