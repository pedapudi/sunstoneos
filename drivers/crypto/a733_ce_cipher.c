#include "a733_ce_cipher.h"
enum { REQUEST,WAIT,CLEAN,PUBLISH,DONE };
static void zero(void *p,size_t n) {volatile uint8_t *b=p;while (n--) *b++=0;}
static int outside(const struct a733_ce_cipher *s,const void *p,size_t n)
{return a733_ce_separate(s,sizeof(*s),p,n) && a733_ce_session_outside(s->bound_engine,p,n);}
static void private_clear(struct a733_ce_cipher *s)
{
    zero(s->key,sizeof(s->key));zero(s->iv,sizeof(s->iv));zero(s->hash_subkey,sizeof(s->hash_subkey));
    zero(s->j0,sizeof(s->j0));zero(s->tag,sizeof(s->tag));zero(s->nonce,sizeof(s->nonce));zero(s->aad,sizeof(s->aad));
    zero(s->input,sizeof(s->input));zero(s->output,sizeof(s->output));
}
static enum a733_ce_cipher_result finish(struct a733_ce_cipher *s,enum a733_ce_cipher_result r)
{atomic_store_explicit(&s->busy,0,memory_order_release);return r;}
static enum a733_ce_cipher_result retain(struct a733_ce_cipher *s)
{
    s->fault=1;private_clear(s);
    if (a733_ce_session_retain(s->bound_engine)==A733_CE_BUSY) return finish(s,A733_CE_CIPHER_BUSY);
    s->terminal=1;return finish(s,A733_CE_CIPHER_RETAINED);
}
static int bound(struct a733_ce_cipher *s)
{return s->self==(uintptr_t)s && s->engine==s->bound_engine && a733_ce_session_bound(s->bound_engine);}
static int time_ok(struct a733_ce_cipher *s)
{
    uint64_t now;
    if (s->bound_engine->binding.ops.now_us(s->bound_engine->binding.cookie,&now)!=1 ||
        now<s->last_now || now>=s->deadline) return 0;
    s->last_now=now;return 1;
}
/* GHASH multiplication in GF(2^128) (SP 800-38D 6.3) without data-dependent
 * branches: x = x*y. */
static void multiply(uint8_t x[16],const uint8_t y[16])
{
    uint8_t z[16]={0},v[16];
    for (unsigned i=0;i<16;++i) v[i]=y[i];
    for (unsigned i=0;i<128;++i) {
        uint8_t bit=(uint8_t)-((x[i/8]>>(7-i%8))&1),reduce=(uint8_t)-(v[15]&1);
        for (unsigned j=0;j<16;++j) z[j]^=v[j]&bit;
        for (unsigned j=15;j;--j) v[j]=(uint8_t)((v[j]>>1)|(v[j-1]<<7));
        v[0]=(uint8_t)((v[0]>>1)^(0xe1&reduce));
    }
    for (unsigned i=0;i<16;++i) x[i]=z[i];
    zero(z,sizeof(z));zero(v,sizeof(v));
}
/* Absorb data, zero-padded to whole blocks, into the GHASH state y. */
static void absorb(uint8_t y[16],const uint8_t h[16],const uint8_t *data,size_t bytes)
{
    for (size_t at=0;at<bytes;at+=16) {
        for (size_t i=0;i<16 && at+i<bytes;++i) y[i]^=data[at+i];
        multiply(y,h);
    }
}
/* Absorb the final block of two 64-bit big-endian bit lengths. */
static void lengths(uint8_t y[16],const uint8_t h[16],uint64_t first,uint64_t second)
{
    uint8_t block[16];
    for (unsigned i=0;i<8;++i) {block[7-i]=(uint8_t)((first*8)>>(8*i));block[15-i]=(uint8_t)((second*8)>>(8*i));}
    absorb(y,h,block,16);
}
/* Add blocks to the low 32 or 128 bits of a big-endian counter block, modulo
 * that width, as the engine's counter does. */
