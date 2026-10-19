#include "return.h"
#include "a733_npu_application.h"
#include "aarch64_device_memory.h"
#include "platform.h"
#include "boot.h"
#include "result.h"
#include "cached.h"
struct npu_finite_receipt npu_finite_receipt __attribute__((section(".npu_runtime_retained")));
/* Only a successfully quiesced single-core client may take normal return.
 * Failure calls the same SMC without a receipt; EL3 permanently retains it. */
_Noreturn void platform_exit(unsigned status)
{
    uint64_t now=a733_smp_counter();
    if(status>1 || !a733_npu_application_quiescent() ||
       atomic_load_explicit(&a733_npu_application_result.complete,memory_order_acquire)!=(status?2u:1u) ||
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
        status,status?2u:1u,start,end,(end-start)/64,a733_observe_identity.deadline,RESULT->log_length};
    __asm__ volatile("dsb sy":::"memory");
    for(uintptr_t at=start;at<end;at+=64) {
        if(!(at&4095) && a733_smp_counter()>=npu_finite_receipt.deadline)npu_finite_smc(4);
        __asm__ volatile("dc cvac,%0"::"r"(at):"memory");
    }
    __asm__ volatile("dsb sy":::"memory");
    if(a733_smp_counter()>=a733_observe_identity.deadline)npu_finite_smc(4);
    npu_finite_smc(status);
}
