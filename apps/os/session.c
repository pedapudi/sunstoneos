/* Typed wire session decoded into the device services' existing records. */
#include "os.h"
#include "application.h"
#include "../board/session.h"
#include "a733_storage_digest.h"
#include "a733_admission_refusal.h"
#ifdef SUNSTONE_A733_OS_USB_CLASSES
#include "a733_usb_memory.h"
#endif
#include <string.h>
#ifdef SUNSTONE_A733_OS_USB_JPEG
#include "../jpeg/a733_jpeg_application.h"
#include "a733_video_platform.h"
#include "a733_rail_leases.h"
#endif
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
#ifdef SUNSTONE_A733_OS_USB_CLASSES
static int usb_memory_extent_valid(const void *raw)
{
    size_t bytes=0;const struct ss_usb_memory_extent *m=os_session_extent(raw,SS_EXTENT_USB_MEMORY,&bytes);
    /* Optional geometry describes the immutable linked allocation. Physical
     * ownership is established by the runner, not an evidence digest here. */
    return !m || (bytes==sizeof(*m)&&m->version==1&&m->base==A733_USB_MEMORY_BASE&&
        m->bytes==A733_USB_MEMORY_BYTES&&m->alias==A733_USB_MEMORY_ALIAS_BASE);
}
#endif
/* The named refusal: *why is the A733_DECODE_* sub-term of the rule that refused the session (include/a733_admission_refusal.h). */
#define REFUSE(sub) do{if(why)*why=(sub);return 0;}while(0)
int os_session_decode_why(const void *raw,size_t bytes,struct os_session *out,unsigned *why)
{
    if(why)*why=0;
    if(!raw||!out||bytes<SS_SESSION_PREFIX_BYTES+sizeof(struct ss_session_header)||bytes>SS_SESSION_MAX_BYTES)REFUSE(A733_DECODE_FRAMING);
    const uint8_t *p=raw;const struct ss_session_header *h=(const void *)(p+SS_SESSION_PREFIX_BYTES);
    if(h->magic!=SS_SESSION_MAGIC||h->version!=SS_SESSION_VERSION||h->bytes!=bytes||
       h->flags&~SS_SESSION_BIND_SID||h->devices&~SS_DEVICE_ALL)REFUSE(A733_DECODE_FRAMING);
    const struct ss_manifest empty={0};
    const struct ss_manifest *m=ss_application.manifest?ss_application.manifest:&empty;
    extern const uint8_t __start_sunstone_manifest[],__stop_sunstone_manifest[];
    size_t manifest_bytes=(uintptr_t)__stop_sunstone_manifest-(uintptr_t)__start_sunstone_manifest;
    if(ss_application.manifest){
        if(manifest_bytes!=sizeof(*m)||m!=(const void *)__start_sunstone_manifest||m->magic!=SS_MANIFEST_MAGIC||
           m->version!=SS_MANIFEST_VERSION||m->bytes!=sizeof(*m)||m->region_count>SS_REGIONS)REFUSE(A733_DECODE_MANIFEST);
    }else if(manifest_bytes)REFUSE(A733_DECODE_MANIFEST);
    uint8_t hash[32];storage_digest_bytes(__start_sunstone_manifest,manifest_bytes,hash);
    if(memcmp(hash,h->manifests_sha256,32)||(h->devices&m->devices_required)!=m->devices_required||
       h->devices&~(m->devices_required|m->devices_optional))REFUSE(A733_DECODE_MANIFEST);
#ifdef SUNSTONE_A733_OS_USB_CLASSES
    if(!usb_memory_extent_valid(raw))REFUSE(A733_DECODE_EXTENT);
#else
    if(os_session_extent(raw,SS_EXTENT_USB_MEMORY,NULL))REFUSE(A733_DECODE_EXTENT);
#endif
    uint32_t capture=h->devices&(SS_DEVICE_AUDIO_I2S|SS_DEVICE_AUDIO_HEADSET);
    if(capture&&(capture&(capture-1)))REFUSE(A733_DECODE_DEVICES);
#ifndef SUNSTONE_A733_OS_USB_CLASSES
    if(h->devices&(SS_DEVICE_USB_MOUSE|SS_DEVICE_USB_SERIAL|SS_DEVICE_USB_CAMERA))REFUSE(A733_DECODE_DEVICES);
    if((h->devices&SS_DEVICE_USB_KEYBOARD)&&!(h->devices&SS_DEVICE_USB_STORAGE))REFUSE(A733_DECODE_DEVICES);
#endif
    if(!(h->devices&SS_DEVICE_USB_STORAGE)){
        if(capture||!(h->flags&SS_SESSION_BIND_SID))REFUSE(A733_DECODE_DEVICES);
    }
#ifndef SUNSTONE_A733_OS_HEADSET
    if(h->devices&SS_DEVICE_AUDIO_HEADSET)REFUSE(A733_DECODE_DEVICES);
#endif
#ifndef SUNSTONE_A733_OS_GPU
    if(h->devices&SS_DEVICE_GPU)REFUSE(A733_DECODE_DEVICES);
#endif
    memset(out,0,sizeof(*out));memcpy(&out->board,raw,sizeof(out->board));
    /* The prefix retains the board startup's existing task-selection value.
     * It must agree with the device grant until that board hook is replaced. */
    uint32_t workload=(h->devices&SS_DEVICE_USB_STORAGE)?((h->devices&SS_DEVICE_USB_KEYBOARD)?4:0):2;
#ifdef SUNSTONE_A733_OS_USB_CLASSES
    if(h->devices&(SS_DEVICE_USB_KEYBOARD|SS_DEVICE_USB_MOUSE|SS_DEVICE_USB_SERIAL|SS_DEVICE_USB_CAMERA))workload=0;
#endif
    /* The radio station has no device bit yet: the board prefix's workload 1 grants it, and it needs storage. */
    const int radio=out->board.workload==1;
    if(radio){
        if(!OS_LINKED_RADIO||!(h->devices&SS_DEVICE_USB_STORAGE))REFUSE(A733_DECODE_DEVICES);
        workload=1;
    }
    if(out->board.workload!=workload)REFUSE(A733_DECODE_WORKLOAD);
    if(!extent_copy(raw,SS_EXTENT_THERMAL,&out->magic,A733_OS_SESSION_BYTES-SS_SESSION_PREFIX_BYTES))REFUSE(A733_DECODE_EXTENT);
    /* Each service extent that the session carries is copied and grants its service. A service is granted only when
     * the image has it, and the I2S receive record only with the I2S device. An absent record grants nothing. */
    if(radio)out->granted|=OS_GRANT_RADIO;
    {
        const struct {uint32_t type,grant;size_t bytes;void *target;int linked,required;} services[]={
            {SS_EXTENT_AUDIO,OS_GRANT_AUDIO,sizeof(out->audio),&out->audio,OS_LINKED_AUDIO,!!(h->devices&SS_DEVICE_AUDIO_I2S)},
            {SS_EXTENT_NPU,OS_GRANT_NPU,sizeof(out->npu),&out->npu,OS_LINKED_NPU,0},
            {SS_EXTENT_CPU_GRANT,OS_GRANT_CPU_CLOCK,sizeof(out->cpu_clock),&out->cpu_clock,OS_LINKED_CPU_CLOCK,0},
            {SS_EXTENT_DMA,OS_GRANT_DMA,sizeof(out->dma),&out->dma,OS_LINKED_DMA,OS_LINKED_DMA},
        };
        for(unsigned i=0;i<sizeof(services)/sizeof(services[0]);++i){
            const int present=os_session_extent(raw,services[i].type,NULL)!=NULL;
            if(present&&!services[i].linked)REFUSE(A733_DECODE_EXTENT);
            if(services[i].required&&!present)REFUSE(A733_DECODE_EXTENT);
            if(present){
                if(!extent_copy(raw,services[i].type,services[i].target,services[i].bytes))REFUSE(A733_DECODE_EXTENT);
                out->granted|=services[i].grant;
            }
        }
    }
    if(out->granted&OS_GRANT_AUDIO){
        if(!(h->devices&SS_DEVICE_AUDIO_I2S))REFUSE(A733_DECODE_DEVICES);
        if(out->audio.capture_samples<m->audio.min_frames||out->audio.capture_samples>m->audio.max_frames)REFUSE(A733_DECODE_AUDIO_FRAMES);
    }
    if(out->granted&OS_GRANT_CPU_CLOCK&&out->cpu_clock.clusters)out->cpu_clock.epoch=out->board.epoch;
    /* On the wire the thermal record's bytes names the board prefix and every service record the session carries, so
     * the existing service validator sees its layout. */
    if(out->bytes!=os_session_wire_bytes(out->granted))REFUSE(A733_DECODE_EXTENT);
#ifdef SUNSTONE_A733_OS_USB_JPEG
    size_t jpeg_bytes=0;const struct a733_jpeg_session *jpeg=os_session_extent(raw,SS_EXTENT_JPEG,&jpeg_bytes);
    if(!jpeg||jpeg_bytes!=sizeof(*jpeg)||memcmp(&jpeg->board,&out->board,sizeof(out->board))||
       jpeg->magic!=A733_JPEG_SESSION_MAGIC||jpeg->version!=4||jpeg->bytes!=sizeof(*jpeg)||
       !jpeg->owner||!jpeg->authority||jpeg->pck_client>=16||jpeg->pck_client==out->board.pck_client||
       jpeg->module!=A733_VIDEO_VENDOR_MODULE||jpeg->module_hz!=A733_VIDEO_VENDOR_HZ||
       jpeg->supply_uv!=800000||jpeg->supply_rail!=A733_RAIL_DCDC2||
       jpeg->iommu_gates[0]!=0x10007||jpeg->iommu_gates[1]!=0x10007)REFUSE(A733_DECODE_EXTENT);
    const uint64_t owners[]={out->board.pmic_owner,out->board.usb0_owner,out->board.typec_owner,
        out->board.usb2_owner,out->thermal_owner,out->board.latch_owner,out->board.latch_m_owner};
    for(unsigned i=0;i<sizeof(owners)/sizeof(owners[0]);++i)if(jpeg->owner==owners[i])REFUSE(A733_DECODE_EXTENT);
    const uint8_t *facts[]={jpeg->initial,jpeg->memory,jpeg->electrical,jpeg->supervision};
    for(unsigned i=0;i<4;++i){uint8_t present=0;for(unsigned j=0;j<32;++j)present|=facts[i][j];if(!present)REFUSE(A733_DECODE_EXTENT);}
    for(unsigned j=0;j<32;++j)if(jpeg->fixture[j]||jpeg->reference[j])REFUSE(A733_DECODE_EXTENT);
#else
    if(os_session_extent(raw,SS_EXTENT_JPEG,NULL))REFUSE(A733_DECODE_EXTENT);
#endif
    /* Scan INPUTS even for an application without inputs, validating the
     * complete extent framing and duplicate-type rule. */
    size_t input_bytes=0;const struct ss_input_fact *inputs=os_session_extent(raw,SS_EXTENT_INPUTS,&input_bytes);
    unsigned n=0;for(unsigned i=0;i<m->region_count;++i)if(m->regions[i].kind==SS_REGION_INPUT)++n;
    if(input_bytes!=n*sizeof(*inputs)||(n&&!inputs))REFUSE(A733_DECODE_INPUT);
    n=0;for(unsigned i=0;i<m->region_count;++i)if(m->regions[i].kind==SS_REGION_INPUT){
        const struct ss_region *r=&m->regions[i];const struct ss_input_fact *f=&inputs[n++];
        uint8_t pinned=0;for(unsigned j=0;j<32;++j)pinned|=r->sha256[j];
        if(f->bytes>r->bytes||(pinned&&(f->bytes!=r->bytes||memcmp(f->sha256,r->sha256,32))))REFUSE(A733_DECODE_INPUT);
    }
#ifndef SUNSTONE_BOARD_RESIDENT
    /* The immutable supervised board prefix has zero time fields. Contract
     * admission binds the run to armed_at plus this configured lifetime;
     * main.c derives the absolute runtime deadline only after admission. */
    const uint64_t lifetime_us=(uint64_t)A733_BOARD_SUPERVISED_SECONDS*1000000;
    if(m->return_reserve_us>=lifetime_us)REFUSE(A733_DECODE_RETURN_RESERVE);
    if(m->run_deadline_us>lifetime_us-m->return_reserve_us)REFUSE(A733_DECODE_RUN_DEADLINE);
#endif
    if(!os_session_valid(out))REFUSE(A733_DECODE_SERVICE);
    return 1;
}
int os_session_decode(const void *raw,size_t bytes,struct os_session *out)
{return os_session_decode_why(raw,bytes,out,NULL);}
const struct os_session *os_session(void)
{
    static struct os_session decoded;static int ready;
    if(!ready){const void *raw=board_session_sealed();if(!raw)return NULL;
        const struct ss_session_header *h=(const void *)((const uint8_t *)raw+SS_SESSION_PREFIX_BYTES);
        if(!os_session_decode(raw,h->bytes,&decoded))return NULL;ready=1;}
    return &decoded;
}
int os_granted(unsigned mask)
{const struct os_session *s=os_session_admitted();return s&&mask&&(s->granted&mask)==mask;}
uint32_t os_session_devices(void)
{const uint8_t *raw=board_session_sealed();return raw?((const struct ss_session_header *)(raw+SS_SESSION_PREFIX_BYTES))->devices:0;}
const struct ss_input_fact *os_session_inputs(unsigned *count)
{size_t bytes=0;const void *p=os_session_extent(board_session_sealed(),SS_EXTENT_INPUTS,&bytes);if(count)*count=bytes/sizeof(struct ss_input_fact);return p;}
