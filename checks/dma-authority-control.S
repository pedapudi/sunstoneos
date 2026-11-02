.section .text.boot,"ax"
.global dma_authority_entry
.type dma_authority_entry,%function
dma_authority_entry:
    msr daifset,#15
    mrs x20,vbar_el3
    msr spsel,#1
    ldr x0,=__stack_top
    mov sp,x0
    ldr x0,=__bss_start
    ldr x1,=__bss_end
1:  cmp x0,x1
    b.hs 2f
    str xzr,[x0],#8
    b 1b
2:  ldr x0,=dma_authority_saved_vbar
    str x20,[x0]
    ldr x0,=dma_authority_vectors
    msr vbar_el3,x0
    isb
    bl dma_authority_main
3:  wfe
    b 3b
.size dma_authority_entry,.-dma_authority_entry
.section .text.vectors,"ax"
.balign 2048
dma_authority_vectors:
.rept 16
    b dma_authority_exception
    .balign 128
.endr
dma_authority_exception:
    mov x0,#0x32
    mrs x1,esr_el3
    mrs x2,elr_el3
    mrs x3,far_el3
    b sunstone_probe_fault
