/* Typed wire session decoded into the device services' existing records. */
#include "os.h"
#include "application.h"
#include "../board/session.h"
#include "a733_storage_digest.h"
#include <string.h>
const void *os_session_extent(const void *raw,uint32_t type,size_t *bytes)
{
    if(!raw)return NULL;
    const uint8_t *p=raw;
    const struct ss_session_header *h=(const void *)(p+SS_SESSION_PREFIX_BYTES);
    if(h->magic!=SS_SESSION_MAGIC||h->version!=SS_SESSION_VERSION||
       h->bytes<SS_SESSION_PREFIX_BYTES+sizeof(*h)||h->bytes>SS_SESSION_MAX_BYTES)return NULL;
    size_t off=SS_SESSION_PREFIX_BYTES+sizeof(*h);
    const void *found=NULL;size_t found_bytes=0;
    uint32_t seen=0;
    if(h->extents>(h->bytes-off)/sizeof(struct ss_extent))return NULL;
    for(unsigned i=0;i<h->extents;++i){
        if(h->bytes-off<sizeof(struct ss_extent))return NULL;
        const struct ss_extent *e=(const void *)(p+off);off+=sizeof(*e);
        if(e->type>=32||(seen&(1u<<e->type))||e->bytes>h->bytes-off)return NULL;
        seen|=1u<<e->type;
        if(e->type==type){found=p+off;found_bytes=e->bytes;}
        size_t padded=((size_t)e->bytes+7)&~(size_t)7;
        if(padded>h->bytes-off)return NULL;
        for(size_t j=e->bytes;j<padded;++j)if(p[off+j])return NULL;
        off+=padded;
    }
    if(off!=h->bytes)return NULL;
    if(bytes)*bytes=found_bytes;return found;
}
static int extent_copy(const void *raw,uint32_t type,void *target,size_t expected)
{size_t bytes=0;const void *p=os_session_extent(raw,type,&bytes);if(!p||bytes!=expected)return 0;memcpy(target,p,bytes);return 1;}
int os_session_decode(const void *raw,size_t bytes,struct os_session *out)
{
    if(!raw||!out||bytes<SS_SESSION_PREFIX_BYTES+sizeof(struct ss_session_header)||bytes>SS_SESSION_MAX_BYTES)return 0;
    const uint8_t *p=raw;const struct ss_session_header *h=(const void *)(p+SS_SESSION_PREFIX_BYTES);
    if(h->magic!=SS_SESSION_MAGIC||h->version!=SS_SESSION_VERSION||h->bytes!=bytes||
       h->flags&~SS_SESSION_BIND_SID||h->devices&~SS_DEVICE_ALL)return 0;
    const struct ss_manifest empty={0};
    const struct ss_manifest *m=ss_application.manifest?ss_application.manifest:&empty;
    extern const uint8_t __start_sunstone_manifest[],__stop_sunstone_manifest[];
    size_t manifest_bytes=(uintptr_t)__stop_sunstone_manifest-(uintptr_t)__start_sunstone_manifest;
    if(ss_application.manifest){
        if(manifest_bytes!=sizeof(*m)||m!=(const void *)__start_sunstone_manifest||m->magic!=SS_MANIFEST_MAGIC||
           m->version!=SS_MANIFEST_VERSION||m->bytes!=sizeof(*m)||m->region_count>SS_REGIONS)return 0;
    }else if(manifest_bytes)return 0;
    uint8_t hash[32];storage_digest_bytes(__start_sunstone_manifest,manifest_bytes,hash);
    if(memcmp(hash,h->manifests_sha256,32)||(h->devices&m->devices_required)!=m->devices_required||
       h->devices&~(m->devices_required|m->devices_optional))return 0;
    uint32_t capture=h->devices&(SS_DEVICE_AUDIO_I2S|SS_DEVICE_AUDIO_HEADSET);
    if(capture&&(capture&(capture-1)))return 0;
    if((h->devices&SS_DEVICE_USB_KEYBOARD)&&!(h->devices&SS_DEVICE_USB_STORAGE))return 0;
    if(!(h->devices&SS_DEVICE_USB_STORAGE)){
        if(capture||!(h->flags&SS_SESSION_BIND_SID))return 0;
#ifdef SUNSTONE_A733_OS_RADIO
        return 0;
#endif
    }
#ifndef SUNSTONE_A733_OS_AUDIO
    if(h->devices&SS_DEVICE_AUDIO_I2S)return 0;
#endif
#ifndef SUNSTONE_A733_OS_HEADSET
    if(h->devices&SS_DEVICE_AUDIO_HEADSET)return 0;
#endif
#ifndef SUNSTONE_A733_OS_GPU_LINEAR
    if(h->devices&SS_DEVICE_GPU)return 0;
#endif
    memset(out,0,sizeof(*out));memcpy(&out->board,raw,sizeof(out->board));
    /* The prefix retains the board startup's existing task-selection value.
     * It must agree with the device grant until that board hook is replaced. */
    uint32_t workload=(h->devices&SS_DEVICE_USB_STORAGE)?((h->devices&SS_DEVICE_USB_KEYBOARD)?4:0):2;
#ifdef SUNSTONE_A733_OS_RADIO
    workload=1;
#endif
    if(out->board.workload!=workload)return 0;
    if(!extent_copy(raw,SS_EXTENT_THERMAL,&out->magic,A733_OS_SESSION_BYTES-SS_SESSION_PREFIX_BYTES))return 0;
    /* On the wire this service record's bytes still names its original
     * complete record, so the existing service validator sees its layout. */
    if(out->bytes!=sizeof(*out))return 0;
#ifdef SUNSTONE_A733_OS_AUDIO
    if(!extent_copy(raw,SS_EXTENT_AUDIO,&out->audio,sizeof(out->audio)))return 0;
    if(capture&&(out->audio.capture_samples<m->audio.min_frames||out->audio.capture_samples>m->audio.max_frames))return 0;
#endif
#ifdef SUNSTONE_A733_OS_NPU
    if(!extent_copy(raw,SS_EXTENT_NPU,&out->npu,sizeof(out->npu)))return 0;
#endif
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    if(!extent_copy(raw,SS_EXTENT_CPU_GRANT,&out->cpu_clock,sizeof(out->cpu_clock)))return 0;
    if(out->cpu_clock.clusters)out->cpu_clock.epoch=out->board.epoch;
#endif
    /* Scan INPUTS even for an application without inputs, validating the
     * complete extent framing and duplicate-type rule. */
    size_t input_bytes=0;const struct ss_input_fact *inputs=os_session_extent(raw,SS_EXTENT_INPUTS,&input_bytes);
    unsigned n=0;for(unsigned i=0;i<m->region_count;++i)if(m->regions[i].kind==SS_REGION_INPUT)++n;
    if(input_bytes!=n*sizeof(*inputs)||(n&&!inputs))return 0;
    n=0;for(unsigned i=0;i<m->region_count;++i)if(m->regions[i].kind==SS_REGION_INPUT){
        const struct ss_region *r=&m->regions[i];const struct ss_input_fact *f=&inputs[n++];
        uint8_t pinned=0;for(unsigned j=0;j<32;++j)pinned|=r->sha256[j];
        if(!f->bytes||f->bytes>r->bytes||(pinned&&(f->bytes!=r->bytes||memcmp(f->sha256,r->sha256,32))))return 0;
    }
#ifndef SUNSTONE_BOARD_RESIDENT
    uint64_t start=out->board.epoch/24;
    if(out->board.deadline_us<=start||m->return_reserve_us>=out->board.deadline_us-start||
       m->run_deadline_us>out->board.deadline_us-start-m->return_reserve_us)return 0;
#endif
    return os_session_valid(out);
}
const struct os_session *os_session(void)
{
    static struct os_session decoded;static int ready;
    if(!ready){const void *raw=board_session_sealed();if(!raw)return NULL;
        const struct ss_session_header *h=(const void *)((const uint8_t *)raw+SS_SESSION_PREFIX_BYTES);
        if(!os_session_decode(raw,h->bytes,&decoded))return NULL;ready=1;}
    return &decoded;
}
uint32_t os_session_devices(void)
{const uint8_t *raw=board_session_sealed();return raw?((const struct ss_session_header *)(raw+SS_SESSION_PREFIX_BYTES))->devices:0;}
const struct ss_input_fact *os_session_inputs(unsigned *count)
{size_t bytes=0;const void *p=os_session_extent(board_session_sealed(),SS_EXTENT_INPUTS,&bytes);if(count)*count=bytes/sizeof(struct ss_input_fact);return p;}
