#include "input.h"
#include "a733_storage_digest.h"
#include "a733_g2d.h"
#include <string.h>
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static int present(const uint8_t *p){uint8_t v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
static int reject(struct g2d_input *p){if(p)p->failed=1;return -1;}
int g2d_input_bind(struct g2d_input *p,uint8_t *bytes,size_t capacity)
{
    if(!p||p->bytes||p->failed||p->header_used||!bytes||!capacity||capacity>G2D_INPUT_PADDED||capacity%4096||
       (uintptr_t)bytes%4096)return 0;
    p->bytes=bytes;p->capacity=capacity;return 1;
}
int g2d_input_ready(const struct g2d_input *p)
{return p&&atomic_load_explicit(&p->committed,memory_order_acquire)==1;}
size_t g2d_input_remaining(const struct g2d_input *p)
{
    if(!p||!p->bytes||p->failed||g2d_input_ready(p))return 0;
    return p->header_used<32?32-p->header_used:p->expected-p->received;
}
int g2d_input_feed(struct g2d_input *p,const struct a733_g2d_session *s,
    int (*clock)(void *,uint64_t *),void *context,uint64_t deadline,const uint8_t *bytes,size_t n)
{
    uint64_t now;
    if(!p||!s||!clock||!bytes||!n||n>4096||p->failed||g2d_input_ready(p)||
       !p->bytes||!p->capacity||p->capacity>G2D_INPUT_PADDED||p->capacity%4096||!deadline||
       clock(context,&now)!=1||now>=deadline||!s->board.epoch||!s->board.generation||
       !a733_g2d_geometry_valid(s)||!s->authority||!present(s->fixture)||present(s->reference))return reject(p);
    if(!p->header_used){p->expected=(size_t)s->stride*s->height;if(!p->expected||p->expected>p->capacity)return reject(p);p->epoch=s->board.epoch;p->generation=s->board.generation;
        p->authority=s->authority;p->deadline=deadline;memcpy(p->hash,s->fixture,32);}
    if(p->epoch!=s->board.epoch||p->generation!=s->board.generation||p->authority!=s->authority||
       p->deadline!=deadline||now<p->last_now||memcmp(p->hash,s->fixture,32)||
       n>g2d_input_remaining(p))return reject(p);
    p->last_now=now;
    if(p->header_used<32){
        memcpy(p->header+p->header_used,bytes,n);p->header_used+=n;
        if(p->header_used!=32)return 0;
        if(le32(p->header)!=G2D_UPLOAD_MAGIC||le32(p->header+4)!=1||
           le32(p->header+8)!=p->expected||le32(p->header+12)||
           le64(p->header+16)!=p->epoch||le64(p->header+24)!=p->generation)return reject(p);
        return 0;
    }
    memcpy(p->bytes+p->received,bytes,n);p->received+=n;
    if(p->received!=p->expected)return 0;
    uint8_t digest[32];storage_digest_bytes(p->bytes,p->received,digest);
    if(memcmp(digest,p->hash,32)||clock(context,&now)!=1||now<p->last_now||now>=deadline)return reject(p);
    p->last_now=now;atomic_store_explicit(&p->committed,1,memory_order_release);return 1;
}
