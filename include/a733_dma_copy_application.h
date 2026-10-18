#ifndef SUNSTONE_A733_DMA_COPY_APPLICATION_H
#define SUNSTONE_A733_DMA_COPY_APPLICATION_H
#include "a733_dma_pool_session.h"
/* Platform inputs to the dedicated copy application image. Supply a qualified
 * immutable handoff with a future absolute startup deadline. There is no default
 * authority record. This image owns its one common counter/registry/lock set;
 * a larger application instead borrows its existing set through the reusable
 * a733_dma_pool_session_init constructor. */
extern const struct a733_dma_handoff a733_dma_copy_handoff;
/* Application result in cached RAM. A native observer may acquire-load complete
 * before reading completed_channels. This is not a durable host result across
 * reset; the image has no console or USB observer of its own yet. */
struct a733_dma_copy_result {
    atomic_uint completed_channels;
    atomic_uint complete;
};
extern struct a733_dma_copy_result a733_dma_copy_result;
/* Existing cached platform guard: primary EL1, all cores at the pre-scheduler
 * barrier, caches adopted, deadline valid, scheduler release not published. */
int a733_smp_device_preparation_admitted(void);
/* Cached application entry selected instead of the scheduler diagnostic. */
void a733_smp_diagnostic_start(void);
#endif
