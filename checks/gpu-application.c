/* Native memory/backend composition with explicitly modeled power operations. */
#include "a733-gpu-backend.c"
static atomic_uint revoked;
static int native_ready(void)
{
    CHECK(!setup());
    const struct a733_gpu_backend_external external={0};
    CHECK(a733_gpu_backend_bind(&backend,&service,&config,&memory,&power,&external));
    service.config=config;service.initialized=1;service.epoch=1;
    CHECK(!enabled());CHECK(config.start.publish(&backend));
    service.queue.phase=PVR_CCB_RETIRED;
    service.control.phase=PVR_GPU_CONTROL_RETIRED;service.control.queue=&service.queue;
    service.stop=(struct pvr_gpu_stop){.ops=config.stop,.control=&service.control,
        .context=&backend,.deadline=operation_deadline,.phase=PVR_GPU_STOP_ISOLATE};
    return 0;
}
unsigned gpu_application_isolation_check(void)
{
    CHECK(!native_ready());
    unsigned accesses=mmio_count;
    CHECK(config.stop.isolate_poll);
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_AGAIN);
    CHECK(provider_calls==1&&service.stop.isolation_started&&power.token);
    CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED&&power.phase==A733_GPU_PLATFORM_STOPPING);
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_AGAIN&&provider_calls==2);
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_OK&&provider_calls==3);
    CHECK(memory.phase==A733_GPU_MEMORY_OFFLINE&&power.phase==A733_GPU_PLATFORM_OFF);
    CHECK(!power.token&&!power.delegated&&backend.phase==A733_GPU_BACKEND_ISOLATED);
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_OK&&provider_calls==3&&mmio_count==accesses);
    for(unsigned mode=1;mode<=2;++mode) {
        CHECK(!native_ready());off_mode=mode;
        uint64_t token=power.token;
        if(mode==2)CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_AGAIN);
        CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_FAULT);
        CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED&&power.token==token);
        unsigned calls=provider_calls;
        CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_FAULT&&provider_calls==calls);
    }
    CHECK(!native_ready());
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_AGAIN);
    service.stop.deadline=1;
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_FAULT&&provider_calls==1);
    CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED&&power.token);
    CHECK(!native_ready());off_mode=3;
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_AGAIN);
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_AGAIN);
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_FAULT);
    CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED&&!power.token&&!power.delegated);
    CHECK(pvr_gpu_stop_poll(&service.stop)==PVR_GPU_STOP_FAULT&&provider_calls==3);
    /* A fresh ACK must agree with the independently read firmware state. */
    for(unsigned phase=PVR_GPU_STOP_WAIT_IDLE;phase<=PVR_GPU_STOP_WAIT_OFF;phase+=3) {
        for(unsigned wrong=0;wrong<3;++wrong) {
            CHECK(!native_ready());service.stop.phase=phase;
            *(uint32_t *)region(PVR_FW_POWER_SYNC)=1;
            unsigned expected=phase==PVR_GPU_STOP_WAIT_IDLE?2:0;
            *(uint32_t *)((uintptr_t)region(PVR_FW_SYSTEM_DATA)+8)=wrong==0?expected:expected+1;
            if(wrong==2)*(uint32_t *)region(PVR_FW_POWER_SYNC)=2;
            uint32_t value=99;
            CHECK(config.stop.sync_read(&backend,&value)==(wrong==0));
            CHECK(wrong==0?value==1:value==99);
            CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED&&power.token&&provider_calls==0);
        }
    }
    CHECK(!native_ready());atomic_init(&revoked,0);backend.external.revoked=&revoked;
    accesses=mmio_count;uint64_t token=power.token;
    atomic_store_explicit(&revoked,1,memory_order_release);
    CHECK(!config.queue.kick(&backend,0xb00,0));
    CHECK(config.power.poll(&backend)==-1&&!config.stop.sync_write(&backend,0));
    CHECK(atomic_load(&backend.error)==A733_GPU_BACKEND_REVOKED);
    CHECK(mmio_count==accesses&&provider_calls==0&&power.token==token);
    CHECK(memory.phase==A733_GPU_MEMORY_PUBLISHED&&backend.phase==A733_GPU_BACKEND_ACTIVE);
    return 0;
}
