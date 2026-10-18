#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "cached.h"
#include "session.h"
#ifndef SUNSTONE_GPU_SINGLE_CORE
#include "snapshot.h"
#else
#include "a733_device_memory_layout.h"
#endif
#include "aarch64_device_memory.h"
#include <string.h>
/* Immutable producer identity is checked alongside the existing service geometry. */
#ifdef SUNSTONE_GPU_SINGLE_CORE
__attribute__((used,retain)) const uint64_t gpu_single_core_geometry[4]={UINT64_C(0x4153373347505331),1,592,60};
#else
__attribute__((used,retain)) const uint64_t storage_read_single_core_geometry[4]={UINT64_C(0x4153373353444331),1,880,60};
#endif
struct a733_cached_runtime a733_cached_runtime[1];
extern const struct a733_smp_contract *sd_single_core_contract(void);
extern void a733_smp_diagnostic_start(void);
static unsigned preparing;
int a733_smp_device_preparation_admitted(void)
{
    uint64_t el,mpidr,control,flags;
    __asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=4)return 0;
    __asm__ volatile("mrs %0,mpidr_el1\n mrs %1,sctlr_el1\n mrs %2,daif"
      :"=r"(mpidr),"=r"(control),"=r"(flags));
    const struct a733_smp_contract *c=sd_single_core_contract();
#ifdef SUNSTONE_GPU_SINGLE_CORE
    const struct a733_board_session *s=gpu_application_session();
#else
    const struct a733_board_session *s=board_session_snapshot();
    const struct storage_read_session *sd=storage_read_admitted_session();
#endif
    struct aarch64_translation mapping;
    return preparing&&(mpidr&~UINT64_C(0xc1000000))==0&&
      (control&0x1005)==0x1005&&(flags&0x3c0)==0x3c0&&
      xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED&&counter()<c->startup_deadline&&
#ifdef SUNSTONE_GPU_SINGLE_CORE
      s&&s->epoch==c->epoch&&
#else
      s&&sd&&!memcmp(s,&sd->board,sizeof(*s))&&
#endif
      sd_single_core_secondaries_admitted()&&
      a733_cached_runtime[0].epoch==c->epoch&&
      a733_cached_runtime[0].startup_deadline==c->startup_deadline&&
      a733_cached_runtime[0].diagnostic_deadline==c->diagnostic_deadline&&
      aarch64_device_translate(0x04020000,1,&mapping)&&mapping.physical==0x04020000&&
      mapping.attributes==0&&mapping.shareability==2;
}
void kernel_main(void)
{
    platform_init();
    const struct a733_smp_contract *c=sd_single_core_contract();
#ifdef SUNSTONE_GPU_SINGLE_CORE
    if(counter()>=c->startup_deadline||!gpu_application_session())
#else
    if(counter()>=c->startup_deadline||!board_session_snapshot()||!storage_read_admitted_session())
#endif
        platform_panic("SD single-core admission",1);
    a733_cached_runtime[0]=(struct a733_cached_runtime){c->epoch,c->startup_deadline,c->diagnostic_deadline,GICR_BASE};
    preparing=1;
#ifdef SUNSTONE_GPU_SINGLE_CORE
    int ready;
    do{ready=a733_device_memory_prepare();}while(ready==0);
    if(ready!=1)platform_panic("GPU single-core memory",1);
#endif
    a733_smp_diagnostic_start();
    platform_panic("SD scheduler returned",2);
}
