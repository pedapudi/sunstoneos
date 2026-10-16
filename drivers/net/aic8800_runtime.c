/* Native bounded radio receive and binding worker. Protocol and algorithm
 * references, source identities and licensing are in docs/radio-runtime.md. */
#include "aic8800_runtime.h"

static uint32_t rotate(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }
static void hash_block(uint32_t *hash, const uint8_t *p)
{
    static const uint32_t k[] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64], a=hash[0],b=hash[1],c=hash[2],d=hash[3],
        e=hash[4],g=hash[6],h=hash[7],v=hash[5];
    for (unsigned i=0;i<16;++i) w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|p[4*i+3];
    for (unsigned i=16;i<64;++i) {
        uint32_t x=w[i-15],y=w[i-2];
        w[i]=w[i-16]+(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-7]+(rotate(y,17)^rotate(y,19)^(y>>10));
    }
    for (unsigned i=0;i<64;++i) {
        uint32_t t1=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&v)^(~e&g))+k[i]+w[i];
        uint32_t t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=v;v=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    hash[0]+=a;hash[1]+=b;hash[2]+=c;hash[3]+=d;
    hash[4]+=e;hash[5]+=v;hash[6]+=g;hash[7]+=h;
}
static int span(const void *p, size_t n) { return p && n <= UINTPTR_MAX-(uintptr_t)p; }
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{ return (uintptr_t)a<(uintptr_t)b+bn && (uintptr_t)b<(uintptr_t)a+an; }
static uint8_t majority(uint8_t a,uint8_t b,uint8_t c) { return (uint8_t)((a&b)^(a&c)^(b&c)); }
static uint8_t choose(uint8_t a,uint8_t b,uint8_t c) { return (uint8_t)((a&b)^(~a&c)); }
/* Every binding hash is SHA-256 of exactly four bytes, with standard padding. */
static void expand(const uint8_t input[4], uint8_t mixed[36])
{
    uint8_t block[64]={input[0],input[1],input[2],input[3],0x80}; block[63]=32;
    uint32_t hash[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    hash_block(hash,block);
    for(unsigned i=0;i<4;++i) {
        for(unsigned j=0;j<8;++j) {
            unsigned n=i*8+j; mixed[i*9+j]=(uint8_t)(hash[n/4]>>(24-8*(n%4)));
        }
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
    if(!span(r,sizeof(*r))||!span(c,sizeof(*c))||overlap(r,sizeof(*r),c,sizeof(*c))||
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
    if(r->config.binding_timeout>UINT64_MAX-now) {
        aic8800_runtime_stop(r,r->owner,AIC8800_RUNTIME_TIME_ERROR);return 1;
    }
    r->binding_deadline=now+r->config.binding_timeout;return 1;
}
int aic8800_runtime_claim(struct aic8800_runtime *r,const void *owner,uint64_t now,const uint8_t *early)
{
    if(!r||!r->initialized||r->state!=AIC8800_RUNTIME_NEW||r->owner||!owner||
        (early&&(!span(early,16)||overlap(r,sizeof(*r),early,16))))return 0;
    r->owner=owner;r->last_now=now;r->state=AIC8800_RUNTIME_RECEIVING;
    if(early)(void)challenge(r,early,now);
    return 1;
}
static uint8_t crc(const uint8_t *p)
{
    uint8_t c=0;for(unsigned i=0;i<3;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(uint8_t)((c<<1)^((c&128)?7:0));}return c;
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
        r->request[0]=28;r->request[2]=0x11;r->request[3]=crc(r->request);
        r->request[8]=0x15;r->request[9]=4;r->request[10]=1;r->request[12]=100;r->request[14]=16;
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
    if(!r||!r->initialized||r->owner!=owner||!span(input,available)||!span(out,sizeof(*out))||
        overlap(r,sizeof(*r),input,available)||overlap(r,sizeof(*r),out,sizeof(*out))||
        overlap(input,available,out,sizeof(*out)))return AIC8800_CODEC_INVALID;
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
