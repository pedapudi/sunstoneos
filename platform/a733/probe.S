#include "a733_cpu_reset.h"
.section .text.probe_smc,"ax"
.global sunstone_probe_smc
.type sunstone_probe_smc,%function
sunstone_probe_smc:
    mov x1,x0
    msr daifset,#15
    mrs x2,sctlr_el1
    bic x2,x2,#1
    bic x2,x2,#4
    bic x2,x2,#0x1000
    msr sctlr_el1,x2
    isb
    mov x0,#0x733
    smc #0
1:  wfe
    b 1b
.size sunstone_probe_smc,.-sunstone_probe_smc
.section .text.probe_recovery,"ax"
.global sunstone_probe_rom
.type sunstone_probe_rom,%function
sunstone_probe_rom:
    ldr x0,=0x08001004
    mov w1,#0x20
    str w1,[x0]
    str wzr,[x0,#4]
    mov x0,#2
    A733_CPU_RESET_WAIT(x0)
.size sunstone_probe_rom,.-sunstone_probe_rom

.section .text.probe_smc,"ax"
.global platform_exception
.global sunstone_probe_exception
.type platform_exception,%function
.type sunstone_probe_exception,%function
platform_exception:
sunstone_probe_exception:
    msr daifset,#15
    mov x0,#0x735
    mov x1,#0x104
    mrs x2,esr_el1
    mrs x3,elr_el1
    mrs x4,far_el1
    smc #0
1:  wfe
    b 1b
.size sunstone_probe_exception,.-sunstone_probe_exception
.size platform_exception,.-platform_exception
.global sunstone_probe_fail
.type sunstone_probe_fail,%function
sunstone_probe_fail:
    msr daifset,#15
    mov x2,x1
    mov x1,x0
    mov x0,#0x735
    mov x3,x30
    mov x4,#0
    smc #0
1:  wfe
    b 1b
.size sunstone_probe_fail,.-sunstone_probe_fail
