/* Focused optional-isolation check. Native MMIO remains instruction modeled. */
#include "a733-gpu-backend.c"
static atomic_uint revoked;
unsigned gpu_application_isolation_check(void)
{
    CHECK(!setup());
    const struct a733_gpu_backend_external external={0};
    CHECK(a733_gpu_backend_bind(&backend,&service,&config,&memory,&power,&external));
    service.config=config;service.initialized=1;service.epoch=1;
    CHECK(!enabled());CHECK(config.start.publish(&backend));
    CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED);
    service.queue.phase=PVR_CCB_ACTIVE;
    service.control.phase=PVR_GPU_CONTROL_READY;service.control.queue=&service.queue;
    service.stop=(struct pvr_gpu_stop){.ops=config.stop,.control=&service.control,
        .context=&backend,.deadline=operation_deadline,.phase=PVR_GPU_STOP_CLEAR_IDLE};
    unsigned accesses=mmio_count;
    uint64_t token=power.token;
    enum a733_gpu_platform_phase phase=power.phase;
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_FAULT);
    CHECK(atomic_load(&backend.error)==A733_GPU_BACKEND_ISOLATION);
    CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED&&power.token==token&&power.phase==phase);
    CHECK(backend.phase==A733_GPU_BACKEND_ACTIVE&&provider_calls==0&&mmio_count==accesses);
    CHECK(!config.stop.isolate(&backend));
    CHECK(provider_calls==0&&mmio_count==accesses);
    CHECK(!setup());atomic_init(&revoked,0);
    const struct a733_gpu_backend_external cancellable={.revoked=&revoked};
    CHECK(a733_gpu_backend_bind(&backend,&service,&config,&memory,&power,&cancellable));
    service.config=config;service.initialized=1;service.epoch=1;
    CHECK(!enabled());CHECK(config.start.publish(&backend));
    accesses=mmio_count;token=power.token;phase=power.phase;
    atomic_store_explicit(&revoked,1,memory_order_release);
    CHECK(!config.queue.kick(&backend,0xb00,0));
    CHECK(config.power.poll(&backend)==-1&&!config.stop.sync_write(&backend,0));
    CHECK(atomic_load(&backend.error)==A733_GPU_BACKEND_REVOKED);
    CHECK(mmio_count==accesses&&provider_calls==0&&power.token==token&&power.phase==phase);
    CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED&&backend.phase==A733_GPU_BACKEND_ACTIVE);
    return 0;
}
