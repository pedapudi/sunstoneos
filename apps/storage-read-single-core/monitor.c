#include "boot.h"
#ifdef SUNSTONE_CACHED_SINGLE_CORE
#include "session.h"
#include "a733_device_memory_layout.h"
#include "a733_cached_memory.h"
#else
#include "storage_read_session.h"
#include "session.h"
#include "snapshot.h"
#endif
#include "a733_storage_digest.h"
#include <string.h>
#ifdef SUNSTONE_NPU_INTERRUPT
#include "../npu-interrupt/irq-route.h"
#endif
#define REPORT ((volatile struct a733_smp_result *)A733_SMP_RESULT_ADDRESS)
#ifdef SUNSTONE_GPU_SINGLE_CORE
static struct gpu_boot_contract gpu_admission __attribute__((section(".monitor_state")));
/* EL1 cannot read monitor RAM. Keep only its bounded immutable runtime view
 * in the GPU linker's separately cleared retained RAM. */
static struct a733_smp_contract gpu_runtime_contract __attribute__((section(".gpu_runtime_retained")));
static unsigned gpu_runtime_secondary_off __attribute__((section(".gpu_runtime_retained")));
#define admission gpu_admission.base
const uint8_t *gpu_application_contract_digest(const struct a733_smp_contract *c)
{return c==&admission?gpu_admission.session_digest:NULL;}
const uint8_t *gpu_application_entry_digest(const struct a733_smp_contract *c)
{return c==&admission?gpu_admission.entry_digest:NULL;}
#elif defined(SUNSTONE_THERMAL_SINGLE_CORE)
static struct a733_smp_contract admission __attribute__((section(".monitor_state")));
static struct a733_smp_contract thermal_runtime_contract __attribute__((section(".thermal_runtime_retained")));
static unsigned thermal_runtime_secondary_off __attribute__((section(".thermal_runtime_retained")));
#elif defined(SUNSTONE_BOARD_SINGLE_CORE)
static struct a733_smp_contract admission __attribute__((section(".monitor_state")));
static struct a733_smp_contract board_runtime_contract __attribute__((section(".board_runtime_retained")));
static unsigned board_runtime_secondary_off __attribute__((section(".board_runtime_retained")));
#elif defined(SUNSTONE_RTC_SINGLE_CORE)
static struct a733_smp_contract admission __attribute__((section(".monitor_state")));
static struct a733_smp_contract rtc_runtime_contract __attribute__((section(".rtc_runtime_retained")));
static unsigned rtc_runtime_secondary_off __attribute__((section(".rtc_runtime_retained")));
#elif defined(SUNSTONE_NPU_SINGLE_CORE)
static struct a733_smp_contract admission __attribute__((section(".monitor_state")));
static struct a733_smp_contract npu_runtime_contract __attribute__((section(".npu_runtime_retained")));
static unsigned npu_runtime_secondary_off __attribute__((section(".npu_runtime_retained")));
#elif defined(SUNSTONE_PWM_SINGLE_CORE)
static struct a733_smp_contract admission __attribute__((section(".monitor_state")));
static struct a733_smp_contract pwm_runtime_contract __attribute__((section(".pwm_runtime_retained")));
static unsigned pwm_runtime_secondary_off __attribute__((section(".pwm_runtime_retained")));
#elif defined(SUNSTONE_CRYPTO_SINGLE_CORE)
static struct a733_smp_contract admission __attribute__((section(".monitor_state")));
static struct a733_smp_contract crypto_runtime_contract __attribute__((section(".crypto_runtime_retained")));
static unsigned crypto_runtime_secondary_off __attribute__((section(".crypto_runtime_retained")));
#else
static struct a733_smp_contract admission __attribute__((section(".monitor_state")));
#endif
static unsigned secondary_off_admitted __attribute__((section(".monitor_state")));
static struct {
    uint32_t ready,banks,ctlr,group,modifier,enabled,configuration;
    uint32_t priorities,spi_enabled[32];
#ifdef SUNSTONE_NPU_INTERRUPT
    struct npu_irq_route_snapshot npu_irq;
#endif
} saved __attribute__((section(".monitor_state")));
uint64_t a733_smp_counter(void){return counter();}
_Noreturn void a733_smp_stop(unsigned status,uint64_t detail)
{
    __asm__ volatile("msr daifset,#15\n msr cntps_ctl_el1,xzr\n msr cntp_ctl_el0,xzr\n isb":::"memory");
    REPORT->status=status;REPORT->detail=detail;REPORT->end_counter=counter();
    REPORT->completed_mask=0;__asm__ volatile("dsb sy":::"memory");
    REPORT->stage=5;__asm__ volatile("dsb sy":::"memory");
    /* Keep controller, clocks, pins, supplies, buffers and locks. Only full
     * external power removal may recover this admitted storage lifetime. */
    for(;;)__asm__ volatile("wfe");
}
static _Noreturn void finish(unsigned status){a733_smp_stop(status,status);}
static void wait_clear(uintptr_t a,uint32_t bits)
{
    uint64_t deadline=counter()+COUNTER_HZ/10;
    if(deadline>admission.startup_deadline)deadline=admission.startup_deadline;
    while(mmio_read(a)&bits)if(counter()>=deadline)finish(4);
}
/* Consume the unchanged CPU-start driver's OFF baseline without starting or
 * modifying any CPU. External admission excludes all other power writers. */
