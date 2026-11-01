/* SPDX-License-Identifier: MIT */
#include "peer.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#define PEER_ELEMENTS 65u
#define PEER_OFFSET 1024u
#define PEER_WAIT_TICKS 4000u
#define PEER_ACCEPT_TICKS 50u
static atomic_uint released,accepted,completed;
void gpu_queue_peer_release(void)
{atomic_store_explicit(&released,1,memory_order_release);}
int gpu_queue_peer_accepted(uint32_t ticks)
{
    TickType_t began=xTaskGetTickCount();
    while (!atomic_load_explicit(&accepted,memory_order_acquire)) {
        if (ss_live()<0 || xTaskGetTickCount()-began>=ticks) return 0;
        ss_progress();vTaskDelay(1);
    }
    return 1;
}
int gpu_queue_peer_wait(uint32_t ticks)
{
    TickType_t began=xTaskGetTickCount();
    while (!atomic_load_explicit(&completed,memory_order_acquire)) {
        if (ss_live()<0 || xTaskGetTickCount()-began>=ticks) return 0;
        ss_progress();vTaskDelay(1);
    }
    return 1;
}
void gpu_queue_peer_main(const struct ss_grant *grant)
{
    if (grant->devices!=SS_DEVICE_GPU || grant->region_count!=2 ||
        grant->regions[0].bytes<PEER_OFFSET+8u*PEER_ELEMENTS ||
        grant->regions[1].bytes<PEER_OFFSET+4u*PEER_ELEMENTS) ss_fail(0x760);
    while (!atomic_load_explicit(&released,memory_order_acquire)) {
        if (ss_live()<0) ss_fail(0x761);
        ss_progress();vTaskDelay(1);
    }
    struct gpu_queue_context c;
    struct gpu_queue_lease_info leases[GPU_QUEUE_LEASES];
    if (gpu_queue_context_join(&c,leases)!=GPU_QUEUE_OK) ss_fail(0x762);
    uint32_t *input=(void *)((unsigned char *)grant->regions[0].base+PEER_OFFSET);
    const volatile uint32_t *output=(void *)((unsigned char *)grant->regions[1].base+PEER_OFFSET);
    if (leases[0].address!=(uintptr_t)grant->regions[1].base ||
        leases[1].address!=(uintptr_t)grant->regions[0].base) ss_fail(0x763);
    for (unsigned i=0;i<PEER_ELEMENTS;++i) {input[i]=11u*i;input[PEER_ELEMENTS+i]=5u*i+3u;}
    struct gpu_queue_job job={.sequence=1,.bindings=2,.argument_bytes=4,.groups={3,1,1},
        .binding={{.lease=0,.address=PEER_OFFSET,.sync_offset=PEER_OFFSET,.sync_bytes=4u*PEER_ELEMENTS},
                  {.lease=1,.address=PEER_OFFSET,.sync_offset=PEER_OFFSET,.sync_bytes=8u*PEER_ELEMENTS}}};
    job.arguments[0]=PEER_ELEMENTS;
    TickType_t began=xTaskGetTickCount();
    int submitted;
    while ((submitted=gpu_queue_submit(c,&job))==GPU_QUEUE_PARKED) {
        if (ss_live()<0 || xTaskGetTickCount()-began>=PEER_ACCEPT_TICKS) ss_fail(0x764);
        ss_progress();vTaskDelay(1);
    }
    if (submitted!=GPU_QUEUE_OK) ss_fail(0x765);
    atomic_store_explicit(&accepted,1,memory_order_release);
    struct gpu_queue_record record;
    if (gpu_queue_wait(c,&record,PEER_WAIT_TICKS)!=GPU_QUEUE_OK ||
        record.sequence!=1 || record.result!=GPU_QUEUE_OK) ss_fail(0x766);
    for (unsigned i=0;i<PEER_ELEMENTS;++i) if (output[i]!=16u*i+3u) ss_fail(0x767);
    atomic_store_explicit(&completed,1,memory_order_release);
}
