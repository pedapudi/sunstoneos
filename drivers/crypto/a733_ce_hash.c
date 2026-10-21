#include "a733_ce_hash.h"
enum { OPEN,REQUEST,WAIT,DONE };
/* Stream passes: HMAC hashes an over-long key first and ends with OUTER. */
enum { INNER,KEY,OUTER };
static void zero(void *p,size_t n) {volatile uint8_t *b=p;while (n--) *b++=0;}
static int outside(const struct a733_ce_hash *h,const void *p,size_t n)
{return a733_ce_separate(h,sizeof(*h),p,n) && a733_ce_session_outside(h->bound_engine,p,n);}
static enum a733_ce_hash_result finish(struct a733_ce_hash *h,enum a733_ce_hash_result r)
{atomic_store_explicit(&h->busy,0,memory_order_release);return r;}
static enum a733_ce_hash_result retain(struct a733_ce_hash *h)
{
    h->fault=1;zero(h->buffer,sizeof(h->buffer));zero(h->digest,sizeof(h->digest));zero(h->key,sizeof(h->key));
    if (a733_ce_session_retain(h->bound_engine)==A733_CE_BUSY) return finish(h,A733_CE_HASH_BUSY);
    h->terminal=1;return finish(h,A733_CE_HASH_RETAINED);
}
static int bound(struct a733_ce_hash *h)
{return h->self==(uintptr_t)h && h->engine==h->bound_engine && a733_ce_session_bound(h->bound_engine);}
static int time_ok(struct a733_ce_hash *h)
{
    uint64_t now;
    if (h->bound_engine->binding.ops.now_us(h->bound_engine->binding.cookie,&now)!=1 ||
        now<h->last_now || now>=h->deadline) return 0;
    h->last_now=now;return 1;
}
/* Append standard SHA padding to a queued tail shorter than 2048 bytes. */
static void pad(struct a733_ce_hash *h)
{
    unsigned block=a733_ce_hash_block(h->method);
    size_t padded=(h->queued+1+block/8+block-1)&~(size_t)(block-1);
    h->buffer[h->queued]=0x80;
    for (size_t i=h->queued+1;i<padded;++i) h->buffer[i]=0;
    uint64_t bits=h->message_bytes*8;
    for (unsigned i=0;i<8;++i) h->buffer[padded-1-i]=(uint8_t)(bits>>(8*i));
    h->queued=padded;h->finalizing=1;h->phase=REQUEST;
}
/* Queue the block-sized HMAC key combined with the inner or outer pad byte. */
static void keyed(struct a733_ce_hash *h,uint8_t pad_byte)
{
    unsigned block=a733_ce_hash_block(h->method);
    for (unsigned i=0;i<block;++i) h->buffer[h->queued+i]=h->key[i]^pad_byte;
    h->queued+=block;h->message_bytes+=block;
}
static int active(struct a733_ce_hash *h)
{return h->active && a733_ce_session_client(h->bound_engine,h,A733_CE_CLIENT_SHA256,h->session_generation);}
int a733_ce_hash_bind(struct a733_ce_hash *h,struct a733_ce_session *engine)
{
    if (!h || h->initialized || !a733_ce_session_bound(engine) ||
        !(a733_ce_capabilities(engine->binding.config.profile)&A733_CE_CAP_SHA256) || !a733_ce_session_outside(engine,h,sizeof(*h))) return 0;
    h->self=(uintptr_t)h;h->engine=engine;h->bound_engine=engine;h->initialized=1;h->next_generation=1;
    atomic_init(&h->busy,0);return 1;
}
enum a733_ce_hash_result a733_ce_hash_begin(struct a733_ce_hash *h,uint64_t deadline,uint64_t *generation)
{return a733_ce_hash_start(h,A733_CE_SHA256,0,0,deadline,generation);}
enum a733_ce_hash_result a733_ce_hash_start(struct a733_ce_hash *h,enum a733_ce_hash_method method,
    const uint8_t *key,size_t key_bytes,uint64_t deadline,uint64_t *generation)
{
    if (!h || !h->initialized) return A733_CE_HASH_INVALID;
    if (atomic_exchange_explicit(&h->busy,1,memory_order_acquire)) return A733_CE_HASH_BUSY;
    if (h->terminal) return finish(h,A733_CE_HASH_RETAINED);
    if (h->fault || !bound(h)) return retain(h);
    unsigned block=a733_ce_hash_block(method);
    if (!deadline || !generation || !outside(h,generation,sizeof(*generation)) || !h->next_generation ||
        !block || (!key && key_bytes) || key_bytes>A733_CE_HMAC_KEY_MAX ||
        (key_bytes && (!outside(h,key,key_bytes) || !a733_ce_separate(key,key_bytes,generation,sizeof(*generation)))))
        return finish(h,A733_CE_HASH_INVALID);
    if (h->active) return finish(h,A733_CE_HASH_BUSY);
    uint64_t now;
    if (h->engine->binding.ops.now_us(h->engine->binding.cookie,&now)!=1 || now<h->last_now) return retain(h);
    h->last_now=now;if (now>=deadline) return finish(h,A733_CE_HASH_INVALID);
    enum a733_ce_result r=a733_ce_session_claim(h->engine,h,sizeof(*h),A733_CE_CLIENT_SHA256,&h->session_generation);
    if (r==A733_CE_BUSY) return finish(h,A733_CE_HASH_BUSY);
    if (r==A733_CE_INVALID) return finish(h,A733_CE_HASH_INVALID);
    if (r!=A733_CE_PENDING) return retain(h);
    zero(h->buffer,sizeof(h->buffer));zero(h->digest,sizeof(h->digest));zero(h->key,sizeof(h->key));
    h->deadline=deadline;h->message_bytes=0;h->processed_bytes=0;h->queued=0;h->inflight=0;
    h->active=1;h->phase=OPEN;h->finalizing=0;h->generation=h->next_generation++;
    h->method=method;h->hmac=!!key;h->pass=INNER;
    if (key_bytes>block) {
        for (size_t i=0;i<key_bytes;++i) h->buffer[i]=key[i];
        h->queued=key_bytes;h->message_bytes=key_bytes;h->pass=KEY;pad(h);
    } else if (key) {
        for (size_t i=0;i<key_bytes;++i) h->key[i]=key[i];
        keyed(h,0x36);
    }
    *generation=h->generation;return finish(h,h->phase==REQUEST ? A733_CE_HASH_PENDING:A733_CE_HASH_READY);
}
enum a733_ce_hash_result a733_ce_hash_update(struct a733_ce_hash *h,uint64_t generation,const uint8_t *input,
    size_t bytes,size_t *accepted)
{
    if (!h || !h->initialized) return A733_CE_HASH_INVALID;
    if (atomic_exchange_explicit(&h->busy,1,memory_order_acquire)) return A733_CE_HASH_BUSY;
    if (h->terminal) return finish(h,A733_CE_HASH_RETAINED);
    if (h->fault || !bound(h)) return retain(h);
    if (!generation || generation!=h->generation || !bytes || bytes>2048 || !accepted ||
        !outside(h,input,bytes) || !outside(h,accepted,sizeof(*accepted)) ||
        !a733_ce_separate(input,bytes,accepted,sizeof(*accepted)) || h->finalizing || !h->active)
        return finish(h,A733_CE_HASH_INVALID);
    if (!active(h)) return retain(h);
    if (h->phase!=OPEN) return finish(h,A733_CE_HASH_BUSY);
    size_t take=2048-h->queued;if (take>bytes) take=bytes;
    if (h->message_bytes>A733_CE_HASH_MAX_BYTES || take>A733_CE_HASH_MAX_BYTES-h->message_bytes)
        return finish(h,A733_CE_HASH_INVALID);
    if (!time_ok(h)) return retain(h);
    for (size_t i=0;i<take;++i) h->buffer[h->queued+i]=input[i];
    h->queued+=take;h->message_bytes+=take;
    if (!time_ok(h)) return retain(h);
    *accepted=take;if (h->queued==2048) h->phase=REQUEST;
    return finish(h,h->phase==REQUEST ? A733_CE_HASH_PENDING:A733_CE_HASH_READY);
}
enum a733_ce_hash_result a733_ce_hash_final(struct a733_ce_hash *h,uint64_t generation)
{
    if (!h || !h->initialized) return A733_CE_HASH_INVALID;
    if (atomic_exchange_explicit(&h->busy,1,memory_order_acquire)) return A733_CE_HASH_BUSY;
    if (h->terminal) return finish(h,A733_CE_HASH_RETAINED);
    if (h->fault || !bound(h)) return retain(h);
    if (!generation || generation!=h->generation || !h->active || h->finalizing) return finish(h,A733_CE_HASH_INVALID);
    if (!active(h)) return retain(h);
    if (h->phase!=OPEN) return finish(h,A733_CE_HASH_BUSY);
    if (!time_ok(h)) return retain(h);
    if (h->message_bytes>A733_CE_HASH_MAX_BYTES || h->queued>=2048) return retain(h);
    pad(h);
    if (!time_ok(h)) return retain(h);
    return finish(h,A733_CE_HASH_PENDING);
}
enum a733_ce_hash_result a733_ce_hash_poll(struct a733_ce_hash *h,uint64_t generation,uint8_t digest[32])
{return a733_ce_hash_poll_digest(h,generation,digest,32);}
enum a733_ce_hash_result a733_ce_hash_poll_digest(struct a733_ce_hash *h,uint64_t generation,uint8_t *digest,size_t bytes)
{
    if (!h || !h->initialized) return A733_CE_HASH_INVALID;
    if (atomic_exchange_explicit(&h->busy,1,memory_order_acquire)) return A733_CE_HASH_BUSY;
    if (h->terminal) return finish(h,A733_CE_HASH_RETAINED);
    if (h->fault || !bound(h)) return retain(h);
    if (!generation || generation!=h->generation || bytes!=a733_ce_hash_digest(h->method) ||
        !outside(h,digest,bytes)) return finish(h,A733_CE_HASH_INVALID);
    if (h->phase==DONE) return finish(h,A733_CE_HASH_CONSUMED);
    if (!active(h) || !time_ok(h)) return retain(h);
    if (h->phase==OPEN) return finish(h,A733_CE_HASH_READY);
    struct a733_ce *controller=&h->engine->controller;
    if (h->phase==REQUEST) {
        size_t take=h->queued;if (take>2048) take=2048;
        enum a733_ce_result r=a733_ce_digest_request(controller,h->method,h->buffer,take,h->digest,
            h->processed_bytes!=0,(h->processed_bytes+take)*8,h->deadline);
        if (controller->fault || !time_ok(h)) return retain(h);
        if (r==A733_CE_BUSY) return finish(h,A733_CE_HASH_BUSY);
        if (r!=A733_CE_PENDING) return retain(h);
        h->inflight=take;h->phase=WAIT;return finish(h,A733_CE_HASH_PENDING);
    }
    unsigned state=a733_ce_hash_state(h->method),out=a733_ce_hash_digest(h->method);
    uint8_t result[64]={0};enum a733_ce_result r=a733_ce_digest_poll(controller,result,state);
    if (controller->fault || !time_ok(h) || (r!=A733_CE_PENDING && r!=A733_CE_BUSY && r!=A733_CE_DIGEST)) {
        zero(result,sizeof(result));return retain(h);
    }
    if (r!=A733_CE_DIGEST) {zero(result,sizeof(result));return finish(h,r==A733_CE_BUSY?A733_CE_HASH_BUSY:A733_CE_HASH_PENDING);}
    for (unsigned i=0;i<state;++i) h->digest[i]=result[i];zero(result,sizeof(result));
    h->processed_bytes+=h->inflight;h->queued-=h->inflight;
    for (size_t i=0;i<h->queued;++i) h->buffer[i]=h->buffer[h->inflight+i];
    zero(h->buffer+h->queued,sizeof(h->buffer)-h->queued);h->inflight=0;
    if (h->queued) {h->phase=REQUEST;return finish(h,A733_CE_HASH_PENDING);}
    if (!h->finalizing) {h->phase=OPEN;return finish(h,A733_CE_HASH_READY);}
    if (h->pass==KEY || (h->hmac && h->pass==INNER)) {
        /* Start the next pass from the default IV: the inner hash after the
         * hashed key, or the outer hash over the key block and inner digest. */
        unsigned pass=h->pass;
        if (pass==KEY) for (unsigned i=0;i<out;++i) h->key[i]=h->digest[i];
        h->queued=0;h->message_bytes=0;h->processed_bytes=0;h->finalizing=0;
        keyed(h,pass==KEY?0x36:0x5c);
        if (pass==INNER) {
            for (unsigned i=0;i<out;++i) h->buffer[h->queued+i]=h->digest[i];
            h->queued+=out;h->message_bytes+=out;
        }
        zero(h->digest,sizeof(h->digest));
        if (pass==KEY) {h->pass=INNER;h->phase=OPEN;return finish(h,A733_CE_HASH_READY);}
        h->pass=OUTER;pad(h);return finish(h,A733_CE_HASH_PENDING);
    }
    if (!time_ok(h) || !a733_ce_session_done(h->engine,h,A733_CE_CLIENT_SHA256,h->session_generation)) return retain(h);
    for (unsigned i=0;i<out;++i) digest[i]=h->digest[i];zero(h->digest,sizeof(h->digest));zero(h->key,sizeof(h->key));
    h->active=0;h->phase=DONE;return finish(h,A733_CE_HASH_DIGEST);
}
