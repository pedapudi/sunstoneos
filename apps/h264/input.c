#include "a733_h264_input.h"
#include "a733_storage_digest.h"
#include <string.h>
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static int present(const uint8_t *p){uint8_t v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
int a733_h264_input_ready(const struct a733_h264_input *p)
{return p&&atomic_load_explicit(&p->committed,memory_order_acquire)==1;}
size_t a733_h264_input_remaining(const struct a733_h264_input *p)
{
    if(!p||p->failed||a733_h264_input_ready(p))return 0;
    if(p->header_used<32)return 32-p->header_used;
    return p->received<p->stream_bytes?p->stream_bytes-p->received:
        p->stream_bytes+p->oracle_bytes-p->received;
}
static int reject(struct a733_h264_input *p){if(p)p->failed=1;return -1;}
int a733_h264_input_feed(struct a733_h264_input *p,const struct a733_h264_session *s,
                         int (*clock)(void *,uint64_t *),void *context,
                         uint64_t deadline,const uint8_t *bytes,size_t n)
{
    uint64_t now;
    if(!p||!s||!clock||!bytes||!n||n>A733_H264_INPUT_BYTES||p->failed||
       a733_h264_input_ready(p)||!deadline||clock(context,&now)!=1||now>=deadline||!s->board.epoch||
       !s->board.generation||!s->authority||!present(s->fixture))return reject(p);
    if(!p->header_used){
        p->epoch=s->board.epoch;p->generation=s->board.generation;
        p->authority=s->authority;p->deadline=deadline;
        memcpy(p->stream_hash,s->fixture,32);memcpy(p->oracle_hash,s->reference,32);
    }
    if(p->epoch!=s->board.epoch||p->generation!=s->board.generation||
       p->authority!=s->authority||p->deadline!=deadline||now<p->last_now||
       memcmp(p->stream_hash,s->fixture,32)||memcmp(p->oracle_hash,s->reference,32)||
       n>a733_h264_input_remaining(p))return reject(p);
    p->last_now=now;
    if(p->header_used<32){
        memcpy(p->header+p->header_used,bytes,n);p->header_used+=n;
        if(p->header_used!=32)return 0;
        p->stream_bytes=le32(p->header+8);p->oracle_bytes=le32(p->header+12);
        if(le32(p->header)!=A733_H264_UPLOAD_MAGIC||le32(p->header+4)!=1||
           !p->stream_bytes||p->stream_bytes>A733_H264_INPUT_BYTES||
           (p->oracle_bytes!=0&&p->oracle_bytes!=A733_H264_OUTPUT_BYTES)||
           (present(p->oracle_hash)!=(p->oracle_bytes!=0))||
           le64(p->header+16)!=p->epoch||le64(p->header+24)!=p->generation)return reject(p);
        return 0;
    }
    if(p->received<p->stream_bytes)memcpy(p->stream+p->received,bytes,n);
    else memcpy(p->oracle+p->received-p->stream_bytes,bytes,n);
    p->received+=n;
    if(p->received!=p->stream_bytes+p->oracle_bytes)return 0;
    uint8_t digest[32];struct a733_h264_picture picture;
    storage_digest_bytes(p->stream,p->stream_bytes,digest);
    if(memcmp(digest,p->stream_hash,32)||!a733_h264_parse(p->stream,p->stream_bytes,&picture)||
       picture.width!=48||picture.height!=32)return reject(p);
    if(p->oracle_bytes){storage_digest_bytes(p->oracle,p->oracle_bytes,digest);
        if(memcmp(digest,p->oracle_hash,32))return reject(p);}
    /* Hashing/parsing can consume the remaining window. Check fresh time
     * before publication, while no decoder consumer can observe the bytes. */
    if(clock(context,&now)!=1||now<p->last_now||now>=deadline)return reject(p);
    p->last_now=now;
    atomic_store_explicit(&p->committed,1,memory_order_release);return 1;
}
