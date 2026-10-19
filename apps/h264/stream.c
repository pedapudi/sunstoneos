#include "stream.h"
#include "a733_storage_digest.h"
#include <string.h>
struct h264_stream h264_stream;
uint32_t h264_stream_u32(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t u64(const uint8_t *p){return h264_stream_u32(p)|((uint64_t)h264_stream_u32(p+4)<<32);}
static int present(const uint8_t *p,size_t n){uint8_t v=0;for(size_t i=0;i<n;++i)v|=p[i];return v!=0;}
const uint8_t *h264_stream_descriptor(const struct h264_stream *s,unsigned i)
{return s&&i<s->count?s->manifest+i*H264_STREAM_DESCRIPTOR_BYTES:NULL;}
void h264_stream_fail(struct h264_stream *s)
{if(s)atomic_store_explicit(&s->failed,1,memory_order_release);}
static int fail(struct h264_stream *s){h264_stream_fail(s);return -1;}
size_t h264_stream_remaining(const struct h264_stream *s)
{
    if(!s)return 0;
    if(s->header_used<32)return 32-s->header_used;
    if(s->command==H264_STREAM_FRAME_MAGIC&&s->received<s->input_bytes)return s->input_bytes-s->received;
    return s->received<s->body_bytes?s->body_bytes-s->received:0;
}
static void reset_command(struct h264_stream *s)
{s->header_used=s->received=s->body_bytes=0;s->command=s->argument=0;}
static int manifest_valid(struct h264_stream *s)
{
    uint8_t hash[32];storage_digest_bytes(s->manifest,s->body_bytes,hash);
    if(memcmp(hash,s->manifest_hash,32))return 0;
    unsigned previous=0;
    for(unsigned i=0;i<s->count;++i){
        const uint8_t *d=h264_stream_descriptor(s,i);
        uint32_t n=h264_stream_u32(d),oracle=h264_stream_u32(d+4),f=h264_stream_u32(d+8);
        if(!n||n>H264_STREAM_INPUT_BYTES||(oracle&&oracle!=H264_STREAM_OUTPUT_BYTES)||
           f>15||(!i&&f)||(i&&f&&f!=previous+1)||h264_stream_u32(d+12)||
           !present(d+16,32)||(present(d+48,32)!=(oracle!=0))||present(d+80,16))return 0;
        previous=f;
    }
    return 1;
}
int h264_stream_feed(struct h264_stream *s,const struct a733_h264_session *session,
    int (*clock)(void *,uint64_t *),void *context,uint64_t deadline,const uint8_t *p,size_t n)
{
    uint64_t now;
    if(!h264_stream_storage_valid())return fail(s);
    if(!s||!session||session->version!=H264_STREAM_SESSION_VERSION||!clock||!p||!n||n>4096||!deadline||
       clock(context,&now)!=1||now>=deadline||!session->authority||!session->board.epoch||
       !session->board.generation||!present(session->fixture,32)||present(session->reference,32))return fail(s);
    if(!s->authority){s->epoch=session->board.epoch;s->generation=session->board.generation;
        s->authority=session->authority;s->deadline=deadline;memcpy(s->manifest_hash,session->fixture,32);}
    if(s->epoch!=session->board.epoch||s->generation!=session->board.generation||
       s->authority!=session->authority||s->deadline!=deadline||now<s->last_now||
       memcmp(s->manifest_hash,session->fixture,32)||n>h264_stream_remaining(s))return fail(s);
    s->last_now=now;
    if(s->header_used<32){
        memcpy(s->header+s->header_used,p,n);s->header_used+=n;if(s->header_used<32)return 0;
        s->command=h264_stream_u32(s->header);s->argument=h264_stream_u32(s->header+8);
        unsigned aux=h264_stream_u32(s->header+12);
        if(h264_stream_u32(s->header+4)!=1||u64(s->header+16)!=s->epoch||u64(s->header+24)!=s->generation)return fail(s);
        if(s->command==H264_STREAM_MANIFEST_MAGIC){
            if(atomic_load_explicit(&s->failed,memory_order_acquire)||atomic_load_explicit(&s->manifest_ready,memory_order_acquire)||
               !s->argument||s->argument>H264_STREAM_FRAMES||aux!=s->argument*H264_STREAM_DESCRIPTOR_BYTES)return fail(s);
            s->count=s->argument;s->body_bytes=aux;return 0;
        }
        if(!atomic_load_explicit(&s->manifest_ready,memory_order_acquire)||aux||s->argument>=s->count)return fail(s);
        if(s->command==H264_STREAM_QUERY_MAGIC){
            if(s->query_pending)return fail(s);s->query_pending=1;s->query_ordinal=s->argument;reset_command(s);return 1;
        }
        if(s->command==H264_STREAM_ACK_MAGIC){s->body_bytes=32;return 0;}
        if(s->command!=H264_STREAM_FRAME_MAGIC||atomic_load_explicit(&s->failed,memory_order_acquire)||
           atomic_load_explicit(&s->au_state,memory_order_acquire)||
           s->argument!=atomic_load_explicit(&s->next,memory_order_acquire))return fail(s);
        const uint8_t *d=h264_stream_descriptor(s,s->argument);
        s->input_bytes=h264_stream_u32(d);s->oracle_bytes=h264_stream_u32(d+4);
        s->body_bytes=s->input_bytes+s->oracle_bytes;return 0;
    }
    if(s->command==H264_STREAM_MANIFEST_MAGIC)memcpy(s->manifest+s->received,p,n);
    else if(s->command==H264_STREAM_ACK_MAGIC)memcpy(s->ack+s->received,p,n);
    else{
        if(atomic_load_explicit(&s->failed,memory_order_acquire)||atomic_load_explicit(&s->au_state,memory_order_acquire))return fail(s);
        /* Reads stop at the AU/oracle boundary; never form an out-of-array pointer. */
        if(s->received<s->input_bytes){if(n>s->input_bytes-s->received)return fail(s);memcpy(s->input+s->received,p,n);}
        else memcpy(s->oracle+(s->received-s->input_bytes),p,n);
    }
    s->received+=n;if(s->received<s->body_bytes)return 0;
    if(s->command==H264_STREAM_MANIFEST_MAGIC){
        if(!manifest_valid(s)||clock(context,&now)!=1||now<s->last_now||now>=deadline)return fail(s);
        s->last_now=now;atomic_store_explicit(&s->manifest_ready,1,memory_order_release);
    }else if(s->command==H264_STREAM_ACK_MAGIC){
        struct h264_stream_output *out=NULL;
        for(unsigned i=0;i<2;++i)if(atomic_load_explicit(&s->output[i].state,memory_order_acquire)==2&&s->output[i].ordinal==s->argument)out=&s->output[i];
        if(!out||!atomic_load_explicit(&out->delivered,memory_order_acquire)||memcmp(out->digest,s->ack,32))return fail(s);
        atomic_store_explicit(&out->state,0,memory_order_release);
    }else{
        const uint8_t *d=h264_stream_descriptor(s,s->argument);uint8_t hash[32];size_t config;
        struct a733_h264_picture picture;
        storage_digest_bytes(s->input,s->input_bytes,hash);
        if(memcmp(hash,d+16,32)||!a733_h264_parse_stream(H264_STREAM_PROFILE,s->input,s->input_bytes,h264_stream_u32(d+8),&picture,&config))return fail(s);
        storage_digest_bytes(s->input,config,hash);
        if(s->config_set&&memcmp(hash,s->config_hash,32))return fail(s);
        if(s->oracle_bytes){storage_digest_bytes(s->oracle,s->oracle_bytes,hash);if(memcmp(hash,d+48,32))return fail(s);}
        if(clock(context,&now)!=1||now<s->last_now||now>=deadline)return fail(s);s->last_now=now;
        if(!s->config_set){storage_digest_bytes(s->input,config,s->config_hash);s->config_bytes=config;s->config_set=1;}
        s->picture=picture;s->active=s->argument;
        atomic_store_explicit(&s->au_state,1,memory_order_release);
    }
    reset_command(s);return 1;
}
int h264_stream_take(struct h264_stream *s)
{
    if(!s||atomic_load_explicit(&s->failed,memory_order_acquire))return -1;
    if(atomic_load_explicit(&s->au_state,memory_order_acquire)!=1)return 0;
    for(unsigned i=0;i<2;++i){
        unsigned expected=0;
        if(atomic_compare_exchange_strong_explicit(&s->output[i].state,&expected,1,memory_order_acq_rel,memory_order_acquire)){
            s->copy_slot=i;s->output[i].ordinal=s->active;s->output[i].picture=s->picture;
            atomic_store_explicit(&s->output[i].delivered,0,memory_order_relaxed);
            atomic_store_explicit(&s->au_state,2,memory_order_release);return 1;
        }
    }
    return 0;
}
int h264_stream_authorized(const struct h264_stream *s,const struct a733_h264_run *r)
{
    return h264_stream_storage_valid()&&s&&r&&atomic_load_explicit(&s->manifest_ready,memory_order_acquire)&&
        !atomic_load_explicit(&s->failed,memory_order_acquire)&&
        atomic_load_explicit(&s->au_state,memory_order_acquire)==2&&
        r->picture_index==s->active&&r->frame_bytes==s->input_bytes&&
        r->job.profile==H264_STREAM_PROFILE&&r->job.input==s->input&&
        r->job.input_bytes==H264_STREAM_INPUT_BYTES&&
        !memcmp(&r->picture,&s->picture,sizeof(r->picture));
}
int h264_stream_complete(struct h264_stream *s)
{
    if(!s||atomic_load_explicit(&s->failed,memory_order_acquire)||
       atomic_load_explicit(&s->au_state,memory_order_acquire)!=2)return -1;
    struct h264_stream_output *out=&s->output[s->copy_slot];
    if(atomic_load_explicit(&out->state,memory_order_acquire)!=1||out->ordinal!=s->active)return fail(s);
    atomic_store_explicit(&out->state,2,memory_order_release);
    atomic_store_explicit(&s->next,s->active+1,memory_order_release);
    atomic_store_explicit(&s->au_state,0,memory_order_release);return 1;
}
