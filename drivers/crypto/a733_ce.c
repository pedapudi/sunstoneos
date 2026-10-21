// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/* Register/descriptor definitions adapted from the A733 CE v5 source. */
#include "a733_clock_reference.h"
#include "a733_ce.h"
#include <stddef.h>
#define CCU UINT32_C(0x02002000)
#define NS UINT32_C(0x04603000)
#define SEC UINT32_C(0x04603800)
#define BIT(n) (UINT32_C(1)<<(n))
enum { ADMIT,PREPARE,SUBMIT,WAIT,DONE,FAILED };
static const unsigned fabric_offset[]={0x500,0x580,0x588};
static const uint32_t fabric_mask[]={0x0300001f,0x8700001f,0x8700001f};
static const unsigned parent_mhz[3][8]={
    {24,0,0,600,0,0,0,0},{24,0,0,600,0,0,0,0},{24,600,0,0,400,0,0,0}};
static void zero(void *p,size_t n)
{volatile uint8_t *b=p;while (n--) *b++=0;}
static int extent(uint64_t a,uint64_t n)
{return a && !(a&63) && a<(UINT64_C(1)<<40) && n<=(UINT64_C(1)<<40)-a;}
int a733_ce_init(struct a733_ce *p,const struct a733_ce_config *c,const struct a733_ce_ops *o,
    void *cookie,struct a733_clock_leases *r,const struct a733_clock_token *t,struct a733_ce_memory *m)
{
    if (!p || p->initialized || !c || !o || !r || !t || !m || ((uintptr_t)m&63) ||
        !c->owner || !c->handoff || !c->deadline_us || c->configure_health>1 ||
        !a733_ce_capabilities(c->profile) ||
        !t->id || t->owner!=c->owner || !o->now_us || !o->authorize || !o->enter ||
        !o->leave || !o->retain || !o->read || !o->write || !o->sync) return 0;
    const void *objects[]={p,c,o,r,t,m};
    const size_t sizes[]={sizeof(*p),sizeof(*c),sizeof(*o),sizeof(*r),sizeof(*t),sizeof(*m)};
    for (unsigned i=0;i<6;++i) for (unsigned j=i+1;j<6;++j)
        if (!a733_ce_separate(objects[i],sizes[i],objects[j],sizes[j])) return 0;
    a733_clock_mask_t stable=A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC;
    if ((a733_ce_capabilities(c->profile)&A733_CE_CAP_TRNG)) stable|=A733_CLOCK_RC16M;
    if ((t->exclusive&A733_CLOCK_CE)!=A733_CLOCK_CE || ((t->shared|t->exclusive)&stable)!=stable ||
        !extent(c->descriptor_physical,256) || !extent(c->output_physical,64) ||
        !(c->descriptor_physical+256<=c->output_physical || c->output_physical+64<=c->descriptor_physical)) return 0;
    if ((a733_ce_capabilities(c->profile)&(A733_CE_CAP_SHA256|A733_CE_CAP_AES128)) &&
        (!extent(c->descriptor_physical,sizeof(*m)) || c->output_physical!=c->descriptor_physical+256)) return 0;
    if (!(a733_ce_capabilities(c->profile)&A733_CE_CAP_TRNG)) {
        if (c->health_profile || c->configure_health) return 0;
        for (unsigned i=0;i<4;++i) if (c->health[i]) return 0;
    } else {
        uint32_t window=c->health[3]&2047;
        if (!c->health_profile || !c->health[0] || !c->health[1] || c->health[1]>window || c->health[2] ||
            (window!=512 && window!=1024) || c->health[3]!=(UINT32_C(0x30000)|window)) return 0;
    }
    for (unsigned i=0;i<3;++i) {
        uint32_t v=c->fabric[i];unsigned mhz=parent_mhz[i][(v>>24)&7],div=(v&31)+1;
        if ((v&~fabric_mask[i]) || (i && !(v&BIT(31))) || !mhz ||
            (mhz*1000000u)%div || c->fabric_hz[i]!=mhz*1000000u/div) return 0;
    }
    *p=(struct a733_ce){.self=(uintptr_t)p,.config=*c,.ops=*o,.token=*t,.registry=r,.memory=m,.cookie=cookie,.initialized=1,.task_configured=!!(a733_ce_capabilities(c->profile)&A733_CE_CAP_TRNG)};
    atomic_init(&p->busy,0);return 1;
}
static int timely(struct a733_ce *p)
{
    uint64_t now;
    if (p->ops.now_us(p->cookie,&now)!=1 || now<p->last_now || now>=p->config.deadline_us) return 0;
    p->last_now=now;return 1;
}
static int rd(struct a733_ce *p,uintptr_t a,uint32_t *v)
{return timely(p) && p->ops.read(p->cookie,a,v)==1 && timely(p);}
static int wr(struct a733_ce *p,uintptr_t a,uint32_t v)
{return timely(p) && p->ops.write(p->cookie,a,v)==1 && timely(p);}
static int bits(struct a733_ce *p,uintptr_t a,uint32_t mask,uint32_t value)
{uint32_t v;return rd(p,a,&v) && (v&mask)==value;}
static int update(struct a733_ce *p,uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t old;
    return rd(p,a,&old) && wr(p,a,(old&~mask)|(value&mask)) && bits(p,a,mask,value);
}
static int clocks(struct a733_ce *p)
{
    uint32_t v,ref;
    if (!rd(p,UINT32_C(0x07090160),&v) ||
        ((a733_ce_capabilities(p->config.profile)&A733_CE_CAP_TRNG) && !(v&1)) ||
        !rd(p,CCU,&ref) || !a733_clock_ref24_valid(v,ref)) return 0;
    /* CE/PERI0 consume normalized PLL_REF; TRNG still requires RC16M above. */
    if (!rd(p,CCU+0xa0,&v) || !a733_clock_peri0_valid(v) ||
        !bits(p,CCU+0xa8,BIT(31),0) || !bits(p,CCU+0xac,0x8e000000,0)) return 0;
    uint32_t gates=BIT(11)|BIT(2)|BIT(1);
    for (unsigned i=0;i<3;++i) {
        if (parent_mhz[i][(p->config.fabric[i]>>24)&7]==600) gates|=BIT(9);
        if (!bits(p,CCU+fabric_offset[i],fabric_mask[i],p->config.fabric[i])) return 0;
    }
    return bits(p,CCU+0x1908,gates|(gates<<16),gates|(gates<<16)) &&
        bits(p,CCU+0x1988,gates<<16,gates<<16) &&
        bits(p,CCU+0xac0,0x8700001f,0x81000000) &&
        bits(p,CCU+0xac4,0x30003,0x30003) && bits(p,CCU+0x5e4,4,4);
}
static int health(struct a733_ce *p)
{
    for (unsigned i=0;i<4;++i) if (!bits(p,SEC+0x90+4*i,UINT32_MAX,p->config.health[i])) return 0;
    return 1;
}
static int provision(struct a733_ce *p)
{
    if (!p->config.configure_health || p->health_programmed) return health(p);
    uint32_t control;
    if (!rd(p,SEC+0x9c,&control) || (control&0x30000u)) return 0;
    for (unsigned i=0;i<3;++i)
        if (!update(p,SEC+0x90+4*i,UINT32_MAX,p->config.health[i])) return 0;
    /* Preserve reserved fields; install the window before enabling either test. */
    if (!update(p,SEC+0x9c,0x307ff,p->config.health[3]&2047) ||
        !update(p,SEC+0x9c,0x307ff,p->config.health[3]) || !health(p)) return 0;
    p->health_programmed=1;return 1;
}
static int task_health(struct a733_ce *p)
{return p->operation || (health(p) && bits(p,NS+0xd0,3,2));}
static void little(uint8_t *p,uint64_t v,unsigned n)
{for (unsigned i=0;i<n;++i) p[i]=(uint8_t)(v>>(8*i));}
static int prepare(struct a733_ce *p)
{
    uint32_t lpc;
    if (!p->operation && p->config.configure_health && !p->health_programmed &&
        !bits(p,SEC+0x9c,0x30000,0)) return 0;
    if (!clocks(p) || !bits(p,NS+8,15,0) || !bits(p,SEC+8,15,0) ||
        !bits(p,NS+12,255,0) || !bits(p,SEC+12,255,0) ||
        !bits(p,NS+0x18,UINT32_MAX,0) || !bits(p,SEC+0x18,UINT32_MAX,0) ||
        !bits(p,NS+0x10,0x8421,0) || !bits(p,SEC+0x10,0x8421,0)) return 0;
    if (!p->operation) {
        if (!rd(p,NS+0xd0,&lpc) || (lpc&1)) return 0;
        if (!(lpc&2) && (!wr(p,NS+0xd0,lpc|2) || !bits(p,NS+0xd0,UINT32_MAX,lpc|2))) return 0;
        if (!provision(p)) return 0;
    }
    for (unsigned i=0;i<256;++i) p->memory->descriptor[i]=0;
    for (unsigned i=0;i<64;++i) p->memory->output[i]=0;
    if (p->operation==2) {
        zero(p->memory->cipher_output,sizeof(p->memory->cipher_output));
        little(p->memory->descriptor,2,4);
        little(p->memory->descriptor+4,UINT32_C(0x80000000)|(p->cipher_decrypt<<8),4);
        little(p->memory->descriptor+16,p->config.descriptor_physical+3456,5);
        little(p->memory->descriptor+32,p->cipher_bytes,4);
        little(p->memory->descriptor+36,p->config.descriptor_physical+384,5);
        little(p->memory->descriptor+41,p->config.descriptor_physical+2432,5);
        little(p->memory->descriptor+48,p->cipher_bytes,4);
        little(p->memory->descriptor+52,p->cipher_bytes,4);
        return timely(p) && p->ops.sync(p->cookie,p->memory,A733_CE_CIPHER_PUBLISH)==1 && timely(p);
    }
    little(p->memory->descriptor,p->operation ? (0x10002u|(p->hash_continue<<8)):0x11002u,4);
    little(p->memory->descriptor+4,p->operation ? 3:0x203,4);
    if (p->operation) {
        little(p->memory->descriptor+8,(uint64_t)p->hash_bytes*8,5);
        if (p->hash_continue) little(p->memory->descriptor+18,p->config.descriptor_physical+320,5);
        little(p->memory->descriptor+24,p->config.descriptor_physical+384,5);
        little(p->memory->descriptor+36,p->hash_bytes,4);
        little(p->memory->descriptor+200,p->hash_total_bits>>32,4);
        little(p->memory->descriptor+204,p->hash_total_bits,4);
    }
    little(p->memory->descriptor+29,p->config.output_physical,5);
    little(p->memory->descriptor+40,32,4);
    return timely(p) && p->ops.sync(p->cookie,p->memory,A733_CE_PUBLISH)==1 && timely(p);
}
static enum a733_ce_result finish(struct a733_ce *p,enum a733_ce_result r)
{atomic_store_explicit(&p->busy,0,memory_order_release);return r;}
static enum a733_ce_result failed(struct a733_ce *p)
{
    p->fault=1;
    if (p->held) {p->ops.retain(p->cookie);p->held=0;}
    if (a733_clock_retain(p->registry,&p->token)==A733_CLOCK_BUSY) return finish(p,A733_CE_BUSY);
    p->stage=FAILED;return finish(p,A733_CE_RETAINED);
}
static enum a733_ce_result poll(struct a733_ce *p,uint8_t *output,size_t output_bytes)
{
    if (!p || !p->initialized || p->self!=(uintptr_t)p || !p->task_configured || !output ||
        output_bytes!=(p->operation==2?p->cipher_bytes:32) ||
        !a733_ce_separate(p,sizeof(*p),output,output_bytes) ||
        !a733_ce_separate(p->memory,sizeof(*p->memory),output,output_bytes) || !a733_ce_separate(p->registry,sizeof(*p->registry),output,output_bytes)) return A733_CE_INVALID;
    if (atomic_exchange_explicit(&p->busy,1,memory_order_acquire)) return A733_CE_BUSY;
    if (p->stage==FAILED) return finish(p,A733_CE_RETAINED);
    if (p->fault) return failed(p);
    if (p->stage==DONE) return finish(p,A733_CE_CONSUMED);
    if (!timely(p)) return failed(p);
    enum a733_clock_result token=a733_clock_token_status(p->registry,&p->token);
    if (token==A733_CLOCK_BUSY) return finish(p,A733_CE_BUSY);
    if (token!=A733_CLOCK_OK) return failed(p);
    if (p->stage==ADMIT) {
        int ok=p->ops.authorize(p->cookie,&p->config,&p->token,p->memory);
        if (!timely(p) || (ok!=0 && ok!=1)) return failed(p);
        if (ok==0) return finish(p,A733_CE_PENDING);
        p->stage=PREPARE;return finish(p,A733_CE_PENDING);
    }
    int lock=p->ops.enter(p->cookie,p->config.deadline_us);
    if (!lock) {if (!timely(p)) return failed(p);return finish(p,A733_CE_BUSY);}
    p->held=1;
    if (lock!=1 || !timely(p)) return failed(p);
    unsigned next=p->stage;uint8_t bytes[32]={0};unsigned ready=0;
    if (p->stage==PREPARE) {
        if (!prepare(p)) goto fail;
        next=SUBMIT;
    } else if (p->stage==SUBMIT) {
        if (!clocks(p) || !task_health(p) ||
            !bits(p,NS+12,255,0) || !bits(p,SEC+12,255,0) ||
            !bits(p,NS+0x18,UINT32_MAX,0) || !bits(p,SEC+0x18,UINT32_MAX,0) ||
            !wr(p,NS,(uint32_t)p->config.descriptor_physical) ||
            !update(p,NS+4,255,(uint32_t)(p->config.descriptor_physical>>32)) ||
            !update(p,NS+8,4,4) || !bits(p,NS+8,15,4) || !wr(p,NS+0x10,p->operation==2?1:0x400)) goto fail;
        next=WAIT;
    } else {
        if (!rd(p,NS+12,&p->status_ns) || !rd(p,SEC+12,&p->status_secure) ||
            !rd(p,NS+0x18,&p->error_ns) || !rd(p,SEC+0x18,&p->error_secure) ||
            p->error_ns || p->error_secure || (p->status_ns&~0x30u) || p->status_secure) goto fail;
        uint32_t status=(p->status_ns>>4)&3;
        if (status>1) goto fail;
        if (status==1) {
            if (!clocks(p) || !task_health(p) ||
                p->ops.sync(p->cookie,p->memory,p->operation==2?A733_CE_CIPHER_ACQUIRE:A733_CE_ACQUIRE)!=1 || !timely(p)) goto fail;
            if (p->operation!=2) for (unsigned i=0;i<32;++i) bytes[i]=p->memory->output[i];
            if (!wr(p,NS+12,0x30) || !update(p,NS+8,4,0) || !bits(p,NS+8,15,0)) goto fail;
            /* The task completed and its result is copied. Do not leave the
             * random block, digest or (HMAC) hash state in the retained page.
             * TRNG writes only the output; IV and input hold no TRNG data. */
            if (p->operation!=2) {
                zero(p->memory->output,sizeof(p->memory->output));
                if (p->operation==1) {
                    zero(p->memory->iv,sizeof(p->memory->iv));
                    zero(p->memory->input,sizeof(p->memory->input));
                }
                if (p->ops.sync(p->cookie,p->memory,p->operation==1?A733_CE_SCRUB:A733_CE_RANDOM_SCRUB)!=1 ||
                    !timely(p)) goto fail;
            }
            ready=1;next=DONE;
        }
    }
    if (!timely(p)) goto fail;
    p->ops.leave(p->cookie);p->held=0;
    if (!timely(p)) goto fail;
    p->stage=next;
    if (ready) {
        if (p->operation==2) {
            for (size_t i=0;i<output_bytes;++i) output[i]=p->memory->cipher_output[i];
        } else for (unsigned i=0;i<32;++i) output[i]=bytes[i];
        zero(bytes,sizeof(bytes));return finish(p,p->operation==2?A733_CE_CIPHER:(p->operation?A733_CE_DIGEST:A733_CE_RANDOM));
    }
    zero(bytes,sizeof(bytes));return finish(p,A733_CE_PENDING);
fail:
    zero(bytes,sizeof(bytes));return failed(p);
}

