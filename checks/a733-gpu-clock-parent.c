#include "a733_gpu_clock_parent.h"
/* The reused main relies on C's implicit main return. It is never called. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main existing_gpu_resource_checks
#include "a733-gpu-resources.c"
#undef main
#pragma GCC diagnostic pop

static struct a733_clock_leases registry;
static struct a733_gpu_clock_parent parent;
static void setup_parent(void)
{
    fresh();memset(&registry,0,sizeof(registry));memset(&parent,0,sizeof(parent));
    assert(a733_clock_leases_init(&registry,now,&model));
    assert(a733_gpu_clock_parent_init(&parent,&registry,&manager,15,99,10000));
}
int main(void)
{
    setup_parent();assert(a733_gpu_clock_parent_poll(&parent)==0);
    assert(parent.common.id && !parent.token);
    assert(a733_gpu_clock_parent_poll(&parent)==1 && parent.token && !model.updates);
    uint64_t token=0;struct a733_clock_token audio={0};
    assert(a733_gpu_resources_change(&manager,2,A733_GPU_RESOURCE_PARENT,&token)==A733_GPU_RESOURCE_BUSY);
    assert(a733_gpu_resources_change(&manager,2,A733_GPU_RESOURCE_OSC,&token)==A733_GPU_RESOURCE_BUSY);
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_REF,10000,&audio)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_AUDIO_CHANGE,10000,&audio)==A733_CLOCK_OK);
    assert(acquire(0,A733_GPU_RESOURCE_ALL,&token)==A733_GPU_RESOURCE_OK);
    assert(a733_gpu_resources_release(&manager,0,token,10000)==A733_GPU_RESOURCE_OK);
    assert(manager.leases[15].token==parent.token && parent.common.id);
    assert(acquire(0,A733_GPU_RESOURCE_ALL,&token)==A733_GPU_RESOURCE_OK);
    unsigned before=model.calls;assert(a733_gpu_clock_parent_poll(&parent)==1 && before==model.calls);

    setup_parent();atomic_store(&registry.busy,1);
    assert(a733_gpu_clock_parent_poll(&parent)==0 && !parent.common.id);
    atomic_store(&registry.busy,0);assert(a733_gpu_clock_parent_poll(&parent)==0);
    atomic_store(&manager.busy,1);assert(a733_gpu_clock_parent_poll(&parent)==0 && parent.common.id && !parent.token);
    atomic_store(&manager.busy,0);assert(a733_gpu_clock_parent_poll(&parent)==1);

    setup_parent();assert(a733_gpu_clock_parent_poll(&parent)==0);
    *reg(&model,0x020020a0)^=1u<<8;
    assert(a733_gpu_clock_parent_poll(&parent)==-1 && parent.token && parent.common.id);
    before=model.calls;assert(a733_gpu_clock_parent_poll(&parent)==-1 && before==model.calls);
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_REF,10000,&audio)==A733_CLOCK_BUSY);

    setup_parent();assert(a733_gpu_clock_parent_poll(&parent)==0);
    model.now=10000;assert(a733_gpu_clock_parent_poll(&parent)==-1 && parent.common.id && !parent.token);
    before=model.calls;assert(a733_gpu_clock_parent_poll(&parent)==-1 && before==model.calls);
    puts("PASS GPU common clock parent: paired leases, legacy change refusal, local reopen, contention and retained faults");
    return 0;
}