static int secondaries_off(void)
{
    for(unsigned c=1;c<8;++c){
        if(counter()>=admission.startup_deadline)return 0;
        __asm__ volatile("dsb sy":::"memory");
        uint32_t status=mmio_read(UINT32_C(0x07052008)+(uintptr_t)c*0x1000);
        uint32_t control=mmio_read(UINT32_C(0x07050200)+c*4);
        uint32_t mode=mmio_read(UINT32_C(0x07050220)+c*4);
        __asm__ volatile("dsb sy":::"memory");
        if((status&15)||(control&1)||(mode&1))return 0;
    }
    return counter()<admission.startup_deadline;
}
#include "a733_el3_security.h"
#include "a733_el3_mbus.h"
void sd_single_core_monitor_init(void)
{
    volatile const struct a733_smp_contract *source=(void *)A733_SMP_CONTRACT_ADDRESS;
    #ifdef SUNSTONE_GPU_SINGLE_CORE
    extern uint64_t __gpu_retained_start[],__gpu_retained_end[];
    for(uint64_t *p=__gpu_retained_start;p<__gpu_retained_end;++p)*p=0;
    for(unsigned i=0;i<sizeof(gpu_admission)/8;++i)((uint64_t *)&gpu_admission)[i]=((const volatile uint64_t *)source)[i];
#else
#ifdef SUNSTONE_THERMAL_SINGLE_CORE
    extern uint64_t __thermal_retained_start[],__thermal_retained_end[];
    for(uint64_t *p=__thermal_retained_start;p<__thermal_retained_end;++p)*p=0;
#endif
#ifdef SUNSTONE_PWM_SINGLE_CORE
    extern uint64_t __pwm_retained_start[],__pwm_retained_end[];
    for(uint64_t *p=__pwm_retained_start;p<__pwm_retained_end;++p)*p=0;
#endif
#ifdef SUNSTONE_CRYPTO_SINGLE_CORE
    extern uint64_t __crypto_retained_start[],__crypto_retained_end[];
    for(uint64_t *p=__crypto_retained_start;p<__crypto_retained_end;++p)*p=0;
#endif
#ifdef SUNSTONE_BOARD_SINGLE_CORE
    extern uint64_t __board_retained_start[],__board_retained_end[];
    for(uint64_t *p=__board_retained_start;p<__board_retained_end;++p)*p=0;
#endif
#ifdef SUNSTONE_RTC_SINGLE_CORE
    extern uint64_t __rtc_retained_start[],__rtc_retained_end[];
    for(uint64_t *p=__rtc_retained_start;p<__rtc_retained_end;++p)*p=0;
#endif
#ifdef SUNSTONE_NPU_SINGLE_CORE
    extern uint64_t __npu_retained_start[],__npu_retained_end[];
    for(uint64_t *p=__npu_retained_start;p<__npu_retained_end;++p)*p=0;
#endif
    for(unsigned i=0;i<sizeof(admission)/8;++i)((uint64_t *)&admission)[i]=((const volatile uint64_t *)source)[i];
#endif
    __asm__ volatile("dsb sy":::"memory");
    if(!a733_smp_contract_valid(&admission,counter())||source->magic!=admission.magic||
       source->generation!=admission.generation||
       (REPORT->magic==A733_SMP_RESULT_MAGIC&&REPORT->generation==admission.generation))finish(1);
    /* The same supervised readback policy as the existing storage boot. */
    if((mmio_read(0x02050014)&1)||(mmio_read(0x02050010)&0x100)||
       mmio_read(0x070501c0)||mmio_read(0x070501d4))finish(1);
    for(unsigned i=0;i<sizeof(*REPORT)/8;++i)((volatile uint64_t *)REPORT)[i]=0;
    REPORT->magic=A733_SMP_RESULT_MAGIC;REPORT->epoch=admission.epoch;
    REPORT->generation=admission.generation;REPORT->status=UINT64_MAX;
    REPORT->stage=1;REPORT->start_counter=counter();
#ifdef SUNSTONE_GPU_SINGLE_CORE
    if(!gpu_application_seal_session(&admission)||!secondaries_off()||
       !a733_device_memory_boot_admit(&admission)||!a733_cached_memory_prepare())finish(1);
#elif defined(SUNSTONE_THERMAL_SINGLE_CORE)
    extern int thermal_session_prepare(const struct a733_smp_contract *);
    if(!secondaries_off()||!board_session_prepare(&admission)||
       !thermal_session_prepare(&admission)||!a733_cached_memory_prepare())finish(1);
#elif defined(SUNSTONE_BOARD_SINGLE_CORE)
    extern int board_identity_prepare(uint64_t);
    if(!secondaries_off()||!board_session_prepare(&admission)||
       !board_identity_prepare(admission.startup_deadline)||!a733_cached_memory_prepare())finish(1);
#elif defined(SUNSTONE_RTC_SINGLE_CORE)
    extern int rtc_session_prepare(const struct a733_smp_contract *);
    if(!secondaries_off()||!board_session_prepare(&admission)||
       !rtc_session_prepare(&admission)||!a733_cached_memory_prepare())finish(1);
#elif defined(SUNSTONE_PWM_SINGLE_CORE)
    extern int pwm_session_prepare(const struct a733_smp_contract *);
    if(!secondaries_off()||!board_session_prepare(&admission)||
       !pwm_session_prepare(&admission)||!a733_cached_memory_prepare())finish(1);
#elif defined(SUNSTONE_CRYPTO_SINGLE_CORE)
    extern int crypto_session_prepare(const struct a733_smp_contract *);
    if(!secondaries_off()||!board_session_prepare(&admission)||
       !crypto_session_prepare(&admission)||!a733_cached_memory_prepare())finish(1);
#elif defined(SUNSTONE_NPU_SINGLE_CORE)
    if(!npu_single_core_seal(&admission)||!secondaries_off()||!a733_cached_memory_prepare())finish(1);
#else
    if(!board_session_prepare(&admission)||!storage_read_session_prepare(&admission)||
       !secondaries_off())finish(1);
#endif
    secondary_off_admitted=1;
#ifdef SUNSTONE_GPU_SINGLE_CORE
    gpu_runtime_contract=admission;gpu_runtime_secondary_off=1;
#elif defined(SUNSTONE_THERMAL_SINGLE_CORE)
    thermal_runtime_contract=admission;thermal_runtime_secondary_off=1;
#elif defined(SUNSTONE_BOARD_SINGLE_CORE)
    board_runtime_contract=admission;board_runtime_secondary_off=1;
#elif defined(SUNSTONE_RTC_SINGLE_CORE)
    rtc_runtime_contract=admission;rtc_runtime_secondary_off=1;
#elif defined(SUNSTONE_NPU_SINGLE_CORE)
    npu_runtime_contract=admission;npu_runtime_secondary_off=1;
#elif defined(SUNSTONE_PWM_SINGLE_CORE)
    pwm_runtime_contract=admission;pwm_runtime_secondary_off=1;
#elif defined(SUNSTONE_CRYPTO_SINGLE_CORE)
    crypto_runtime_contract=admission;crypto_runtime_secondary_off=1;
#endif
    __asm__ volatile("dsb sy":::"memory");
    /* Snapshot helpers hash and publish before any providers. EL3 remains
     * cache-disabled; snapshots survive the accepted EL1 BSS clearing. */
    for(unsigned i=0;i<sizeof(*RESULT)/8;++i)((volatile uint64_t *)RESULT)[i]=0;
    RESULT->magic=RESULT_MAGIC;RESULT->status=UINT64_MAX;
    RESULT->stage=1;RESULT->start_counter=counter();
    __asm__ volatile("msr cntfrq_el0, %0" :: "r"(COUNTER_HZ));
    saved.ready = 0;
    saved.ctlr = mmio_read(GICD_BASE);
    RESULT->gicd_ctlr = saved.ctlr;
    RESULT->gicd_typer = mmio_read(GICD_BASE + 4);
    RESULT->gicr_typer = *(volatile uint64_t *)(GICR_BASE + 8);
    // Keep the two security states and identify the boot CPU redistributor.
    if ((saved.ctlr & (1U << 6)) || (RESULT->gicr_typer >> 32) != 0)
        finish(4);
#ifndef MONITOR_QEMU
    // Require the physically established affinity-routing baseline.
    if ((saved.ctlr & 0x30U) != 0x30U)
        finish(4);
#endif
    saved.banks = (RESULT->gicd_typer & 31) + 1;
    saved.group = mmio_read(GICR_SGI + 0x80);
    saved.modifier = mmio_read(GICR_SGI + 0xd00);
    saved.enabled = mmio_read(GICR_SGI + 0x100);
    saved.configuration = mmio_read(GICR_SGI + 0xc04);
    saved.priorities = mmio_read(GICR_SGI + 0x41c);
    for (unsigned i = 1; i < saved.banks; ++i)
        saved.spi_enabled[i] = mmio_read(GICD_BASE + 0x100 + 4 * i);
#ifdef SUNSTONE_NPU_INTERRUPT
    if(!npu_irq_route_capture(&saved.npu_irq,admission.startup_deadline))finish(4);
#endif
#ifdef SUNSTONE_ACCELERATORS
    /* A retained combined lifetime must begin without an inherited GPU
     * interrupt. Disabling delivery alone cannot retire a pending/active owner. */
    if((mmio_read(GICD_BASE+0x108)|mmio_read(GICD_BASE+0x208)|
        mmio_read(GICD_BASE+0x308))&(1U<<31))finish(4);
#endif
#ifdef SUNSTONE_PWM_SINGLE_CORE
    /* Reject inherited PWM IRQ state before distributor cleanup hides it. */
    if((mmio_read(GICD_BASE+0x108)|mmio_read(GICD_BASE+0x208)|
        mmio_read(GICD_BASE+0x308))&(1u<<3))finish(1);
#endif
    saved.ready = 1;
    // Disable groups before changing routing or per-interrupt configuration.
    mmio_write(GICD_BASE, saved.ctlr & ~7U);
    wait_clear(GICD_BASE, 1U << 31);
    for (unsigned i = 1; i < saved.banks; ++i)
        mmio_write(GICD_BASE + 0x180 + 4 * i, 0xffffffffU);
    wait_clear(GICD_BASE, 1U << 31);
    mmio_write(GICD_BASE, (saved.ctlr & ~7U) | 0x30U);
    wait_clear(GICD_BASE, 1U << 31);
#ifdef SUNSTONE_NPU_INTERRUPT
    if(!npu_irq_route_install(&saved.npu_irq,admission.startup_deadline))finish(4);
#endif
    mmio_write(GICR_BASE + 0x14, mmio_read(GICR_BASE + 0x14) & ~2U);
    wait_clear(GICR_BASE + 0x14, 4);
    mmio_write(GICR_SGI + 0x180, 0xffffffffU);
    wait_clear(GICR_BASE, 8);
    mmio_write(GICR_SGI + 0x280, 0xffffffffU);
    mmio_write(GICR_SGI + 0x380, 0xffffffffU);
    mmio_write(GICR_SGI + 0x80, ~(1U << RECOVERY_IRQ));
    mmio_write(GICR_SGI + 0xd00, 0);
    // Both timer PPIs are level-sensitive.
    mmio_write(GICR_SGI + 0xc04, saved.configuration & ~((2U << 26) | (2U << 28)));
    *(volatile uint8_t *)(GICR_SGI + 0x400 + RECOVERY_IRQ) = 0;
    mmio_write(GICR_SGI + 0x100, 1U << RECOVERY_IRQ);
    wait_clear(GICR_BASE, 8);
    uint64_t value = 0xf;
    __asm__ volatile("msr ICC_SRE_EL3, %0\n isb" :: "r"(value) : "memory");
    value = 0;
    __asm__ volatile("msr ICC_CTLR_EL3, %0\n isb" :: "r"(value) : "memory");
    value = 0xff;
    __asm__ volatile("msr ICC_PMR_EL1, %0" :: "r"(value));
    value = 1;
    __asm__ volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"(value));
    // Group 0 FIQs target EL3 even if the nonsecure kernel masks exceptions.
    value = 0x505;
    __asm__ volatile("msr scr_el3, %0\n isb" :: "r"(value) : "memory");
    value = 0xf;
    __asm__ volatile("msr ICC_SRE_EL2, %0\n isb\n msr ICC_SRE_EL1, %0\n isb"
                     :: "r"(value) : "memory");
    value = 1UL << 31;
    __asm__ volatile("msr hcr_el2, %0" :: "r"(value));
    value = 3;
    __asm__ volatile("msr cnthctl_el2, %0\n msr cntvoff_el2, xzr\n"
                     "msr cptr_el3, xzr" :: "r"(value));
    value = 0x33ff;
    __asm__ volatile("msr cptr_el2, %0" :: "r"(value));
#if defined(SUNSTONE_GPU_SINGLE_CORE) || defined(SUNSTONE_ACCELERATORS)
    /* SPI95 stays disabled until the real service owns its interrupt binding. */
    mmio_write(GICD_BASE+0x88,mmio_read(GICD_BASE+0x88)|(1U<<31));
    mmio_write(GICD_BASE+0xd08,mmio_read(GICD_BASE+0xd08)&~(1U<<31));
    mmio_write(GICD_BASE+0xc14,mmio_read(GICD_BASE+0xc14)&~(3U<<30));
    mmio_write(GICD_BASE+0x45c,(mmio_read(GICD_BASE+0x45c)&0x00ffffff)|0xd8000000);
    *(volatile uint64_t *)(GICD_BASE+0x62f8)=0;
    __asm__ volatile("dsb sy":::"memory");
#ifdef SUNSTONE_ACCELERATORS
    if(!(mmio_read(GICD_BASE+0x88)&(1U<<31))||
       (mmio_read(GICD_BASE+0xd08)&(1U<<31))||
       (mmio_read(GICD_BASE+0xc14)&(3U<<30))||
       (mmio_read(GICD_BASE+0x45c)&0xff000000U)!=0xd8000000U||
       *(volatile uint64_t *)(GICD_BASE+0x62f8)||
       ((mmio_read(GICD_BASE+0x108)|mmio_read(GICD_BASE+0x208)|
         mmio_read(GICD_BASE+0x308))&(1U<<31))||
       counter()>=admission.startup_deadline)finish(4);
#endif
#endif
    mmio_write(GICD_BASE, (saved.ctlr & ~7U) | 0x33U);
    wait_clear(GICD_BASE, 1U << 31);
#ifdef SUNSTONE_RTC_SINGLE_CORE
    extern int rtc_boot_route(void);
    if(!rtc_boot_route())finish(1);
#endif
#ifdef SUNSTONE_THERMAL_SINGLE_CORE
    /* Same Group1NS/level/priority/boot-CPU route as thermal's accepted boot. */
    mmio_write(GICD_BASE+0x88,mmio_read(GICD_BASE+0x88)|(1u<<7));
    mmio_write(GICD_BASE+0xd08,mmio_read(GICD_BASE+0xd08)&~(1u<<7));
    mmio_write(GICD_BASE+0xc10,mmio_read(GICD_BASE+0xc10)&~(3u<<14));
    mmio_write(GICD_BASE+0x444,(mmio_read(GICD_BASE+0x444)&0x00ffffff)|0xd8000000);
    *(volatile uint64_t *)(GICD_BASE+0x6238)=0;
    __asm__ volatile("dsb sy":::"memory");
#endif
#ifdef SUNSTONE_PWM_SINGLE_CORE
    /* Same Group1NS/level/priority/boot-CPU route as pwm's accepted boot. */
    mmio_write(GICD_BASE+0x88,mmio_read(GICD_BASE+0x88)|(1u<<3));
    mmio_write(GICD_BASE+0xd08,mmio_read(GICD_BASE+0xd08)&~(1u<<3));
    mmio_write(GICD_BASE+0xc10,mmio_read(GICD_BASE+0xc10)&~(3u<<6));
    mmio_write(GICD_BASE+0x440,(mmio_read(GICD_BASE+0x440)&0x00ffffff)|0xd8000000);
    *(volatile uint64_t *)(GICD_BASE+0x6218)=0;
    __asm__ volatile("dsb sy":::"memory");
#endif
    value = admission.diagnostic_deadline;
    __asm__ volatile("msr cntps_cval_el1, %0" :: "r"(value));
    value = 1;
    __asm__ volatile("msr cntps_ctl_el1, %0\n isb" :: "r"(value) : "memory");
    /* MBUS back to boot0's 600 MHz (include/a733_el3_mbus.h), then the
     * vendor's cold-boot security setup, the last step before EL1. */
    {unsigned mbus=a733_el3_mbus_restore();if(mbus)a733_smp_stop(2,0x200u|mbus);}
    a733_el3_security_setup();
    RESULT->stage = 2;
    REPORT->stage = 2;
}

