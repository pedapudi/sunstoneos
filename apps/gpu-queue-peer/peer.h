#ifndef SUNSTONE_GPU_QUEUE_PEER_H
#define SUNSTONE_GPU_QUEUE_PEER_H
#include "sunstone_app.h"
extern const struct ss_manifest gpu_queue_peer_manifest;
void gpu_queue_peer_main(const struct ss_grant *);
void gpu_queue_peer_release(void);
/* 1 after the peer verified and collected its job; 0 when the wait expires. */
int gpu_queue_peer_wait(uint32_t wait_ticks);
#define GPU_QUEUE_PEER_COMPOSITION(owner_manifest, shared_program) \
    const struct ss_gpu_share ss_gpu_share = { \
        &(owner_manifest), &gpu_queue_peer_manifest, gpu_queue_peer_main, \
        &(shared_program), 3u, 0u }
#endif
