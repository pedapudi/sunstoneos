#include "input.h"
#include "a733_storage_digest.h"
#include <string.h>
__attribute__((used,retain)) const uint64_t jpeg_runtime_geometry[6]={
 A733_JPEG_SESSION_MAGIC,2,856,JPEG_INPUT_CAPACITY,JPEG_OUTPUT_CAPACITY,JPEG_RESULT_HEADER};
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t le64(const uint8_t *p){return le32(p)|((uint64_t)le32(p+4)<<32);}
static int present(const uint8_t *p){uint8_t v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
int jpeg_input_geometry(struct jpeg_input *p,const struct a733_video_jpeg *j)
{
    if(!p||!j||!j->width||!j->height||j->width>1920||j->height>1080)return 0;
    uint32_t w=j->width,h=j->height,stride=(w+31u)&~31u,rows=(h+31u)&~31u;
    uint32_t luma=stride*rows,chroma=luma/2u,output=w*h+2u*((w+1u)/2u)*((h+1u)/2u);
    if(luma>JPEG_PLANE_CAPACITY||chroma>JPEG_PLANE_CAPACITY||output>JPEG_OUTPUT_CAPACITY)return 0;
    p->width=w;p->height=h;p->stride=stride;p->rows=rows;
    p->luma_bytes=luma;p->chroma_bytes=chroma;p->output_bytes=output;return 1;
}
uint32_t jpeg_input_mapped(const struct jpeg_input *p,unsigned buffer)
{
    size_t n=buffer==0?p->input_bytes:buffer==1?p->luma_bytes:p->chroma_bytes;
    return (uint32_t)((n+4095u)&~(size_t)4095u);
}
int jpeg_input_ready(const struct jpeg_input *p)
{return p&&atomic_load_explicit(&p->committed,memory_order_acquire)==1;}
size_t jpeg_input_remaining(const struct jpeg_input *p)
{
    if(!p||p->failed||jpeg_input_ready(p))return 0;
    if(p->header_used<32)return 32-p->header_used;
    return p->received<p->input_bytes?p->input_bytes-p->received:p->input_bytes+p->oracle_bytes-p->received;
}
static int reject(struct jpeg_input *p){if(p)p->failed=1;return -1;}
int jpeg_input_feed(struct jpeg_input *p,const struct a733_jpeg_session *s,
    int (*clock)(void *,uint64_t *),void *context,uint64_t deadline,const uint8_t *bytes,size_t n)
{
    uint64_t now;
    if(!p||!s||!clock||!bytes||!n||n>JPEG_INPUT_CAPACITY||p->failed||jpeg_input_ready(p)||
       !deadline||clock(context,&now)!=1||now>=deadline||!s->board.epoch||!s->board.generation||
       !s->authority||!present(s->fixture))return reject(p);
    if(!p->header_used){
        p->epoch=s->board.epoch;p->generation=s->board.generation;p->authority=s->authority;p->deadline=deadline;
        memcpy(p->input_hash,s->fixture,32);memcpy(p->oracle_hash,s->reference,32);
    }
    if(p->epoch!=s->board.epoch||p->generation!=s->board.generation||p->authority!=s->authority||
       p->deadline!=deadline||now<p->last_now||memcmp(p->input_hash,s->fixture,32)||
       memcmp(p->oracle_hash,s->reference,32)||n>jpeg_input_remaining(p))return reject(p);
    p->last_now=now;
    if(p->header_used<32){
        memcpy(p->header+p->header_used,bytes,n);p->header_used+=n;
        if(p->header_used!=32)return 0;
        p->input_bytes=le32(p->header+8);p->oracle_bytes=le32(p->header+12);
        if(le32(p->header)!=JPEG_UPLOAD_MAGIC||le32(p->header+4)!=1||
           !p->input_bytes||p->input_bytes>JPEG_INPUT_CAPACITY||p->oracle_bytes>JPEG_OUTPUT_CAPACITY||
           present(p->oracle_hash)!=(p->oracle_bytes!=0)||le64(p->header+16)!=p->epoch||
           le64(p->header+24)!=p->generation)return reject(p);
        return 0;
    }
    if(p->received<p->input_bytes)memcpy(p->bytes+p->received,bytes,n);
    else memcpy(p->oracle+(p->received-p->input_bytes),bytes,n);
    p->received+=n;
    if(p->received!=p->input_bytes+p->oracle_bytes)return 0;
    uint8_t digest[32];struct a733_video_jpeg picture;
    storage_digest_bytes(p->bytes,p->input_bytes,digest);
    if(memcmp(digest,p->input_hash,32)||!a733_video_jpeg_parse(p->bytes,p->input_bytes,&picture)||
       !jpeg_input_geometry(p,&picture)||(p->oracle_bytes&&p->oracle_bytes!=p->output_bytes))return reject(p);
    if(p->oracle_bytes){storage_digest_bytes(p->oracle,p->oracle_bytes,digest);
        if(memcmp(digest,p->oracle_hash,32))return reject(p);}
    if(clock(context,&now)!=1||now<p->last_now||now>=deadline)return reject(p);
    p->last_now=now;atomic_store_explicit(&p->committed,1,memory_order_release);return 1;
}