_Noreturn void sd_single_core_exception(uint64_t operation,uint64_t status,uint64_t kind)
{
    __asm__ volatile("mrs %0,esr_el3\n mrs %1,elr_el3\n mrs %2,far_el3"
      :"=r"(RESULT->esr),"=r"(RESULT->elr),"=r"(RESULT->far));
    if(kind==2){
        uint64_t irq;__asm__ volatile("mrs %0,ICC_IAR0_EL1":"=r"(irq));
        RESULT->interrupt=irq;
        if(irq<1020)__asm__ volatile("msr ICC_EOIR0_EL1,%0\n isb"::"r"(irq):"memory");
        a733_smp_stop(irq==RECOVERY_IRQ?2:5,irq);
    }
    a733_smp_stop(kind==1&&(RESULT->esr>>26)==0x17&&operation==0x733?(unsigned)status:3,operation);
}
const struct a733_smp_contract *sd_single_core_contract(void)
{
#ifdef SUNSTONE_GPU_SINGLE_CORE
    return &gpu_runtime_contract;
#elif defined(SUNSTONE_THERMAL_SINGLE_CORE)
    return &thermal_runtime_contract;
#elif defined(SUNSTONE_BOARD_SINGLE_CORE)
    return &board_runtime_contract;
#elif defined(SUNSTONE_RTC_SINGLE_CORE)
    return &rtc_runtime_contract;
#elif defined(SUNSTONE_NPU_SINGLE_CORE)
    return &npu_runtime_contract;
#elif defined(SUNSTONE_PWM_SINGLE_CORE)
    return &pwm_runtime_contract;
#elif defined(SUNSTONE_CRYPTO_SINGLE_CORE)
    return &crypto_runtime_contract;
#else
    return &admission;
#endif
}

int sd_single_core_secondaries_admitted(void)
{
#ifdef SUNSTONE_GPU_SINGLE_CORE
    return gpu_runtime_secondary_off==1;
#elif defined(SUNSTONE_THERMAL_SINGLE_CORE)
    return thermal_runtime_secondary_off==1;
#elif defined(SUNSTONE_BOARD_SINGLE_CORE)
    return board_runtime_secondary_off==1;
#elif defined(SUNSTONE_RTC_SINGLE_CORE)
    return rtc_runtime_secondary_off==1;
#elif defined(SUNSTONE_NPU_SINGLE_CORE)
    return npu_runtime_secondary_off==1;
#elif defined(SUNSTONE_PWM_SINGLE_CORE)
    return pwm_runtime_secondary_off==1;
#elif defined(SUNSTONE_CRYPTO_SINGLE_CORE)
    return crypto_runtime_secondary_off==1;
#else
    return secondary_off_admitted==1;
#endif
}
