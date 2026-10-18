#include "a733_dma_copy_application.h"
#include "boot.h"
#include "result.h"
#include <stddef.h>
_Static_assert(A733_SMP_CONTRACT_MAGIC==UINT64_C(0x415337334f424331),"preserved launcher contract magic");
_Static_assert(sizeof(struct a733_smp_contract)==224,"preserved launcher contract extent");
/* The host extracts these immutable records from the reviewed native ELF. */
_Static_assert(sizeof(struct a733_dma_copy_usb_handoff)==56,"host USB record ABI");
_Static_assert(offsetof(struct a733_dma_handoff,controller)==80,"host DMA controller ABI");
_Static_assert(offsetof(struct a733_dma_handoff,evidence)==88,"host DMA evidence ABI");
_Static_assert(offsetof(struct a733_dma_handoff,service_channels)==102,"host DMA channel ABI");
_Static_assert(offsetof(struct a733_dma_handoff,allowed.handoff)==56,"host DMA identity ABI");

int a733_dma_copy_observer_admit(void)
{
    const struct a733_dma_copy_usb_handoff *u=&a733_dma_copy_usb_handoff;
    const struct a733_observe_identity *id=&a733_observe_identity;
    uint64_t now=a733_smp_counter();
    return id->epoch && id->generation && id->lease && id->armed_at<id->deadline &&
        now>=id->armed_at && now<id->deadline && u->epoch==id->epoch &&
        u->generation==id->generation && u->lease==id->lease && u->evidence &&
        u->dma_evidence && u->dma_evidence==a733_dma_copy_handoff.evidence &&
        u->dma_handoff && u->dma_handoff==a733_dma_copy_handoff.allowed.handoff &&
        u->established==A733_DMA_COPY_USB_REQUIRED;
}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p)
{uint64_t v=0;for(unsigned i=0;i<8;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
static void put64(uint8_t *p,uint64_t value)
{for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(value>>(8*i));}
int a733_dma_copy_request(const uint8_t p[64],const struct a733_observe_identity *id,
    uint32_t *sequence,uint8_t challenge[32])
{
    if(le32(p)!=A733_OBSERVE_MESSAGE_MAGIC || !le32(p+4) ||
       le32(p+8)!=A733_DMA_COPY_COMMAND || le32(p+12)!=48 ||
       le64(p+16)!=id->epoch || le64(p+24)!=id->generation)return 0;
    unsigned nonzero=0;for(unsigned i=0;i<32;++i)nonzero|=p[32+i];if(!nonzero)return 0;
    *sequence=le32(p+4);for(unsigned i=0;i<32;++i)challenge[i]=p[32+i];return 1;
}
int a733_dma_copy_frame(uint8_t out[512],const struct a733_observe_identity *id,
    const uint8_t challenge[32],const uint8_t build[32])
{
    if(id!=&a733_observe_identity || !a733_dma_copy_observer_admit())return 0;
    const struct a733_dma_handoff *d=&a733_dma_copy_handoff;
    unsigned done=atomic_load_explicit(&a733_dma_copy_result.complete,memory_order_acquire);
    unsigned mask=0;
    uint64_t start=0,end=0;
    if(!d->service_channels || d->controller!=UINT32_C(0x04601000))return 0;
    if(done) {
        mask=atomic_load_explicit(&a733_dma_copy_result.completed_channels,memory_order_relaxed);
        start=a733_dma_copy_result.start;end=a733_dma_copy_result.end;
        if(done!=1 || mask!=d->service_channels || start<id->armed_at || end<start ||
           end>=id->deadline)return 0;
    }
    for(unsigned i=0;i<512;++i)out[i]=0;
    put64(out,A733_DMA_COPY_FRAME_MAGIC);put64(out+8,UINT64_C(512)<<32|1);
    put64(out+16,done?0:1);put64(out+24,id->epoch);put64(out+32,id->generation);
    put64(out+40,id->lease);put64(out+48,start);put64(out+56,end);
    put64(out+64,id->deadline);put64(out+72,mask);
    for(unsigned i=0;i<32;++i){out[80+i]=challenge[i];out[112+i]=build[i];}
    put64(out+144,d->service_channels);put64(out+152,A733_DMA_COPY_BYTES);
    put64(out+160,d->controller);put64(out+168,d->evidence);
    put64(out+176,d->allowed.handoff);put64(out+184,a733_dma_copy_usb_handoff.evidence);
    put64(out+192,id->armed_at);
    a733_observe_digest(out,480,out+480);return 1;
}
