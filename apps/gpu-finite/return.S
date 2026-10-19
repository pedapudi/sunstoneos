/* The EL1 tail never accesses a stack after clearing M/C/I. Final set/way
 * cleanup at EL3 retires late EL1 stack stores in both PA security spaces. */
.section .text.gpu_finite_smc,"ax"
.global gpu_finite_smc
.type gpu_finite_smc,%function
gpu_finite_smc:
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
2:  wfe
    b 2b
.size gpu_finite_smc,.-gpu_finite_smc
.section .text.gpu_return_reset,"ax"
.global gpu_return_reset
.type gpu_return_reset,%function
gpu_return_reset:
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
1:  wfi
    b 1b
.size gpu_return_reset,.-gpu_return_reset
.section .note.GNU-stack,"",%progbits