static enum a733_ce_result request(struct a733_ce *p,uint64_t deadline,unsigned operation,
    const uint8_t *input,size_t bytes,const uint8_t *iv,unsigned continuation,uint64_t total)
{
    if (!p || !p->initialized || p->self!=(uintptr_t)p || !deadline) return A733_CE_INVALID;
    unsigned capability=operation==0?A733_CE_CAP_TRNG:(operation==1?A733_CE_CAP_SHA256:A733_CE_CAP_AES128);
    if (!(a733_ce_capabilities(p->config.profile)&capability)) return A733_CE_INVALID;
    if (operation && (!input || !bytes || continuation>1 ||
        !a733_ce_separate(input,bytes,p,sizeof(*p)) || !a733_ce_separate(input,bytes,p->memory,sizeof(*p->memory)) ||
        !a733_ce_separate(input,bytes,p->registry,sizeof(*p->registry)))) return A733_CE_INVALID;
    if (operation==1 && (bytes>2048 || bytes%64 || total%512 || total<(uint64_t)bytes*8 ||
        (!continuation && total!=(uint64_t)bytes*8) || (continuation && (total==(uint64_t)bytes*8 || !iv)))) return A733_CE_INVALID;
    if (operation==2 && (bytes>1024 || bytes%16 || !iv || total)) return A733_CE_INVALID;
    size_t auxiliary_bytes=operation==2?16:32;
    if ((operation==2 || (operation==1 && continuation)) &&
        (!a733_ce_separate(iv,auxiliary_bytes,p,sizeof(*p)) || !a733_ce_separate(iv,auxiliary_bytes,p->memory,sizeof(*p->memory)) ||
         !a733_ce_separate(iv,auxiliary_bytes,p->registry,sizeof(*p->registry)))) return A733_CE_INVALID;
    if (atomic_exchange_explicit(&p->busy,1,memory_order_acquire)) return A733_CE_BUSY;
    if (p->stage==FAILED) return finish(p,A733_CE_RETAINED);
    if (p->fault) return failed(p);
    if (p->key_dirty || (p->stage!=DONE && p->stage!=ADMIT)) return finish(p,A733_CE_BUSY);
    uint64_t now;
    if (p->ops.now_us(p->cookie,&now)!=1 || now<p->last_now) return failed(p);
    p->last_now=now;
    if (deadline<=now) return finish(p,A733_CE_INVALID);
    enum a733_clock_result token=a733_clock_token_status(p->registry,&p->token);
    if (token==A733_CLOCK_BUSY) return finish(p,A733_CE_BUSY);
    if (token!=A733_CLOCK_OK) return failed(p);
    p->config.deadline_us=deadline;
    if (p->stage==ADMIT) goto configure;
    int lock=p->ops.enter(p->cookie,deadline);
    if (!lock) {if (!timely(p)) return failed(p);return finish(p,A733_CE_BUSY);}
    p->held=1;
    if (lock!=1 || !timely(p) || !clocks(p) || !task_health(p) ||
        !bits(p,NS+8,15,0) || !bits(p,SEC+8,15,0) ||
        !bits(p,NS+12,255,0) || !bits(p,SEC+12,255,0) ||
        !bits(p,NS+0x18,UINT32_MAX,0) || !bits(p,SEC+0x18,UINT32_MAX,0) ||
        !bits(p,NS+0x10,0x8421,0) || !bits(p,SEC+0x10,0x8421,0)) return failed(p);
    if (!timely(p)) return failed(p);
    p->ops.leave(p->cookie);p->held=0;
    if (!timely(p)) return failed(p);
    p->stage=PREPARE;
configure:
    /* A TRNG request replacing an unadmitted SHA request clears the hash
     * input and IV here, because its own scrub covers only the output. */
    if (!operation && p->operation==1) {
        zero(p->memory->iv,sizeof(p->memory->iv));
        zero(p->memory->input,sizeof(p->memory->input));
    }
    p->operation=operation;p->task_configured=1;
    if (operation) {
        zero(p->memory->input,sizeof(p->memory->input));
        for (size_t i=0;i<bytes;++i) p->memory->input[i]=input[i];
        if (operation==1) {
            zero(p->memory->iv,sizeof(p->memory->iv));
            if (continuation) for (unsigned i=0;i<32;++i) p->memory->iv[i]=iv[i];
            p->hash_bytes=(unsigned)bytes;p->hash_continue=continuation;p->hash_total_bits=total;
        } else {
            zero(p->memory->key,sizeof(p->memory->key));
            for (unsigned i=0;i<16;++i) p->memory->key[i]=iv[i];
            p->cipher_bytes=(unsigned)bytes;p->cipher_decrypt=continuation;p->key_dirty=1;p->key_clean_complete=0;
        }
    }
    if (!timely(p)) return failed(p);
    return finish(p,A733_CE_PENDING);
}

