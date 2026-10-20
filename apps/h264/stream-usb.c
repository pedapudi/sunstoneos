#include "h264.h"
#include "tusb.h"
#include <string.h>
static uint8_t header[H264_STREAM_RESPONSE_BYTES];
static unsigned total,sent,transferred;
static struct h264_stream_output *sending;
static void put32(uint8_t *p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i));}
static int now(void *unused,uint64_t *out)
{(void)unused;return board_live(NULL)&&a733_counter_clock_now(&providers.shared.counter,out)==1&&board_live(NULL)?1:-1;}
void h264_stream_tx_complete(unsigned bytes)
{
    if(bytes>sent-transferred){h264_stream_fail(&h264_stream);return;}
    transferred+=bytes;
    if(total&&sent==total&&transferred==total){
        if(sending)atomic_store_explicit(&sending->delivered,1,memory_order_release);
        total=sent=transferred=0;sending=NULL;
    }
}
static void response(void)
{
    struct h264_stream *s=&h264_stream;unsigned ordinal=s->query_ordinal;
    unsigned next=atomic_load_explicit(&s->next,memory_order_acquire);
    unsigned terminal=atomic_load_explicit(&s->failed,memory_order_acquire)?2:atomic_load_explicit(&h264.result,memory_order_acquire);
#ifdef SUNSTONE_H264_HIGH
    unsigned present_next=atomic_load_explicit(&s->present_next,memory_order_acquire);
#endif
    sending=NULL;
    for(unsigned i=0;i<H264_STREAM_COPIES;++i)if((atomic_load_explicit(&s->output[i].state,memory_order_acquire)==2
#ifdef SUNSTONE_H264_HIGH
        ||(terminal==2&&atomic_load_explicit(&s->output[i].state,memory_order_acquire)==3)
#endif
        )&&s->output[i].ordinal==ordinal)sending=&s->output[i];
#ifdef SUNSTONE_H264_HIGH
    unsigned status=sending?1:terminal==2?2:ordinal<present_next?3:0;
#else
    unsigned status=sending?1:terminal==2?2:ordinal<next?3:0;
#endif
    const uint8_t *d=h264_stream_presentation_descriptor(s,ordinal);
#ifdef SUNSTONE_H264_HIGH
    unsigned output_bytes=h264_stream_u32(s->manifest+32)*h264_stream_u32(s->manifest+36)*3/2;
#else
    unsigned output_bytes=H264_STREAM_OUTPUT_BYTES;
#endif
    memset(header,0,sizeof(header));put32(header,0x34363248);put32(header+4,H264_STREAM_RESPONSE_VERSION);put32(header+8,status);
    put32(header+12,sending?output_bytes:0);
    put32(header+16,(uint32_t)s->epoch);put32(header+20,(uint32_t)(s->epoch>>32));
    put32(header+24,(uint32_t)s->generation);put32(header+28,(uint32_t)(s->generation>>32));
    memcpy(header+32,s->manifest_hash,32);memcpy(header+64,d+16,32);
    if(sending)memcpy(header+96,sending->digest,32);
    put32(header+128,ordinal);put32(header+132,s->count);
    put32(header+136,h264_stream_u32(d+8));
#ifdef SUNSTONE_H264_HIGH
    put32(header+140,h264_stream_u32(d+12));
    put32(header+144,h264_stream_u32(s->manifest+32));put32(header+148,h264_stream_u32(s->manifest+36));put32(header+152,h264_stream_u32(d+92));put32(header+156,terminal);
    uint32_t geometry[]={h264_stream_u32(s->manifest+24),h264_stream_u32(s->manifest+28),h264_stream_u32(s->manifest+24),h264_stream_u32(s->manifest+24)/2,h264_stream_u32(s->manifest+40),h264_stream_u32(s->manifest+44),h264_stream_u32(s->manifest+48),h264_stream_u32(s->manifest+52)};
    for(unsigned i=0;i<8;++i)put32(header+160+4*i,geometry[i]);
    put32(header+192,next);put32(header+196,h264_stream_u32(d+88));put32(header+200,h264_stream_u32(d+80));put32(header+204,h264_stream_u32(d+96));put32(header+208,h264_stream_u32(d+100));put32(header+212,sending&&h264_stream_u32(d+4)?1:0);
    put32(header+216,sending&&atomic_load_explicit(&sending->state,memory_order_acquire)==3?1:0);
#else
    put32(header+140,2*h264_stream_u32(d+8));
    put32(header+144,H264_STREAM_WIDTH);put32(header+148,H264_STREAM_HEIGHT);put32(header+152,h264_stream_u32(d+8)?0:2);put32(header+156,terminal);
#ifdef SUNSTONE_H264_FULLHD
    /* Authenticated fixed profile, including pending/failed metadata. */
    const uint32_t geometry[]={1920,1088,1920,960,0,0,0,8};
    for(unsigned i=0;i<8;++i)put32(header+160+4*i,geometry[i]);
#endif
#endif
    s->query_pending=0;sent=transferred=0;total=H264_STREAM_RESPONSE_BYTES+(sending?output_bytes:0);
}
void h264_stream_usb(void)
{
    if(total){
        const uint8_t *p;unsigned available;
        if(sent<H264_STREAM_RESPONSE_BYTES){p=header+sent;available=H264_STREAM_RESPONSE_BYTES-sent;}
        else{p=sending?sending->payload+(sent-H264_STREAM_RESPONSE_BYTES):NULL;available=total-sent;}
        if(available){unsigned n=tud_vendor_write(p,available);if(n>available){h264_stream_fail(&h264_stream);return;}sent+=n;tud_vendor_flush();}
        return;
    }
    if(h264_stream.query_pending){response();return;}
    unsigned available=tud_vendor_available();if(!available)return;
    size_t n=h264_stream_remaining(&h264_stream);if(n>512)n=512;if(n>available)n=available;
    uint8_t incoming[512];
    if(!n||tud_vendor_read(incoming,n)!=n){h264_stream_fail(&h264_stream);return;}
    if(h264_stream_feed(&h264_stream,&h264.session,now,NULL,board.session.deadline_us-10000,incoming,n)<0){
        /* Keep authenticated copied outputs queryable after a protocol failure.
         * Failed state prevents subsequent manifest/frame publication. */
        h264_stream.header_used=h264_stream.received=h264_stream.body_bytes=0;
        h264_stream.command=h264_stream.argument=0;
    }
}
