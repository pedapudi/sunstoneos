#include "return.h"
#include "a733_npu_application.h"
#include "aarch64_device_memory.h"
#include "platform.h"
#include "boot.h"
#include "result.h"
#include "cached.h"
#include "sunstone_crumb.h"
/* A refused return records why before it ends at the monitor, so the first
 * failure is visible in the run's breadcrumb words. Reasons 0xe01..0xe08 name
 * the failed predicate of the first check; 0xf01..0xf04 name a later refusal. */
#define REFUSE(code) do{sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU),0xf00u|(code),__builtin_return_address(0));npu_finite_smc(4);}while(0)
#if defined(SUNSTONE_NPU_STARTUP_ONLY) && !defined(SUNSTONE_NPU_FINITE_THERMAL) && !defined(SUNSTONE_NPU_ENTRY_CHECKPOINT)
static __attribute__((noinline)) _Noreturn void refuse_first(unsigned status,uint64_t now)
{
    const unsigned reason=status>NPU_STARTUP_MAX_STATUS?1:!npu_startup_return_ready()?2:status>1?3:
        !a733_npu_application_quiescent()?4:
        atomic_load_explicit(&a733_npu_application_result.complete,memory_order_acquire)!=NPU_FINITE_COMPLETE(status)?5:
        now>=a733_observe_identity.deadline?6:aarch64_device_cache_stride()!=64?7:8;
    sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU),0xe00u|reason,__builtin_return_address(0));
    npu_finite_smc(4);
}
#else
#define refuse_first(status,now) REFUSE(0)
#endif
#ifdef SUNSTONE_NPU_FINITE_THERMAL
const uint64_t npu_finite_thermal_profile[4]={NPU_FINITE_RECEIPT_MAGIC,1,160,2};
#endif
struct npu_finite_receipt npu_finite_receipt __attribute__((section(".npu_runtime_retained")));
/* Normal return requires either the finite client quiescence proof or the
 * separately compiled never-started checkpoint. Failure publishes no receipt. */
static _Noreturn void finish_return(unsigned status)
{
    __asm__ volatile("msr daifset,#15\n msr cntp_ctl_el0,xzr\n msr cntv_ctl_el0,xzr\n isb":::"memory");
    uintptr_t start=(uintptr_t)__npu_clean_start,end=(uintptr_t)__device_pool_end;
    if(start!=UINT64_C(0x41200000) || end<=start || end>UINT64_C(0x41f00000) ||
       ((start|end)&4095))REFUSE(1);
    /* Both mappings and exclusion were established by the unchanged boot and
     * reservation consumers. The interval includes retained metadata, all
     * task/provider state, active stacks, command buffers and NPU tables. */
    npu_finite_receipt=(struct npu_finite_receipt){NPU_FINITE_RECEIPT_MAGIC,
        a733_observe_identity.epoch,a733_observe_identity.generation,a733_observe_identity.lease,
        status,NPU_FINITE_COMPLETE(status),start,end,(end-start)/64,a733_observe_identity.deadline,RESULT->log_length
#ifdef SUNSTONE_NPU_FINITE_THERMAL
        ,a733_npu_application_result.thermal_reason,a733_npu_application_result.jobs,a733_npu_application_thermal_deadline()
#endif
    };
    __asm__ volatile("dsb sy":::"memory");
    for(uintptr_t at=start;at<end;at+=64) {
        if(!(at&4095) && a733_smp_counter()>=
#ifdef SUNSTONE_NPU_FINITE_THERMAL
           npu_finite_receipt.thermal_deadline
#else
           npu_finite_receipt.deadline
#endif
        )REFUSE(2);
        __asm__ volatile("dc cvac,%0"::"r"(at):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
    if(a733_smp_counter()>=a733_observe_identity.deadline)REFUSE(3);
#ifdef SUNSTONE_NPU_FINITE_THERMAL
    if(a733_smp_counter()>=npu_finite_receipt.thermal_deadline)REFUSE(4);
#endif
    npu_finite_smc(status);
}

#ifdef SUNSTONE_NPU_FAILURE_RETURN
/* Only a quiesced NPU may return after a failure. The monitor accepts status 2
 * with a matching receipt, restores the interrupt controller and hands the
 * shared SRAM back; the failure record is the last line of the platform log.
 * Every other failure keeps the watchdog ending. */
#define NPU_FAILURE_STATUS 2u
__attribute__((used,retain)) const uint64_t npu_failure_return_profile[3]={UINT64_C(0x4e5055464c523031),1,NPU_FAILURE_STATUS};
static unsigned return_refusal(unsigned status,uint64_t now)
{
    return status>NPU_STARTUP_MAX_STATUS?1:!npu_startup_return_ready()?2:status>1?3:
        !a733_npu_application_quiescent()?4:
        atomic_load_explicit(&a733_npu_application_result.complete,memory_order_acquire)!=NPU_FINITE_COMPLETE(status)?5:
        now>=a733_observe_identity.deadline?6:aarch64_device_cache_stride()!=64?7:0;
}
static _Noreturn void failure_return(unsigned reason,uint64_t now)
{
    if(!a733_npu_application_quiescent() || now>=a733_observe_identity.deadline || aarch64_device_cache_stride()!=64) {
        sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU),0xd00u|reason,__builtin_return_address(0));
        npu_finite_smc(4);
    }
    sunstone_crumb_fail(SUNSTONE_CRUMB_FAIL_SITE(NPU),0xc00u|reason,__builtin_return_address(0));
    platform_puts("NPU_FAILURE=");platform_hex(reason);platform_puts("\n");
    atomic_store_explicit(&a733_npu_application_result.complete,NPU_FINITE_COMPLETE(NPU_FAILURE_STATUS),memory_order_release);
    finish_return(NPU_FAILURE_STATUS);
}
#endif
_Noreturn void platform_exit(unsigned status)
{
    uint64_t now=a733_smp_counter();
#ifdef SUNSTONE_NPU_FAILURE_RETURN
    {
        const unsigned reason=status==NPU_FAILURE_STATUS?9u:return_refusal(status,now);
        if(reason)failure_return(reason,now);
    }
#endif
#ifdef SUNSTONE_NPU_ENTRY_CHECKPOINT
    if(status || !npu_entry_checkpoint_ready(a733_observe_identity.epoch,
       a733_observe_identity.generation,a733_observe_identity.lease,
       a733_observe_identity.armed_at,a733_observe_identity.deadline) ||
#else
    if(
#ifdef SUNSTONE_NPU_STARTUP_ONLY
       status>NPU_STARTUP_MAX_STATUS || !npu_startup_return_ready() ||
#endif
       status>
#ifdef SUNSTONE_NPU_FINITE_THERMAL
       2 || now>=a733_npu_application_thermal_deadline() || a733_npu_application_result.jobs>2 || a733_npu_application_result.thermal_reason>3 ||
       (status==1 && !a733_npu_application_result.jobs) ||
       (status==0 && a733_npu_application_result.jobs!=2) ||
       (status==2 && (!a733_npu_application_result.thermal_reason || a733_npu_application_result.jobs==2)) ||
#else
       1 ||
#endif
       !a733_npu_application_quiescent() ||
       atomic_load_explicit(&a733_npu_application_result.complete,memory_order_acquire)!=NPU_FINITE_COMPLETE(status) ||
#endif
       now>=a733_observe_identity.deadline || aarch64_device_cache_stride()!=64)refuse_first(status,now);
    finish_return(status);
}
