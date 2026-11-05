/* Fixed composition of two trusted GPU applications. */
#include "gpu-share.h"
#include <string.h>
int os_gpu_share_admit(const struct ss_gpu_share *share,const struct ss_manifest *owner,
    const struct ss_grant *grant,struct ss_grant *peer)
{
    if (!share || !owner || !grant || !peer || share->owner!=owner || !share->manifest ||
        !share->main || !share->program || share->reserved || !grant->worker_cores ||
        (grant->worker_cores&~255u) || grant->main_core>=8 ||
        (grant->worker_cores&(1u<<grant->main_core)) ||
        !(grant->devices&SS_DEVICE_GPU)) return 0;
    const struct ss_manifest *m=share->manifest;
    if (m==owner || m->magic!=SS_MANIFEST_MAGIC || m->version!=SS_MANIFEST_VERSION ||
        m->bytes!=sizeof(*m) || m->devices_required!=SS_DEVICE_GPU || m->devices_optional ||
        m->workers || m->heap_bytes || m->reserved0 || m->clock_use ||
        m->stack_bytes<1024u || m->stack_bytes>OS_GPU_PEER_STACK_BYTES ||
        m->stack_bytes%16u || !m->priority || m->priority>owner->priority ||
        m->cluster!=owner->cluster || m->host_op_first || m->host_op_count ||
        m->output_limit || m->audio.rate_hz || m->audio.channels || m->audio.bits ||
        m->audio.min_frames || m->audio.max_frames || m->run_deadline_us ||
        m->return_reserve_us || m->progress_window_us ||
        !m->region_count || m->region_count!=owner->region_count || m->region_count>SS_REGIONS ||
        grant->region_count!=owner->region_count || !m->name[0] ||
        !memchr(m->name,0,sizeof(m->name)) || !memcmp(m->name,owner->name,sizeof(m->name))) return 0;
    unsigned ended=0;
    for (unsigned i=0;i<SS_NAME_BYTES;++i) {
        unsigned c=(unsigned char)m->name[i];
        if (!c) ended=1;
        else if (ended || !((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-')) return 0;
    }
    uint32_t mapped=0;
    for (unsigned i=0;i<m->region_count;++i) {
        if (memcmp(&m->regions[i],&owner->regions[i],sizeof(m->regions[i]))) return 0;
        if (m->regions[i].access&(SS_ACCESS_GPU_READ|SS_ACCESS_GPU_WRITE)) mapped|=1u<<i;
    }
    if (!mapped || share->shared_regions!=mapped) return 0;
    *peer=*grant; peer->devices=SS_DEVICE_GPU; peer->worker_cores=0;
    peer->capture_frames=0;
    unsigned core=0;
    while (!(grant->worker_cores&(1u<<core))) ++core;
    peer->main_core=core; return 1;
}
