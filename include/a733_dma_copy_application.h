#ifndef SUNSTONE_A733_DMA_COPY_APPLICATION_H
#define SUNSTONE_A733_DMA_COPY_APPLICATION_H
#include "a733_dma_pool_session.h"
/* Platform inputs to the dedicated copy application image. Supply a qualified
 * immutable handoff with a future absolute startup deadline. There is no default
 * authority record. This image owns its one common counter/registry/lock set;
 * a larger application instead borrows its existing set through the reusable
 * a733_dma_pool_session_init constructor. */
extern const struct a733_dma_handoff a733_dma_copy_handoff;
/* Existing cached platform guard: primary EL1, all cores at the pre-scheduler
 * barrier, caches adopted, deadline valid, scheduler release not published. */
int a733_smp_device_preparation_admitted(void);
/* Cached application entry selected instead of the scheduler diagnostic. */
void a733_smp_diagnostic_start(void);
#endif
