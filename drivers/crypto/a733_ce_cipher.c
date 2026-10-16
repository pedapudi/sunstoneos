#include "a733_ce_cipher.h"
enum { REQUEST,WAIT,CLEAN,PUBLISH,DONE };
static void zero(void *p,size_t n) {volatile uint8_t *b=p;while (n--) *b++=0;}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);}
static int outside(const struct a733_ce_cipher *s,const void *p,size_t n)
{return separate(s,sizeof(*s),p,n) && a733_ce_session_outside(s->bound_engine,p,n);}
static void private_clear(struct a733_ce_cipher *s)
{zero(s->key,sizeof(s->key));zero(s->input,sizeof(s->input));zero(s->output,sizeof(s->output));}
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
int a733_ce_cipher_bind(struct a733_ce_cipher *s,struct a733_ce_session *engine)
{
    if (!s || s->initialized || !a733_ce_session_bound(engine) ||
        !(a733_ce_capabilities(engine->binding.config.profile)&A733_CE_CAP_AES128) ||
        !a733_ce_session_outside(engine,s,sizeof(*s))) return 0;
    s->self=(uintptr_t)s;s->engine=engine;s->bound_engine=engine;s->initialized=1;s->next_generation=1;
    atomic_init(&s->busy,0);return 1;
}
enum a733_ce_cipher_result a733_ce_cipher_begin(struct a733_ce_cipher *s,const uint8_t key[16],
    const uint8_t *input,size_t bytes,unsigned decrypt,uint64_t deadline,uint64_t *generation)
{
    if (!s || !s->initialized) return A733_CE_CIPHER_INVALID;
    if (atomic_exchange_explicit(&s->busy,1,memory_order_acquire)) return A733_CE_CIPHER_BUSY;
    if (s->terminal) return finish(s,A733_CE_CIPHER_RETAINED);
    if (s->fault || !bound(s)) return retain(s);
    if (!deadline || !generation || !bytes || bytes>A733_CE_CIPHER_MAX_BYTES || bytes%16 || decrypt>1 ||
        !outside(s,key,16) || !outside(s,input,bytes) || !outside(s,generation,sizeof(*generation)) ||
        !separate(key,16,generation,sizeof(*generation)) || !separate(input,bytes,generation,sizeof(*generation)) ||
        !s->next_generation) return finish(s,A733_CE_CIPHER_INVALID);
    if (s->active) return finish(s,A733_CE_CIPHER_BUSY);
    uint64_t now;
    if (s->engine->binding.ops.now_us(s->engine->binding.cookie,&now)!=1 || now<s->last_now) return retain(s);
    s->last_now=now;if (now>=deadline) return finish(s,A733_CE_CIPHER_INVALID);
    enum a733_ce_result r=a733_ce_session_claim(s->engine,s,sizeof(*s),A733_CE_CLIENT_AES128,&s->session_generation);
    if (r==A733_CE_BUSY) return finish(s,A733_CE_CIPHER_BUSY);
    if (r==A733_CE_INVALID) return finish(s,A733_CE_CIPHER_INVALID);
    if (r!=A733_CE_PENDING) return retain(s);
    private_clear(s);
    for (unsigned i=0;i<16;++i) s->key[i]=key[i];
    for (size_t i=0;i<bytes;++i) s->input[i]=input[i];
    s->deadline=deadline;s->bytes=bytes;s->completed=0;s->inflight=0;s->decrypt=decrypt;
    s->active=1;s->phase=REQUEST;s->generation=s->next_generation++;
    if (!time_ok(s)) return retain(s);
    *generation=s->generation;return finish(s,A733_CE_CIPHER_PENDING);
}
enum a733_ce_cipher_result a733_ce_cipher_poll(struct a733_ce_cipher *s,uint64_t generation,uint8_t *output,size_t bytes)
{
    if (!s || !s->initialized) return A733_CE_CIPHER_INVALID;
    if (atomic_exchange_explicit(&s->busy,1,memory_order_acquire)) return A733_CE_CIPHER_BUSY;
    if (s->terminal) return finish(s,A733_CE_CIPHER_RETAINED);
    if (s->fault || !bound(s)) return retain(s);
    if (!generation || generation!=s->generation || bytes!=s->bytes || !outside(s,output,bytes))
        return finish(s,A733_CE_CIPHER_INVALID);
    if (s->phase==DONE) return finish(s,A733_CE_CIPHER_CONSUMED);
    if (!s->active || !a733_ce_session_client(s->engine,s,A733_CE_CLIENT_AES128,s->session_generation) || !time_ok(s))
        return retain(s);
    struct a733_ce *controller=&s->engine->controller;
    enum a733_ce_result r;
    if (s->phase==REQUEST) {
        size_t take=s->bytes-s->completed;if (take>1024) take=1024;
        r=a733_ce_cipher_request(controller,s->key,s->input+s->completed,take,s->decrypt,s->deadline);
        if (controller->fault || !time_ok(s)) return retain(s);
        if (r==A733_CE_BUSY) return finish(s,A733_CE_CIPHER_BUSY);
        if (r!=A733_CE_PENDING) return retain(s);
        s->inflight=take;s->phase=WAIT;return finish(s,A733_CE_CIPHER_PENDING);
    }
    if (s->phase==WAIT) {
        r=a733_ce_cipher_task_poll(controller,s->output+s->completed,s->inflight);
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
        s->completed+=s->inflight;s->inflight=0;
        if (s->completed<s->bytes) {s->phase=REQUEST;return finish(s,A733_CE_CIPHER_PENDING);}
        zero(s->key,sizeof(s->key));zero(s->input,sizeof(s->input));s->phase=PUBLISH;
    }
    if (s->phase!=PUBLISH || !time_ok(s)) return retain(s);
    enum a733_clock_result token=a733_clock_token_status(s->engine->binding.registry,&s->engine->binding.token);
    if (token==A733_CLOCK_BUSY) return finish(s,A733_CE_CIPHER_BUSY);
    if (token!=A733_CLOCK_OK || !a733_ce_session_done(s->engine,s,A733_CE_CLIENT_AES128,s->session_generation)) return retain(s);
    for (size_t i=0;i<bytes;++i) output[i]=s->output[i];zero(s->output,sizeof(s->output));
    s->active=0;s->phase=DONE;return finish(s,A733_CE_CIPHER_OUTPUT);
}