enum a733_ce_result a733_ce_request(struct a733_ce *p,uint64_t deadline)
{return request(p,deadline,0,0,0,0,0,0);}
enum a733_ce_result a733_ce_hash_request(struct a733_ce *p,const uint8_t *input,size_t bytes,
    const uint8_t iv[32],unsigned continuation,uint64_t total,uint64_t deadline)
{return request(p,deadline,1,input,bytes,iv,continuation,total);}
int a733_ce_idle(const struct a733_ce *p)
{return p && p->initialized && p->self==(uintptr_t)p && !p->fault &&
    !p->key_dirty && !atomic_load_explicit(&p->busy,memory_order_acquire) && (p->stage==ADMIT || p->stage==DONE);}

enum a733_ce_result a733_ce_poll(struct a733_ce *p,uint8_t output[32])
{if (!p || p->operation==2) return A733_CE_INVALID;return poll(p,output,32);}
enum a733_ce_result a733_ce_cipher_request(struct a733_ce *p,const uint8_t key[16],
    const uint8_t *input,size_t bytes,unsigned decrypt,uint64_t deadline)
{return request(p,deadline,2,input,bytes,key,decrypt,0);}
enum a733_ce_result a733_ce_cipher_task_poll(struct a733_ce *p,uint8_t *output,size_t bytes)
{if (!p || p->operation!=2) return A733_CE_INVALID;return poll(p,output,bytes);}
enum a733_ce_result a733_ce_cipher_clear_key(struct a733_ce *p)
{
    if (!p || !p->initialized || p->self!=(uintptr_t)p) return A733_CE_INVALID;
    if (atomic_exchange_explicit(&p->busy,1,memory_order_acquire)) return A733_CE_BUSY;
    if (p->stage==FAILED) return finish(p,A733_CE_RETAINED);
    if (p->fault) return failed(p);
    if (p->operation!=2 || p->stage!=DONE) return finish(p,A733_CE_INVALID);
    if (!p->key_dirty) return finish(p,A733_CE_CONSUMED);
    if (!timely(p)) return failed(p);
    enum a733_clock_result token=a733_clock_token_status(p->registry,&p->token);
    if (token==A733_CLOCK_BUSY) return finish(p,A733_CE_BUSY);
    if (token!=A733_CLOCK_OK) return failed(p);
    if (!p->key_clean_complete) {
        int lock=p->ops.enter(p->cookie,p->config.deadline_us);
        if (!lock) {if (!timely(p)) return failed(p);return finish(p,A733_CE_BUSY);}
        p->held=1;
        if (lock!=1 || !timely(p)) return failed(p);
        zero(p->memory->key,sizeof(p->memory->key));
        zero(p->memory->input,sizeof(p->memory->input));
        zero(p->memory->cipher_output,sizeof(p->memory->cipher_output));
        if (p->ops.sync(p->cookie,p->memory,A733_CE_KEY_CLEAN)!=1 || !timely(p)) return failed(p);
        p->ops.leave(p->cookie);p->held=0;
        if (!timely(p)) return failed(p);
        p->key_clean_complete=1;
        token=a733_clock_token_status(p->registry,&p->token);
        if (token==A733_CLOCK_BUSY) return finish(p,A733_CE_BUSY);
        if (token!=A733_CLOCK_OK) return failed(p);
    }
    p->key_dirty=0;return finish(p,A733_CE_KEY_CLEARED);
}