static void advance(uint8_t counter[16],uint64_t blocks,unsigned width_bytes)
{
    unsigned carry=0;
    for (unsigned i=0;i<width_bytes;++i) {
        unsigned v=counter[15-i]+(unsigned)(blocks&255)+carry;
        counter[15-i]=(uint8_t)v;carry=v>>8;blocks>>=8;
    }
}
static size_t output_bytes(const struct a733_ce_cipher *s)
{return s->mode!=A733_CE_CIPHER_GCM ? s->length:s->length+(s->decrypt?0:s->tag_bytes);}
int a733_ce_cipher_bind(struct a733_ce_cipher *s,struct a733_ce_session *engine)
{
    if (!s || s->initialized || !a733_ce_session_bound(engine) ||
        !(a733_ce_capabilities(engine->binding.config.profile)&A733_CE_CAP_AES) ||
        !a733_ce_session_outside(engine,s,sizeof(*s))) return 0;
    s->self=(uintptr_t)s;s->engine=engine;s->bound_engine=engine;s->initialized=1;s->next_generation=1;
    atomic_init(&s->busy,0);return 1;
}
enum a733_ce_cipher_result a733_ce_cipher_begin(struct a733_ce_cipher *s,const uint8_t key[16],
    const uint8_t *input,size_t bytes,unsigned decrypt,uint64_t deadline,uint64_t *generation)
{
    const struct a733_ce_cipher_params c={.key=key,.key_bytes=16,.mode=A733_CE_CIPHER_ECB,.decrypt=decrypt};
    return a733_ce_cipher_start(s,&c,input,bytes,deadline,generation);
}
enum a733_ce_cipher_result a733_ce_cipher_start(struct a733_ce_cipher *s,const struct a733_ce_cipher_params *c,
    const uint8_t *input,size_t bytes,uint64_t deadline,uint64_t *generation)
{
    if (!s || !s->initialized) return A733_CE_CIPHER_INVALID;
    if (atomic_exchange_explicit(&s->busy,1,memory_order_acquire)) return A733_CE_CIPHER_BUSY;
    if (s->terminal) return finish(s,A733_CE_CIPHER_RETAINED);
    if (s->fault || !bound(s)) return retain(s);
    if (!c || !outside(s,c,sizeof(*c))) return finish(s,A733_CE_CIPHER_INVALID);
    const struct a733_ce_cipher_params r=*c;
    unsigned gcm=r.mode==A733_CE_CIPHER_GCM,blocks=r.mode==A733_CE_CIPHER_ECB || r.mode==A733_CE_CIPHER_CBC;
    size_t length=gcm && r.decrypt && bytes>=r.tag_bytes ? bytes-r.tag_bytes:bytes;
    const void *buffers[]={r.key,r.iv,r.aad,input};
    const size_t sizes[]={r.key_bytes,r.iv_bytes,r.aad_bytes,bytes};
    if (!deadline || !generation || r.mode>A733_CE_CIPHER_GCM || r.decrypt>1 || !r.key ||
        (r.key_bytes!=16 && r.key_bytes!=24 && r.key_bytes!=32) || !input || length>A733_CE_CIPHER_MAX_BYTES ||
        (r.mode==A733_CE_CIPHER_ECB && (r.iv || r.iv_bytes)) || (!gcm && r.mode!=A733_CE_CIPHER_ECB && r.iv_bytes!=16) ||
        (!gcm && (r.aad || r.aad_bytes || r.tag_bytes || !bytes)) || (blocks && bytes%16) ||
        (gcm && (!r.iv_bytes || r.iv_bytes>A733_CE_GCM_IV_MAX || r.aad_bytes>A733_CE_GCM_AAD_MAX ||
            r.tag_bytes<12 || r.tag_bytes>16 || bytes<(r.decrypt?r.tag_bytes:0))) ||
        (r.iv_bytes && !r.iv) || (r.aad_bytes && !r.aad) ||
        !outside(s,generation,sizeof(*generation)) || !s->next_generation) return finish(s,A733_CE_CIPHER_INVALID);
    for (unsigned i=0;i<4;++i)
        if ((sizes[i] || i==3) && (!outside(s,buffers[i],sizes[i]) ||
            !a733_ce_separate(buffers[i],sizes[i],generation,sizeof(*generation)))) return finish(s,A733_CE_CIPHER_INVALID);
    if (s->active) return finish(s,A733_CE_CIPHER_BUSY);
    uint64_t now;
    if (s->engine->binding.ops.now_us(s->engine->binding.cookie,&now)!=1 || now<s->last_now) return retain(s);
    s->last_now=now;if (now>=deadline) return finish(s,A733_CE_CIPHER_INVALID);
    enum a733_ce_result result=a733_ce_session_claim(s->engine,s,sizeof(*s),A733_CE_CLIENT_AES128,&s->session_generation);
    if (result==A733_CE_BUSY) return finish(s,A733_CE_CIPHER_BUSY);
    if (result==A733_CE_INVALID) return finish(s,A733_CE_CIPHER_INVALID);
    if (result!=A733_CE_PENDING) return retain(s);
    private_clear(s);
    /* GCM runs CTR from J0 over a leading zero block, whose output is E(K,J0). */
    size_t offset=gcm?16:0;
    for (size_t i=0;i<r.key_bytes;++i) s->key[i]=r.key[i];
    for (size_t i=0;i<length;++i) s->input[offset+i]=input[i];
    if (gcm) {
        for (size_t i=0;i<r.iv_bytes;++i) s->nonce[i]=r.iv[i];
        for (size_t i=0;i<r.aad_bytes;++i) s->aad[i]=r.aad[i];
        if (r.decrypt) for (size_t i=0;i<r.tag_bytes;++i) s->tag[i]=input[length+i];
    } else for (size_t i=0;i<r.iv_bytes;++i) s->iv[i]=r.iv[i];
    s->mode=r.mode;s->key_bytes=r.key_bytes;s->iv_bytes=r.iv_bytes;s->aad_bytes=r.aad_bytes;
    s->tag_bytes=gcm?r.tag_bytes:0;s->length=length;s->bytes=(offset+length+15)&~(size_t)15;
    s->deadline=deadline;s->completed=0;s->inflight=0;s->decrypt=r.decrypt;s->subkey=gcm;s->rejected=0;
    s->active=1;s->phase=REQUEST;s->generation=s->next_generation++;
    if (!time_ok(s)) return retain(s);
    *generation=s->generation;return finish(s,A733_CE_CIPHER_PENDING);
}
/* After the hash subkey task: J0 is IV || 0^31 || 1 for a 96-bit IV, otherwise
 * GHASH of the zero-padded IV and its length (SP 800-38D 7.1). */
