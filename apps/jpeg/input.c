#include "input.h"
#include "a733_storage_digest.h"
#include <string.h>
__attribute__((used,retain)) const uint64_t jpeg_runtime_geometry[6]={
 A733_JPEG_SESSION_MAGIC,2,856,JPEG_INPUT_CAPACITY,JPEG_OUTPUT_CAPACITY,JPEG_RESULT_HEADER};
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static int present(const uint8_t *p){uint8_t v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
static uint32_t page(size_t n){return (uint32_t)((n+4095u)&~(size_t)4095u);}
/* Padded luma, chroma and active output bytes of one admitted shape. */
static int sizes(uint32_t w,uint32_t h,uint32_t out[3])
{
    if(!w||!h||w>1920||h>1080)return 0;
    uint32_t luma=((w+31u)&~31u)*((h+31u)&~31u);
    out[0]=luma;out[1]=luma/2u;out[2]=w*h+2u*((w+1u)/2u)*((h+1u)/2u);
    return luma<=JPEG_PLANE_CAPACITY&&out[2]<=JPEG_OUTPUT_CAPACITY;
}
int jpeg_input_geometry(struct jpeg_input *p,const struct a733_video_jpeg *j)
{
    uint32_t n[3];
    if(!p||!j||!sizes(j->width,j->height,n))return 0;
    p->width=j->width;p->height=j->height;p->stride=(j->width+31u)&~31u;p->rows=(j->height+31u)&~31u;
    p->luma_bytes=n[0];p->chroma_bytes=n[1];p->output_bytes=n[2];return 1;
}
uint32_t jpeg_input_mapped(const struct jpeg_input *p,unsigned buffer)
{
    if(p->sequence)return buffer<3?p->mapped[buffer]:0;
    return page(buffer==0?p->input_bytes:buffer==1?p->luma_bytes:p->chroma_bytes);
}
unsigned jpeg_input_ready(const struct jpeg_input *p)
{return p?atomic_load_explicit(&p->committed,memory_order_acquire):0;}
size_t jpeg_input_remaining(const struct jpeg_input *p)
{
    if(!p||p->failed)return 0;
    if(!p->sequence&&jpeg_input_ready(p))return 0;
    if(p->manifest_used<p->manifest_bytes)return p->manifest_bytes-p->manifest_used;
    if(p->sequence&&jpeg_input_ready(p)==p->count)return 0;
    if(p->header_used<32)return 32-p->header_used;
    return p->received<p->input_bytes?p->input_bytes-p->received:p->input_bytes+p->oracle_bytes-p->received;
}
size_t jpeg_input_partial(const struct jpeg_input *p)
{
    if(!p||(!p->header_used&&p->manifest_used==p->manifest_bytes))return 0;
    return jpeg_input_remaining(p);
}
static int reject(struct jpeg_input *p){if(p)p->failed=1;return -1;}
/* Entry fields: LE32 JPEG bytes, oracle bytes, width and height, then the
 * JPEG and oracle digests. Every entry must fit the fixed capacities on its
 * own; the shared mappings take the largest input, luma and chroma. */
static int manifest(struct jpeg_input *p)
{
    uint8_t digest[32];const uint8_t *m=p->manifest;
    storage_digest_bytes(m,p->manifest_bytes,digest);
    if(memcmp(digest,p->fixture,32)||le32(m)!=JPEG_SEQUENCE_MAGIC||le32(m+4)!=1||
       JPEG_MANIFEST_BYTES(le32(m+8))!=p->manifest_bytes||le32(m+12))return 0;
    p->count=le32(m+8);
    uint32_t input=0,luma=0,chroma=0;
    for(unsigned i=0;i<p->count;++i){
        const uint8_t *e=m+JPEG_MANIFEST_BYTES(i);uint32_t n[3];
        if(!sizes(le32(e+8),le32(e+12),n)||!le32(e)||le32(e)>JPEG_INPUT_CAPACITY||
           (le32(e+4)&&le32(e+4)!=n[2])||!present(e+16)||present(e+48)!=(le32(e+4)!=0))return 0;
        if(le32(e)>input)input=le32(e);
        if(n[0]>luma)luma=n[0];
        if(n[1]>chroma)chroma=n[1];
    }
    p->mapped[0]=page(input);p->mapped[1]=page(luma);p->mapped[2]=page(chroma);return 1;
}
int jpeg_input_feed(struct jpeg_input *p,const struct a733_jpeg_session *s,
    int (*clock)(void *,uint64_t *),void *context,uint64_t deadline,const uint8_t *bytes,size_t n)
{
    uint64_t now;
    if(!p||!s||!clock||!bytes||!n||n>JPEG_INPUT_CAPACITY||p->failed||
       !deadline||clock(context,&now)!=1||now>=deadline||!s->board.epoch||!s->board.generation||
       !s->authority||!present(s->fixture))return reject(p);
    if(!p->authority){
        p->epoch=s->board.epoch;p->generation=s->board.generation;p->authority=s->authority;p->deadline=deadline;
        memcpy(p->fixture,s->fixture,32);memcpy(p->reference,s->reference,32);
        memcpy(p->input_hash,s->fixture,32);memcpy(p->oracle_hash,s->reference,32);
    }
    if(p->epoch!=s->board.epoch||p->generation!=s->board.generation||p->authority!=s->authority||
       p->deadline!=deadline||now<p->last_now||memcmp(p->fixture,s->fixture,32)||
       memcmp(p->reference,s->reference,32)||n>jpeg_input_remaining(p))return reject(p);
    p->last_now=now;
    if(!p->sequence&&p->header_used<32){
        memcpy(p->header+p->header_used,bytes,n);p->header_used+=n;
        if(p->header_used!=32)return 0;
        if(le32(p->header)!=JPEG_UPLOAD_MAGIC||le64(p->header+16)!=p->epoch||le64(p->header+24)!=p->generation)return reject(p);
        if(le32(p->header+4)==2){
            /* A sequence session binds its manifest digest; oracles are per image. */
            size_t length=le32(p->header+8);
            if(s->version!=3||present(p->reference)||le32(p->header+12)||length<JPEG_MANIFEST_BYTES(1)||
               length>JPEG_MANIFEST_BYTES(JPEG_SEQUENCE_CAPACITY)||(length-16u)%JPEG_MANIFEST_ENTRY)return reject(p);
            p->sequence=1;p->manifest_bytes=length;p->header_used=0;return 0;
        }
        p->input_bytes=le32(p->header+8);p->oracle_bytes=le32(p->header+12);
        if(s->version!=2||le32(p->header+4)!=1||
           !p->input_bytes||p->input_bytes>JPEG_INPUT_CAPACITY||p->oracle_bytes>JPEG_OUTPUT_CAPACITY||
           present(p->oracle_hash)!=(p->oracle_bytes!=0))return reject(p);
        return 0;
    }
    if(p->manifest_used<p->manifest_bytes){
        memcpy(p->manifest+p->manifest_used,bytes,n);p->manifest_used+=n;
        if(p->manifest_used!=p->manifest_bytes)return 0;
        return manifest(p)?0:reject(p);
    }
    unsigned published=jpeg_input_ready(p);
    if(p->header_used<32){
        /* The previous image's input and output stay with the decoder until
         * its result has been delivered. */
        if(published!=p->delivered)return reject(p);
        memcpy(p->header+p->header_used,bytes,n);p->header_used+=n;
        if(p->header_used!=32)return 0;
        const uint8_t *e=p->manifest+JPEG_MANIFEST_BYTES(published);
        if(le32(p->header)!=JPEG_IMAGE_MAGIC||le32(p->header+4)!=published||le32(p->header+8)!=le32(e)||
           le32(p->header+12)!=le32(e+4)||le64(p->header+16)!=p->epoch||le64(p->header+24)!=p->generation)return reject(p);
        p->input_bytes=le32(e);p->oracle_bytes=le32(e+4);p->received=0;
        memcpy(p->input_hash,e+16,32);memcpy(p->oracle_hash,e+48,32);return 0;
    }
    if(p->received<p->input_bytes)memcpy(p->bytes+p->received,bytes,n);
    else memcpy(p->oracle+(p->received-p->input_bytes),bytes,n);
    p->received+=n;
    if(p->received!=p->input_bytes+p->oracle_bytes)return 0;
    uint8_t digest[32];struct a733_video_jpeg picture;
    storage_digest_bytes(p->bytes,p->input_bytes,digest);
    if(memcmp(digest,p->input_hash,32)||!a733_video_jpeg_parse(p->bytes,p->input_bytes,&picture)||
       !jpeg_input_geometry(p,&picture)||(p->oracle_bytes&&p->oracle_bytes!=p->output_bytes))return reject(p);
    if(p->sequence){
        const uint8_t *e=p->manifest+JPEG_MANIFEST_BYTES(published);
        if(p->width!=le32(e+8)||p->height!=le32(e+12))return reject(p);
    }
    if(p->oracle_bytes){storage_digest_bytes(p->oracle,p->oracle_bytes,digest);
        if(memcmp(digest,p->oracle_hash,32))return reject(p);}
    if(clock(context,&now)!=1||now<p->last_now||now>=deadline)return reject(p);
    p->last_now=now;
    if(p->sequence)p->header_used=0;
    atomic_store_explicit(&p->committed,published+1,memory_order_release);return 1;
}
