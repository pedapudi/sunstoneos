/* The ordinary native provider storage is present, but its constructors are
 * unreachable. Linking this unit replaces native.c and the diagnostic main. */
#include "../npu-convolution/native.c"
#include "checkpoint.h"
#include "a733_memory_reservations.h"
#include "boot.h"
#include "cached.h"
struct a733_npu_application_result a733_npu_application_result;
__attribute__((used,retain)) const uint64_t npu_entry_checkpoint_profile[4]={
    NPU_ENTRY_RECEIPT_MAGIC,1,NPU_ENTRY_MEMORY_READY,0};
__attribute__((used,retain)) const uint64_t npu_entry_checkpoint_mbus_profile[4]={
    NPU_ENTRY_MBUS_MAGIC,1,3,0};
uint64_t npu_entry_checkpoint_mbus[4] __attribute__((section(".npu_runtime_retained")));
static struct {
    uint64_t complete,epoch,generation,lease,armed_at,deadline;
    struct device_memory_pool *pool;
} checkpoint;
extern unsigned char __npu_clean_start[],__device_pool_start[],__device_pool_end[];
extern unsigned char __uncached_pool_start[],__uncached_pool_end[];
extern const struct a733_smp_contract *sd_single_core_contract(void);
static int published(const void *p,size_t bytes)
{
    uintptr_t at=(uintptr_t)p,lo=(uintptr_t)__npu_clean_start,hi=(uintptr_t)__device_pool_start;
    return bytes && at>=lo && at<hi && bytes<=hi-at;
}
static int zero_bytes(const void *p,size_t bytes)
{
    const unsigned char *b=p;unsigned value=0;
    for(size_t i=0;i<bytes;++i)value|=b[i];
    return value==0;
}
int npu_entry_checkpoint_ready(uint64_t epoch,uint64_t generation,uint64_t lease,
    uint64_t armed_at,uint64_t deadline)
{
    const struct npu_session *s=npu_session_get();
    const struct a733_smp_contract *c=sd_single_core_contract();
    if(!published(&checkpoint,sizeof(checkpoint)))return 0;
    const struct device_memory_pool *p=checkpoint.pool;
    return published(npu_entry_checkpoint_mbus,sizeof(npu_entry_checkpoint_mbus)) &&
        npu_entry_checkpoint_mbus[0]==NPU_ENTRY_MBUS_MAGIC &&
        npu_entry_checkpoint_mbus[1]<=UINT32_MAX && npu_entry_checkpoint_mbus[3]<=UINT32_MAX &&
        (npu_entry_checkpoint_mbus[2]==0 || (npu_entry_checkpoint_mbus[2]>=8 && npu_entry_checkpoint_mbus[2]<=10)) &&
        published(s,sizeof(*s)) &&
        published(c,sizeof(*c)) && published(&app,sizeof(app)) &&
        published(&a733_npu_application_result,sizeof(a733_npu_application_result)) &&
        published(&a733_npu_application_failed,sizeof(a733_npu_application_failed)) &&
        published(&a733_observe_identity,sizeof(a733_observe_identity)) &&
        published(a733_cached_runtime,sizeof(a733_cached_runtime[0])) && published(p,sizeof(*p)) &&
        checkpoint.complete==NPU_ENTRY_MEMORY_READY && epoch && generation && lease && armed_at<deadline &&
        checkpoint.epoch==epoch && checkpoint.generation==generation && checkpoint.lease==lease &&
        checkpoint.armed_at==armed_at && checkpoint.deadline==deadline &&
        s->epoch==epoch && s->generation==generation && s->lease==lease &&
        c->epoch==epoch && c->generation==generation && c->lease==lease &&
        c->armed_at==armed_at && c->diagnostic_deadline==deadline &&
        a733_observe_identity.epoch==epoch && a733_observe_identity.generation==generation &&
        a733_observe_identity.lease==lease && a733_observe_identity.armed_at==armed_at &&
        a733_observe_identity.deadline==deadline && a733_cached_runtime[0].epoch==epoch &&
        a733_cached_runtime[0].startup_deadline==c->startup_deadline &&
        a733_cached_runtime[0].diagnostic_deadline==deadline &&
        zero_bytes(&app,sizeof(app)) && zero_bytes(&a733_npu_application_result,sizeof(a733_npu_application_result)) &&
        zero_bytes(&a733_npu_application_failed,sizeof(a733_npu_application_failed)) &&
        (uintptr_t)__uncached_pool_start==(uintptr_t)__uncached_pool_end &&
        p->initialized==1 && p->backing.cpu==(uintptr_t)__device_pool_start &&
        p->backing.physical==(uintptr_t)__device_pool_start &&
        p->backing.bytes==(size_t)(__device_pool_end-__device_pool_start) &&
        p->next_generation==1 && p->clean_end==0 && p->memory_class==DEVICE_MEMORY_CLASS_NONE &&
        p->memory_type==DEVICE_MEMORY_WRITE_BACK && zero_bytes(p->allocations,sizeof(p->allocations));
}
void a733_smp_diagnostic_start(void)
{
    const struct a733_smp_contract *c=sd_single_core_contract();
    if(!a733_smp_device_preparation_admitted() || !npu_session_copy() ||
       !a733_npu_application_observer_admit() || xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED ||
       checkpoint.complete || !published(c,sizeof(*c)))platform_panic("NPU checkpoint admission",1);
    checkpoint.pool=a733_memory_reservations_init();
    checkpoint.epoch=c->epoch;checkpoint.generation=c->generation;checkpoint.lease=c->lease;
    checkpoint.armed_at=c->armed_at;checkpoint.deadline=c->diagnostic_deadline;
    checkpoint.complete=NPU_ENTRY_MEMORY_READY;
    platform_puts("NPU_CHECKPOINT=ENTRY_MEMORY_READY; providers never constructed; jobs=0\n");
    platform_puts("NPU_MBUS=\n");
    for(unsigned i=1;i<4;++i){platform_hex(npu_entry_checkpoint_mbus[i]);platform_puts("\n");}
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
