/* Native bounded radio receive and binding worker. Protocol and algorithm
 * references, source identities and licensing are in docs/radio-runtime.md. */
#include "aic8800_runtime.h"
#include "a733_range.h"
#include "a733_storage_digest.h"

static uint8_t majority(uint8_t a,uint8_t b,uint8_t c) { return (uint8_t)((a&b)^(a&c)^(b&c)); }
static uint8_t choose(uint8_t a,uint8_t b,uint8_t c) { return (uint8_t)((a&b)^(~a&c)); }
/* Every binding hash is SHA-256 of exactly four bytes. */
static void expand(const uint8_t input[4], uint8_t mixed[36])
{
    uint8_t hash[32]; storage_digest_bytes(input,4,hash);
    for(unsigned i=0;i<4;++i) {
        for(unsigned j=0;j<8;++j) mixed[i*9+j]=hash[i*8+j];
        mixed[i*9+8]=input[i];
    }
}
static void confuse(const uint8_t input[4],uint8_t output[16])
{
    uint8_t mixed[36];expand(input,mixed);
    for(unsigned i=0;i<12;++i)output[i]=choose(mixed[i*3],mixed[i*3+1],mixed[i*3+2]);
    for(unsigned i=0;i<4;++i)output[12+i]=majority(output[i*3],output[i*3+1],output[i*3+2])^input[i];
}
void aic8800_runtime_stop(struct aic8800_runtime *r,const void *owner,enum aic8800_runtime_error e)
{
    if(!r||!r->initialized||r->owner!=owner||r->state==AIC8800_RUNTIME_FAULT)return;
    r->error=e;r->state=AIC8800_RUNTIME_FAULT;
}
void aic8800_runtime_observe(struct aic8800_runtime *r,const void *owner,uint64_t now)
{
    if(!r||!r->initialized||r->owner!=owner||r->state==AIC8800_RUNTIME_FAULT)return;
    if(now<r->last_now||(r->binding_seen&&!r->binding_confirmed&&now>=r->binding_deadline))
        aic8800_runtime_stop(r,owner,AIC8800_RUNTIME_TIME_ERROR);
    else r->last_now=now;
}
int aic8800_runtime_init(struct aic8800_runtime *r,const struct aic8800_runtime_config *c)
{
    if(!a733_span_excl(r,sizeof(*r))||!a733_span_excl(c,sizeof(*c))||a733_extents_overlap_prechecked((uintptr_t)r,sizeof(*r),(uintptr_t)c,sizeof(*c))||
       r->initialized||!c->read_platform_id||!c->binding_timeout)return 0;
    struct aic8800_runtime initial={0};initial.config=*c;initial.initialized=1;initial.binding_status=255;
    *r=initial;return 1;
}
static int challenge(struct aic8800_runtime *r,const uint8_t *bytes,uint64_t now)
{
    if(r->binding_seen)return 0;
    /* Copy before any operation that can fault: received work stays owned. */
    for(unsigned i=0;i<16;++i)r->challenge[i]=bytes[i];
    r->binding_seen=1;r->stage=0;r->state=AIC8800_RUNTIME_VERIFYING;
    if(!a733_deadline_after(now,r->config.binding_timeout,&r->binding_deadline))
        aic8800_runtime_stop(r,r->owner,AIC8800_RUNTIME_TIME_ERROR);
    return 1;
}
int aic8800_runtime_claim(struct aic8800_runtime *r,const void *owner,uint64_t now,const uint8_t *early)
{
    if(!r||!r->initialized||r->state!=AIC8800_RUNTIME_NEW||r->owner||!owner||
        (early&&(!a733_span_excl(early,16)||a733_extents_overlap_prechecked((uintptr_t)r,sizeof(*r),(uintptr_t)early,16))))return 0;
    r->owner=owner;r->last_now=now;r->state=AIC8800_RUNTIME_RECEIVING;
    if(early)(void)challenge(r,early,now);
    return 1;
}
void aic8800_runtime_step(struct aic8800_runtime *r,const void *owner,uint64_t now)
{
    if(!r||!r->initialized||r->owner!=owner)return;
    aic8800_runtime_observe(r,owner,now);
    if(r->state!=AIC8800_RUNTIME_VERIFYING)return;
    uint8_t bytes[36],tmp[16];
    switch(r->stage) {
    case 0:
        if(r->config.read_platform_id(r->config.context,r->platform_id)!=1) {
            aic8800_runtime_stop(r,owner,AIC8800_RUNTIME_PLATFORM_ERROR);return;
        }
        for(unsigned i=0;i<4;++i)r->random[i]=majority(r->challenge[3*i],r->challenge[3*i+1],r->challenge[3*i+2])^r->challenge[12+i];
        break;
    case 1:
        confuse(r->random,tmp);
        for(unsigned i=0;i<16;++i)if(tmp[i]!=r->challenge[i]) {
            aic8800_runtime_stop(r,owner,AIC8800_RUNTIME_CHALLENGE_ERROR);return;
        }
        break;
    case 2:
        expand(r->random,bytes);
        for(unsigned i=0;i<12;++i)tmp[i]=majority(bytes[3*i],bytes[3*i+1],bytes[3*i+2]);
        for(unsigned i=0;i<4;++i)r->encrypted[i]=choose(tmp[3*i],tmp[3*i+1],tmp[3*i+2]);
        break;
    case 3: confuse(r->encrypted,r->reply);break;
    case 4:
        for(unsigned i=0;i<sizeof(r->request);++i)r->request[i]=0;
        aic8800_sdio_command_header(r->request,0x0415,16);
        for(unsigned i=0;i<16;++i)r->request[16+i]=r->reply[i];
        r->state=AIC8800_RUNTIME_REQUEST;break;
    default:aic8800_runtime_stop(r,owner,AIC8800_RUNTIME_MESSAGE_ERROR);return;
    }
    ++r->stage;
}
int aic8800_runtime_sent(struct aic8800_runtime *r,const void *owner)
{
    if(!r||!r->initialized||r->owner!=owner||r->state!=AIC8800_RUNTIME_REQUEST)return 0;
    r->state=AIC8800_RUNTIME_WAITING;return 1;
}
enum aic8800_codec_result aic8800_runtime_response(struct aic8800_runtime *r,
    const void *owner,const void *input,size_t available,uint64_t now,struct aic8800_sdio_response *out)
{
    if(!r||!r->initialized||r->owner!=owner||!a733_span_excl(input,available)||!a733_span_excl(out,sizeof(*out))||
        a733_extents_overlap_prechecked((uintptr_t)r,sizeof(*r),(uintptr_t)input,available)||
        a733_extents_overlap_prechecked((uintptr_t)r,sizeof(*r),(uintptr_t)out,sizeof(*out))||
        a733_extents_overlap_prechecked((uintptr_t)input,available,(uintptr_t)out,sizeof(*out)))return AIC8800_CODEC_INVALID;
    aic8800_runtime_observe(r,owner,now);
    if(r->state==AIC8800_RUNTIME_FAULT)return AIC8800_CODEC_INVALID;
    const uint8_t *p=input;struct aic8800_sdio_response v={0};
    if(available<4)return AIC8800_CODEC_TRUNCATED;
    size_t length=(size_t)p[0]|(size_t)p[1]<<8;
    if(!length)return AIC8800_CODEC_END;
    if(!(p[2]&0x10)) {
        /* Source SDIO data includes its 60-byte hardware receive header. */
        v.record=p;v.record_bytes=length+60;v.consumed=(v.record_bytes+3)&~(size_t)3;
        v.transport_type=p[2];
        if(v.consumed>available)return AIC8800_CODEC_TRUNCATED;
        *out=v;return AIC8800_CODEC_UNRELATED;
    }
    enum aic8800_codec_result result=aic8800_sdio_decode_identity_response(
        input,available,AIC8800_IDENTITY_CHIP,&v);
    if(result!=AIC8800_CODEC_OK&&result!=AIC8800_CODEC_UNRELATED)return result;
    if(v.has_message&&v.message_id==0x0417) {
        if(v.parameter_bytes!=16)return AIC8800_CODEC_MALFORMED;
        if(challenge(r,v.parameters,now)) { *out=v;return AIC8800_CODEC_OK; }
    } else if(v.has_message&&v.message_id==0x0416&&r->state==AIC8800_RUNTIME_WAITING) {
        /* The published sender passes a one-byte destination; nonzero fails. */
        if(v.parameter_bytes!=1)return AIC8800_CODEC_MALFORMED;
        r->binding_status=v.parameters[0];
        if(r->binding_status)aic8800_runtime_stop(r,owner,AIC8800_RUNTIME_BINDING_REJECTED);
        else {r->binding_confirmed=1;r->state=AIC8800_RUNTIME_RECEIVING;}
        *out=v;return AIC8800_CODEC_OK;
    }
    *out=v;return AIC8800_CODEC_UNRELATED;
}
