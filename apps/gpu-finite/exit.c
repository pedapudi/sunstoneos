#include "boot.h"
#include "a733_range.h"
#include "return.h"
#include "../gpu-render/application.h"
#include "../gpu-render/session.h"
extern unsigned char __gpu_private_start[],__gpu_private_end[];
struct gpu_return_receipt gpu_return_receipt __attribute__((section(".gpu_runtime_retained")));
static _Noreturn void retain(void){gpu_finite_smc(4);}
_Noreturn void platform_exit(unsigned status)
{
    uint64_t el,ctr,control;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,ctr_el0\n mrs %2,sctlr_el1"
        :"=r"(el),"=r"(ctr),"=r"(control));
    if(status||el!=4||(control&0x1005)!=0x1005||((ctr>>16)&15)!=4||
       ((ctr>>24)&15)!=4||!gpu_finite_return_ready()||!gpu_return_allocations()||
       !gpu_return_device_quiet())retain();
    __asm__ volatile("msr daifset,#15\n msr cntp_ctl_el0,xzr\n msr cntv_ctl_el0,xzr\n isb":::"memory");
    uintptr_t start=(uintptr_t)__gpu_private_start,end=((uintptr_t)__gpu_private_end+63)&~(uintptr_t)63;
    uint64_t now=counter(),last=now,deadline=now+2*A733_SMP_COUNTER_HZ;
    uint64_t admitted=app.session.deadline_us*24;
    if(deadline<now||admitted/24!=app.session.deadline_us||now>=admitted||
       start<0x41200000||end<=start||end>0x41f00000||(start&63))retain();
    if(deadline>admitted)deadline=admitted;
    gpu_return_receipt=(struct gpu_return_receipt){GPU_RETURN_RECEIPT_MAGIC,
        app.session.epoch,app.session.generation,app.session.lease,0,1,
        start,end,GPU_RETURN_WB_START,GPU_RETURN_WB_END,(end-start+GPU_RETURN_WB_BYTES)/64,deadline,RESULT->log_length};
    __asm__ volatile("dsb sy":::"memory");
    /* Both intervals are accessed through the existing Nonsecure EL1 mapping.
     * The five exact DEVICE parents are checked before touching their contents. */
    for(unsigned interval=0;interval<2;++interval){
        uintptr_t lo=interval?GPU_RETURN_WB_START:start,hi=interval?GPU_RETURN_WB_END:end;
        for(uintptr_t p=lo;p<hi;p+=64){
            now=counter();if(!a733_deadline_admit(now,&last,deadline))retain();
            __asm__ volatile("dc cvac,%0"::"r"(p):"memory");
        }
    }
    __asm__ volatile("dsb sy":::"memory");
    now=counter();if(!a733_deadline_admit(now,&last,deadline))retain();
    gpu_finite_smc(0);
}
