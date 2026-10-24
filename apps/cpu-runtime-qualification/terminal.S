/* Current-port success terminal and control preservation. No interrupted
 * stack access occurs on the fatal path. Success has a distinct SVC and SMC
 * PC; only that path selects a dedicated Secure Device cleanup stack. */
#include "runtime.h"
#include "a733_fault.h"
.section .text.qualification,"ax"
.balign 4
.global a733_qualification_terminal_el0, a733_qualification_terminal_after
.global a733_qualification_terminal_privileged, a733_qualification_success_dispatch
.global a733_qualification_terminal_smc, a733_qualification_terminal_smc_after
.global a733_qualification_system_save, a733_qualification_system_restore
.global a733_qualification_power_down, a733_qualification_rom

.macro address out, symbol
    adrp \out, \symbol
    add \out, \out, :lo12:\symbol
.endm
.macro check_time
    isb
    mrs x9, cntpct_el0
    ldr x10, [x23, #32]
    cmp x9, x10
    b.hs qualification_park
    ldr x10, [x23, #48]
    cbnz x10, qualification_park
.endm

a733_qualification_terminal_el0:
    svc #0x735
a733_qualification_terminal_after:
    b a733_qualification_terminal_after

a733_qualification_terminal_privileged:
    // The production vector has classified a 64-bit SVC and restored x0/x1.
    mrs x1, elr_el1
    address x2, a733_qualification_terminal_after
    cmp x1, x2
    b.ne a733_cached_el1_fault
    mrs x1, tpidr_el1
    cmp x0, x1
    b.ne a733_cached_el1_fault
    cmp x0, #8
    b.hs a733_cached_el1_fault
    // Interrupts are masked on synchronous exception entry. Stop generation
    // and delivery before entering the real stackless EL3 exit recorder.
    msr daifset, #15
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr
    msr icc_igrpen1_el1, xzr
    dsb sy
    isb
    mov x0, #0
    mov x1, #0
    mov x2, #0
    b a733_qualification_terminal_smc
a733_qualification_terminal_smc:
    smc #0
a733_qualification_terminal_smc_after:
    b a733_qualification_terminal_smc_after

a733_qualification_success_dispatch:
    // Native handler preserved status/detail/marker/core in x20/x21/x22/x19.
    // A normal exit0, another SMC or any vector fault must stay retained.
    cbnz x20, qualification_park
    cbnz x21, qualification_park
    cbnz x22, qualification_park
    cmp x19, #8
    b.hs qualification_park
    mrs x0, elr_el3
    address x1, a733_qualification_terminal_smc_after
    cmp x0, x1
    b.ne qualification_park
    mrs x0, elr_el1
    address x1, a733_qualification_terminal_after
    cmp x0, x1
    b.ne qualification_park
    mrs x0, esr_el1
    and x0, x0, #0xffff
    mov x1, #0x735
    cmp x0, x1
    b.ne qualification_park
    // The C cleanup stack must be inside current Secure Device monitor RAM.
    mrs x2, ttbr0_el3
    address x3, a733_cached_el3_l1
    cmp x2, x3
    b.ne qualification_park
    address x2, a733_cached_el3_l2
    ldr x2, [x2, #128]
    address x3, a733_cached_el3_l3
    orr x3, x3, #3
    cmp x2, x3
    b.ne qualification_park
    address x1, a733_qualification_stacks
    add x1, x1, x19, lsl #12
    address x2, __monitor_bss_start
    cmp x1, x2
    b.lo qualification_park
    add x2, x1, #4096
    address x3, __monitor_bss_end
    cmp x2, x3
    b.hi qualification_park
    mov x2, #0x42000000
    sub x2, x1, x2
    lsr x2, x2, #12
    address x3, a733_cached_el3_l3
    ldr x3, [x3, x2, lsl #3]
    mov x4, #0x7ff
    and x4, x3, x4
    mov x5, #0x443
    cmp x4, x5
    b.ne qualification_park
    ubfx x3, x3, #12, #36
    lsr x4, x1, #12
    cmp x3, x4
    b.ne qualification_park
    add x1, x1, #4096
    mov sp, x1
    mov x0, x19
    b a733_qualification_captured

.macro save reg, index
    mrs x2, \reg
    str x2, [x0, #(\index*8)]
.endm
.macro restore reg, index, keep=0
    ldr x2, [x21, #(\index*8)]
    .if \keep
    mrs x3, \reg
    and x3, x3, #\keep
    orr x2, x2, x3
    .endif
    msr \reg, x2
    isb
    mrs x3, \reg
    cmp x2, x3
    b.ne qualification_park
.endm

a733_qualification_system_save:
    save sctlr_el3, 0
    save tcr_el3, 1
    save ttbr0_el3, 2
    save mair_el3, 3
    save scr_el3, 4
    save mdcr_el3, 5
    save cptr_el3, 6
    save icc_sre_el3, 7
    mrs x2, icc_sre_el3
    orr x2, x2, #15
    msr icc_sre_el3, x2
    isb
    save icc_ctlr_el3, 8
    save icc_igrpen0_el1, 9
    save icc_igrpen1_el3, 10
    save icc_pmr_el1, 11
    ldr x2, [x0, #32]
    bic x2, x2, #1
    msr scr_el3, x2
    isb
    save sctlr_el1, 12
    save tcr_el1, 13
    save ttbr0_el1, 14
    save ttbr1_el1, 15
    save mair_el1, 16
    save vbar_el1, 17
    save cpacr_el1, 18
    save cntkctl_el1, 19
    save tpidr_el1, 20
    save icc_sre_el1, 21
    // Reload SCR: the save macro clobbers x2.
    ldr x2, [x0, #32]
    orr x2, x2, #1
    msr scr_el3, x2
    isb
    save sctlr_el2, 22
    save hcr_el2, 23
    save hstr_el2, 24
    save mdcr_el2, 25
    save vttbr_el2, 26
    save cptr_el2, 27
    save cnthctl_el2, 28
    save cntvoff_el2, 29
    save icc_sre_el2, 30
    save sctlr_el1, 31
    save tcr_el1, 32
    save ttbr0_el1, 33
    save ttbr1_el1, 34
    save mair_el1, 35
    save vbar_el1, 36
    save cpacr_el1, 37
    save cntkctl_el1, 38
    save tpidr_el1, 39
    save icc_sre_el1, 40
    save icc_ctlr_el1, 41
    save icc_bpr1_el1, 42
    save sp_el0, 43
    save sp_el1, 44
    save tpidrro_el0, 45
    save cntp_ctl_el0, 46
    save cntp_cval_el0, 47
    save cntv_ctl_el0, 48
    save cntv_cval_el0, 49
    ldr x2, [x0, #32]
    msr scr_el3, x2
    isb
    ret

a733_qualification_system_restore:
    mov x20, x0
    mov x21, x1
    address x23, a733_qualification_report
    // Current EL3 translation must still select the production NS WB alias.
    mrs x2, sctlr_el3
    mov x3, #5
    and x2, x2, x3
    cmp x2, #5
    b.ne qualification_park
    mrs x2, ttbr0_el3
    address x3, a733_cached_el3_l1
    cmp x2, x3
    b.ne qualification_park
    mov x24, #0x41200000
    address x25, __bss_end
    add x25, x25, #63
    and x25, x25, #-64
    mov x2, #0x42000000
    cmp x25, x2
    b.hi qualification_park
    // Linker RAM is mapped by 2MiB blocks. Require the NS bit, WB index1,
    // inner-shareable field and access flag before cleaning each block.
    address x26, a733_cached_el3_l2
.Lclean:
    check_time
    mov x2, #0x40000000
    sub x2, x24, x2
    lsr x2, x2, #21
    ldr x3, [x26, x2, lsl #3]
    ubfx x4, x3, #21, #27
    lsr x5, x24, #21
    cmp x4, x5
    b.ne qualification_park
    mov x4, #0x73f
    and x3, x3, x4
    mov x4, #0x725
    cmp x3, x4
    b.ne qualification_park
    dc civac, x24
    add x24, x24, #64
    cmp x24, x25
    b.lo .Lclean
    dsb sy
    add x2, x23, #688
    add x2, x2, x20, lsl #10
    ldr x3, [x23, #24]
    str x3, [x2, #16]
    dsb sy
    mov x0, x20
    bl a733_qualification_restore_local
    cmp w0, #1
    b.ne qualification_park
    // The current fault entry already blocked EL1 walks. Restore NS banks
    // first, then Secure EL1, before disabling EL3 translation.
    mrs x2, scr_el3
    orr x2, x2, #1
    msr scr_el3, x2
    isb
    mrs x2, sctlr_el1
    bic x2, x2, #4
    bic x2, x2, #0x1000
    msr sctlr_el1, x2
    isb
    tlbi vmalle1
    dsb sy
    tlbi vmalle1
    dsb sy
    restore ttbr0_el1, 33
    restore ttbr1_el1, 34
    restore mair_el1, 35
    restore vbar_el1, 36
    restore cpacr_el1, 37
    restore cntkctl_el1, 38
    restore tpidr_el1, 39
    restore icc_sre_el1, 40, 1
    restore icc_ctlr_el1, 41
    restore icc_bpr1_el1, 42
    restore sp_el0, 43
    restore sp_el1, 44
    restore tpidrro_el0, 45
    restore cntp_ctl_el0, 46
    restore cntp_cval_el0, 47
    restore cntv_ctl_el0, 48
    restore cntv_cval_el0, 49
    restore sctlr_el1, 31
    restore tcr_el1, 32
    restore sctlr_el2, 22
    restore hcr_el2, 23
    restore hstr_el2, 24
    restore mdcr_el2, 25
    restore vttbr_el2, 26
    restore cptr_el2, 27
    restore cnthctl_el2, 28
    restore cntvoff_el2, 29
    restore icc_sre_el2, 30, 1
    mrs x2, scr_el3
    bic x2, x2, #1
    msr scr_el3, x2
    isb
    restore ttbr0_el1, 14
    restore ttbr1_el1, 15
    restore mair_el1, 16
    restore vbar_el1, 17
    restore cpacr_el1, 18
    restore cntkctl_el1, 19
    restore tpidr_el1, 20
    restore icc_sre_el1, 21, 1
    restore sctlr_el1, 12
    restore tcr_el1, 13
    restore icc_ctlr_el3, 8
    restore icc_igrpen0_el1, 9
    restore icc_igrpen1_el3, 10
    restore icc_pmr_el1, 11
    restore mdcr_el3, 5
    restore cptr_el3, 6
    // EL3 was admitted with M/C/I clear. Saved controls restore only after
    // the full NS clean; subsequent code and stacks use Secure Device data.
    ldr x2, [x21]
    mov x3, #0x1005
    tst x2, x3
    b.ne qualification_park
    dsb sy
    msr sctlr_el3, x2
    isb
    tlbi alle3
    dsb sy
    tlbi alle3
    dsb sy
    ic iallu
    dsb sy
    isb
    restore tcr_el3, 1
    restore ttbr0_el3, 2
    restore mair_el3, 3
    restore scr_el3, 4
    // Keep all enabled SRE bits; a 1->0 transition at EL3/EL2/Secure EL1
    // is architecturally unpredictable (IHI0069G12.1.6).
    mov x0, x20
    b a733_qualification_off

a733_qualification_power_down:
    // x0 core, x1 epoch; use only Device report/register stores from here.
    address x2, a733_qualification_report
    add x2, x2, #688
    add x2, x2, x0, lsl #10
    mov x3, #0x07050000
    add x3, x3, #0x200
    add x3, x3, x0, lsl #2
    mrs x4, S3_0_C15_C2_7
    orr x4, x4, #1
    msr S3_0_C15_C2_7, x4
    dsb sy
    isb
    ldr w4, [x3, #32]
    bic w4, w4, #1
    str w4, [x3, #32]
    dsb sy
    ldr w4, [x3]
    orr w4, w4, #2
    str w4, [x3]
    dsb sy
    str x1, [x2, #40]
    dsb sy
    b qualification_park

a733_qualification_rom:
    mov x0, #0x08000000
    add x0, x0, #0x1000
    mov w1, #0x20
    str w1, [x0, #4]
    str wzr, [x0, #8]
    dsb sy
    mov x0, #2
    msr rmr_el3, x0
    isb
qualification_park:
    wfi
    b qualification_park
.section .note.GNU-stack,"",%progbits
