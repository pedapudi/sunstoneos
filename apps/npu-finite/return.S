/* Identity-mapped EL1 tail: no stack access after translation/cache disable.
 * The original single-core monitor remains cache disabled at EL3. */
.section .text,"ax"
.global npu_finite_smc
.type npu_finite_smc,%function
npu_finite_smc:
    msr daifset,#15
    mov x1,x0
    mov x0,#0x733
    dsb sy
    mrs x9,sctlr_el1
    ldr x10,=0x1005
    bic x9,x9,x10
    msr sctlr_el1,x9
    isb
    smc #0
1:  wfe
    b 1b
.size npu_finite_smc,.-npu_finite_smc
/* Accepted returning kernel's forced-FEL sequence, after successful NPU stop
 * and bounded interrupt cleanup. Whole-cache completion includes stores on the
 * final client/monitor stack after the owned-interval publication. */
.global npu_finite_rom_return
.type npu_finite_rom_return,%function
npu_finite_rom_return:
    msr daifset,#15
    bl clean_data_cache
    ic iallu
    dsb sy
    isb
    ldr x0,=0x08001004
    mov w1,#0x20
    str w1,[x0]
    str wzr,[x0,#4]
    dsb sy
    mov x0,#2
    msr rmr_el3,x0
    isb
2:  wfi
    b 2b
.size npu_finite_rom_return,.-npu_finite_rom_return
.section .note.GNU-stack,"",%progbits
