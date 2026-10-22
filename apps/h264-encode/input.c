#include "input.h"
#include "a733_storage_digest.h"
#include <string.h>
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static int present(const uint8_t *p){uint8_t v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
static int reject(struct h264_encode_input *p){if(p)p->failed=1;return -1;}
int h264_encode_input_bind(struct h264_encode_input *p,uint8_t *bytes,size_t capacity)
{
    if(!p||p->bytes||p->failed||p->header_used||!bytes||capacity!=H264_ENCODE_INPUT_PADDED||
       (uintptr_t)bytes%4096)return 0;
    p->bytes=bytes;p->capacity=capacity;return 1;
}
int h264_encode_input_ready(const struct h264_encode_input *p)
{return p&&atomic_load_explicit(&p->committed,memory_order_acquire)==1;}
size_t h264_encode_input_remaining(const struct h264_encode_input *p)
{
    if(!p||!p->bytes||p->failed||h264_encode_input_ready(p))return 0;
    return p->header_used<32?32-p->header_used:A733_H264_ENCODE_INPUT_BYTES-p->received;
}
int h264_encode_input_feed(struct h264_encode_input *p,const struct a733_h264_encode_session *s,
    int (*clock)(void *,uint64_t *),void *context,uint64_t deadline,const uint8_t *bytes,size_t n)
{
    uint64_t now;
    if(!p||!s||!clock||!bytes||!n||n>4096||p->failed||h264_encode_input_ready(p)||
       !p->bytes||p->capacity!=H264_ENCODE_INPUT_PADDED||!deadline||
       clock(context,&now)!=1||now>=deadline||!s->board.epoch||!s->board.generation||
       !s->authority||!present(s->input_digest)||present(s->reserved_reference))return reject(p);
    if(!p->header_used){p->epoch=s->board.epoch;p->generation=s->board.generation;
        p->authority=s->authority;p->deadline=deadline;memcpy(p->hash,s->input_digest,32);}
    if(p->epoch!=s->board.epoch||p->generation!=s->board.generation||p->authority!=s->authority||
       p->deadline!=deadline||now<p->last_now||memcmp(p->hash,s->input_digest,32)||
       n>h264_encode_input_remaining(p))return reject(p);
    p->last_now=now;
    if(p->header_used<32){
        memcpy(p->header+p->header_used,bytes,n);p->header_used+=n;
        if(p->header_used!=32)return 0;
        if(le32(p->header)!=H264_ENCODE_UPLOAD_MAGIC||le32(p->header+4)!=1||
           le32(p->header+8)!=A733_H264_ENCODE_INPUT_BYTES||!le32(p->header+12)||
           le32(p->header+12)>A733_H264_ENCODE_GOP_FRAMES||
           le64(p->header+16)!=p->epoch||le64(p->header+24)!=p->generation)return reject(p);
        p->frames=le32(p->header+12);return 0;
    }
    memcpy(p->bytes+p->received,bytes,n);p->received+=n;
    if(p->received!=A733_H264_ENCODE_INPUT_BYTES)return 0;
    uint8_t digest[32];storage_digest_bytes(p->bytes,p->received,digest);
    if(memcmp(digest,p->hash,32)||clock(context,&now)!=1||now<p->last_now||now>=deadline)return reject(p);
    p->last_now=now;atomic_store_explicit(&p->committed,1,memory_order_release);return 1;
}
