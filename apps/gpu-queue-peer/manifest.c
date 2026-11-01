/* SPDX-License-Identifier: MIT */
#include "peer.h"
/* The primary manifest's placement is fixed by its two GPU regions. Admission
 * compares every byte of these region records with the owner before startup. */
__attribute__((used,retain,section("sunstone_gpu_peer_manifest"),aligned(8)))
const struct ss_manifest gpu_queue_peer_manifest = {
    .magic=SS_MANIFEST_MAGIC,.version=SS_MANIFEST_VERSION,.bytes=sizeof(struct ss_manifest),
    .name="gpu-queue-peer",.devices_required=SS_DEVICE_GPU,.cluster=SS_CLUSTER_A55,
    .priority=2,.stack_bytes=4096,.region_count=2,
    .regions={
        {.address=UINT64_C(0x44000000),.bytes=4096,.kind=SS_REGION_WORK,
         .access=SS_ACCESS_READ|SS_ACCESS_WRITE|SS_ACCESS_GPU_READ},
        {.address=UINT64_C(0x44200000),.bytes=4096,.kind=SS_REGION_WORK,
         .access=SS_ACCESS_READ|SS_ACCESS_GPU_WRITE}
    }
};
