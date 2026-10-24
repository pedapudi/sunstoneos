#include "return.h"
#include "a733_npu_application.h"
#include "aarch64_device_memory.h"
#include "platform.h"
#include "boot.h"
#include "result.h"
#include "cached.h"
#ifdef SUNSTONE_NPU_FINITE_THERMAL
const uint64_t npu_finite_thermal_profile[4]={NPU_FINITE_RECEIPT_MAGIC,1,160,2};
#endif
struct npu_finite_receipt npu_finite_receipt __attribute__((section(".npu_runtime_retained")));
/* Normal return requires either the finite client quiescence proof or the
 * separately compiled never-started checkpoint. Failure publishes no receipt. */
_Noreturn void platform_exit(unsigned status)
{
    uint64_t now=a733_smp_counter();
#ifdef SUNSTONE_NPU_ENTRY_CHECKPOINT
    if(status || !npu_entry_checkpoint_ready(a733_observe_identity.epoch,
       a733_observe_identity.generation,a733_observe_identity.lease,
       a733_observe_identity.armed_at,a733_observe_identity.deadline) ||
#else
    if(status>
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
       now>=a733_observe_identity.deadline || aarch64_device_cache_stride()!=64)npu_finite_smc(4);
    __asm__ volatile("msr daifset,#15\n msr cntp_ctl_el0,xzr\n msr cntv_ctl_el0,xzr\n isb":::"memory");
    uintptr_t start=(uintptr_t)__npu_clean_start,end=(uintptr_t)__device_pool_end;
    if(start!=UINT64_C(0x41200000) || end<=start || end>UINT64_C(0x41f00000) ||
       ((start|end)&4095))npu_finite_smc(4);
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
        )npu_finite_smc(4);
        __asm__ volatile("dc cvac,%0"::"r"(at):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
    if(a733_smp_counter()>=a733_observe_identity.deadline)npu_finite_smc(4);
#ifdef SUNSTONE_NPU_FINITE_THERMAL
    if(a733_smp_counter()>=npu_finite_receipt.thermal_deadline)npu_finite_smc(4);
#endif
    npu_finite_smc(status);
}