static void counter_block(struct a733_ce_cipher *s)
{
    if (s->iv_bytes==12) {
        for (unsigned i=0;i<12;++i) s->j0[i]=s->nonce[i];
        s->j0[15]=1;
    } else {
        absorb(s->j0,s->hash_subkey,s->nonce,s->iv_bytes);lengths(s->j0,s->hash_subkey,0,s->iv_bytes);
    }
    for (unsigned i=0;i<16;++i) s->iv[i]=s->j0[i];
}
/* T = E(K,J0) xor GHASH(A,C). Decryption compares in constant time. */
static void tag(struct a733_ce_cipher *s)
{
    uint8_t y[16]={0};uint8_t difference=0;
    absorb(y,s->hash_subkey,s->aad,s->aad_bytes);
    absorb(y,s->hash_subkey,(s->decrypt?s->input:s->output)+16,s->length);
    lengths(y,s->hash_subkey,s->aad_bytes,s->length);
    for (unsigned i=0;i<16;++i) y[i]^=s->output[i];
    if (s->decrypt) {
        for (size_t i=0;i<s->tag_bytes;++i) difference|=y[i]^s->tag[i];
        s->rejected=difference!=0;
    } else for (unsigned i=0;i<16;++i) s->tag[i]=y[i];
    zero(y,sizeof(y));
}
enum a733_ce_cipher_result a733_ce_cipher_poll(struct a733_ce_cipher *s,uint64_t generation,uint8_t *output,size_t bytes)
{
    if (!s || !s->initialized) return A733_CE_CIPHER_INVALID;
    if (atomic_exchange_explicit(&s->busy,1,memory_order_acquire)) return A733_CE_CIPHER_BUSY;
    if (s->terminal) return finish(s,A733_CE_CIPHER_RETAINED);
    if (s->fault || !bound(s)) return retain(s);
    if (!generation || generation!=s->generation || bytes!=output_bytes(s) || !output || !outside(s,output,bytes))
        return finish(s,A733_CE_CIPHER_INVALID);
    if (s->phase==DONE) return finish(s,A733_CE_CIPHER_CONSUMED);
    if (!s->active || !a733_ce_session_client(s->engine,s,A733_CE_CLIENT_AES128,s->session_generation) || !time_ok(s))
        return retain(s);
    struct a733_ce *controller=&s->engine->controller;
    enum a733_ce_result r;
    unsigned gcm=s->mode==A733_CE_CIPHER_GCM;
    if (s->phase==REQUEST) {
        struct a733_ce_symmetric task={.key=s->key,.key_bytes=s->key_bytes};
        const uint8_t *in=s->subkey_block;size_t take=16;
        if (!s->subkey) {
            take=s->bytes-s->completed;if (take>1024) take=1024;
            in=s->input+s->completed;
            if (s->mode==A733_CE_CIPHER_ECB || s->mode==A733_CE_CIPHER_CBC) {task.mode=s->mode;task.decrypt=s->decrypt;}
            else {task.mode=A733_CE_AES_CTR;task.counter_bits=gcm?32:128;}
            if (s->mode!=A733_CE_CIPHER_ECB) task.iv=s->iv;
        }
        r=a733_ce_symmetric_request(controller,&task,in,take,s->deadline);
        if (controller->fault || !time_ok(s)) return retain(s);
        if (r==A733_CE_BUSY) return finish(s,A733_CE_CIPHER_BUSY);
        if (r!=A733_CE_PENDING) return retain(s);
        s->inflight=take;s->phase=WAIT;return finish(s,A733_CE_CIPHER_PENDING);
    }
    if (s->phase==WAIT) {
        r=a733_ce_cipher_task_poll(controller,s->subkey?s->hash_subkey:s->output+s->completed,s->inflight);
        if (controller->fault || !time_ok(s)) return retain(s);
        if (r==A733_CE_BUSY || r==A733_CE_PENDING)
            return finish(s,r==A733_CE_BUSY?A733_CE_CIPHER_BUSY:A733_CE_CIPHER_PENDING);
        if (r!=A733_CE_CIPHER) return retain(s);
        s->phase=CLEAN;return finish(s,A733_CE_CIPHER_PENDING);
    }
    if (s->phase==CLEAN) {
        r=a733_ce_cipher_clear_key(controller);
        if (controller->fault || !time_ok(s)) return retain(s);
        if (r==A733_CE_BUSY) return finish(s,A733_CE_CIPHER_BUSY);
        if (r!=A733_CE_KEY_CLEARED) return retain(s);
        if (s->subkey) {
            s->subkey=0;s->inflight=0;counter_block(s);
            s->phase=REQUEST;return finish(s,A733_CE_CIPHER_PENDING);
        }
        /* Chain the next task: CBC continues from the last ciphertext block,
         * CTR and GCM from the counter advanced past this task's blocks. */
        const uint8_t *last=(s->decrypt?s->input:s->output)+s->completed+s->inflight-16;
        if (s->mode==A733_CE_CIPHER_CBC) for (unsigned i=0;i<16;++i) s->iv[i]=last[i];
        else if (s->mode!=A733_CE_CIPHER_ECB) advance(s->iv,s->inflight/16,gcm?4:16);
        s->completed+=s->inflight;s->inflight=0;
        if (s->completed<s->bytes) {s->phase=REQUEST;return finish(s,A733_CE_CIPHER_PENDING);}
        if (gcm) tag(s);
        zero(s->key,sizeof(s->key));zero(s->hash_subkey,sizeof(s->hash_subkey));s->phase=PUBLISH;
    }
    if (s->phase!=PUBLISH || !time_ok(s)) return retain(s);
    enum a733_clock_result token=a733_clock_token_status(s->engine->binding.registry,&s->engine->binding.token);
    if (token==A733_CLOCK_BUSY) return finish(s,A733_CE_CIPHER_BUSY);
    if (token!=A733_CLOCK_OK || !a733_ce_session_done(s->engine,s,A733_CE_CLIENT_AES128,s->session_generation)) return retain(s);
    if (!s->rejected) {
        size_t offset=gcm?16:0;
        for (size_t i=0;i<s->length;++i) output[i]=s->output[offset+i];
        if (gcm && !s->decrypt) for (size_t i=0;i<s->tag_bytes;++i) output[s->length+i]=s->tag[i];
    }
    private_clear(s);
    s->active=0;s->phase=DONE;return finish(s,s->rejected?A733_CE_CIPHER_AUTH:A733_CE_CIPHER_OUTPUT);
}
