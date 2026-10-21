#include "a733_ce_entropy.h"
#include "a733_ce_hash.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define NS UINT32_C(0x04603000)
#define SEC UINT32_C(0x04603800)
_Static_assert(offsetof(struct a733_ce_memory,output)==256 &&
    offsetof(struct a733_ce_memory,iv)==320 && offsetof(struct a733_ce_memory,input)==384 &&
    sizeof(struct a733_ce_memory)<=4096,"CE pool layout");
struct reg {uintptr_t a;uint32_t v;};
struct model {
    struct a733_clock_leases registry;
    struct a733_clock_token token;
    struct device_memory_pool pool;
    struct device_memory_lease lease;
    struct a733_ce_entropy service;
    struct a733_ce_session hash_session,*engine;
    struct a733_ce_hash hash;
    uint8_t visible[sizeof(struct a733_ce_memory)],last_hash[32];
    uint64_t hash_processed;
    unsigned hash_starts,rng_starts,health_reads,lpc_reads,fail_next_start;
    struct a733_ce_config config;
    _Alignas(4096) uint8_t pages[8192];
    struct reg regs[40];unsigned count;
    uint64_t now;
    unsigned held,reads,writes,starts,pending,published,acquired,health_writes,authorizations;
    unsigned fail_health_write,finish_late,retain_busy,retained;
};
static uint32_t *reg(struct model *m,uintptr_t a)
{
    for (unsigned i=0;i<m->count;++i) if (m->regs[i].a==a) return &m->regs[i].v;
    assert(m->count<40);m->regs[m->count]=(struct reg){a,0};return &m->regs[m->count++].v;
}
static int now(void *v,uint64_t *n) {struct model *m=v;*n=m->now;return 1;}
static int authorize(void *v,const struct a733_ce_config *c,const struct a733_clock_token *t,const struct a733_ce_memory *mem)
{
    struct model *m=v;assert(c->owner==5 && c->handoff==7);
    assert(c->profile==m->config.profile);
    assert(t->id==m->token.id && mem==(void *)m->pages);
    assert(c->descriptor_physical==0x41000000 && c->output_physical==0x41000100);
    ++m->authorizations;return 1;
}
static int enter(void *v,uint64_t d)
{struct model *m=v;assert(d==m->engine->controller.config.deadline_us && !m->held);m->held=1;return 1;}
static void leave(void *v)
{
    struct model *m=v;assert(m->held);m->held=0;
    if (m->finish_late && m->acquired) {
        m->now=m->engine->controller.config.deadline_us;
        if (m->retain_busy) atomic_store(&m->registry.busy,1);
    }
}
static void retain(void *v) {struct model *m=v;assert(m->held);++m->retained;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{struct model *m=v;assert(m->held);++m->reads;if (a>=SEC+0x90 && a<=SEC+0x9c) ++m->health_reads;
 if (a==NS+0xd0) ++m->lpc_reads;*out=*reg(m,a);return 1;}
static uint64_t le(const uint8_t *p,unsigned n)
{uint64_t v=0;for (unsigned i=0;i<n;++i) v|=(uint64_t)p[i]<<(8*i);return v;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;assert(m->held);++m->writes;
    if (a>=SEC+0x90 && a<=SEC+0x9c) {
        assert(!(*reg(m,SEC+0x9c)&0x30000) && (*reg(m,NS+0xd0)&3)==2);
        const uintptr_t order[]={SEC+0x90,SEC+0x94,SEC+0x98,SEC+0x9c,SEC+0x9c};
        assert(m->health_writes<5 && a==order[m->health_writes]);
        if (m->health_writes==3) assert(value==512);
        if (m->health_writes==4) assert(value==0x30200);
        ++m->health_writes;*reg(m,a)=value;
        return m->fail_health_write==m->health_writes ? -1:1;
    }
    assert(a>=NS && a<SEC);
    if (a==NS+12) {*reg(m,a)&=~value;m->pending=0;}
    else if (a==NS+0x10) {
        assert(value==0x400 && m->published==m->starts+1 && !m->pending);
        assert(*reg(m,NS)==0x41000000 && (*reg(m,NS+4)&255)==0);
        assert((*reg(m,NS+8)&15)==4);
        const uint8_t *d=m->visible;uint32_t control=(uint32_t)le(d,4);
        assert(le(d+29,5)==0x41000100 && le(d+40,4)==32);
        if (le(d+4,4)==0x203) {
            assert(control==0x11002 && !le(d+24,5) && !le(d+36,4));++m->rng_starts;
        } else {
            assert(le(d+4,4)==3 && (control==0x10002 || control==0x10102));
            size_t n=(size_t)le(d+36,4);assert(n && n<=2048 && n%64==0);
            assert(le(d+8,5)==n*8 && le(d+24,5)==0x41000180);
            if (control&256) {assert(le(d+18,5)==0x41000140);assert(!memcmp(m->visible+320,m->last_hash,32));}
            else {assert(!le(d+18,5));m->hash_processed=0;}
            m->hash_processed+=n;
            assert((le(d+200,4)<<32|le(d+204,4))==m->hash_processed*8);
            for (unsigned i=44;i<200;++i) assert(!d[i]);
            for (unsigned i=208;i<256;++i) assert(!d[i]);
            ++m->hash_starts;
        }
        ++m->starts;m->pending=1;
        if (m->fail_next_start) return -1;
    } else *reg(m,a)=value;
    return 1;
}
static int sync_memory(void *v,struct a733_ce_memory *mem,enum a733_ce_sync op)
{
    struct model *m=v;struct device_memory_extent e;enum device_memory_state state;
    assert(m->held && mem==(void *)m->pages);
    assert(device_memory_describe(&m->pool,m->lease,&e,&state) && state==DEVICE_MEMORY_DEVICE);
    if (op==A733_CE_PUBLISH) {assert(!m->pending);memcpy(m->visible,mem,sizeof(*mem));++m->published;}
    else {assert(m->pending && (*reg(m,NS+12)&0x30)==0x10);++m->acquired;}
    return 1;
}
static const struct a733_ce_ops ops={now,authorize,enter,leave,retain,read_reg,write_reg,sync_memory};
static void setup(struct model *m,unsigned hash_only,unsigned cold)
{
    memset(m,0,sizeof(*m));m->now=1;
    assert(a733_clock_leases_init(&m->registry,now,m));
    a733_clock_mask_t shared=A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC;
    if (!hash_only) shared|=A733_CLOCK_RC16M;
    assert(a733_clock_acquire(&m->registry,5,shared,A733_CLOCK_CE,10000,&m->token)==A733_CLOCK_OK);
    assert(device_memory_pool_init(&m->pool,(uintptr_t)m->pages,0x41000000,sizeof(m->pages)));
    struct device_memory_extent e;
    assert(device_memory_acquire(&m->pool,5,4096,&m->lease,&e));
    m->config=(struct a733_ce_config){.owner=5,.handoff=7,.health_profile=8,.deadline_us=10000,
        .fabric={0x03000001,0x83000000,0x84000000},.fabric_hz={300000000,600000000,400000000},
        .health={9,325,0,0x30200},.configure_health=cold,.profile=A733_CE_TRNG_SHA256};
    if (hash_only) {m->config.profile=A733_CE_SHA256_ONLY;m->config.health_profile=0;m->config.configure_health=0;memset(m->config.health,0,sizeof(m->config.health));}
    /* Artificial model values: no production threshold or entropy assertion. */
    *reg(m,0x07090160)=0x10000002|!hash_only;*reg(m,CCU)=0xf8000000|79u<<8|79u<<16;
    *reg(m,CCU+0xa0)=0xfe000000|99u<<8|1u<<20;
    *reg(m,CCU+0x1908)=0x0a060a06;*reg(m,CCU+0x1988)=0x0a060000;
    *reg(m,CCU+0xac0)=0x81000000;*reg(m,CCU+0xac4)=0x30003;*reg(m,CCU+0x5e4)=4;
    for (unsigned i=0;i<3;++i) *reg(m,CCU+(unsigned[]){0x500,0x580,0x588}[i])=m->config.fabric[i];
    for (unsigned i=0;i<4;++i) *reg(m,SEC+0x90+4*i)=cold?0:m->config.health[i];
    *reg(m,NS+4)=0xa5000099;
    if (hash_only) {
        assert(a733_ce_session_bind(&m->hash_session,&m->pool,m->lease,&m->config,&ops,m,&m->registry,&m->token));
        m->engine=&m->hash_session;
    } else {
        assert(a733_ce_entropy_bind(&m->service,&m->pool,m->lease,&m->config,&ops,m,&m->registry,&m->token));
        m->engine=m->service.engine;
    }
    assert(a733_ce_hash_bind(&m->hash,m->engine));
    assert(!device_memory_release(&m->pool,m->lease));
}
/* Independent emulated SHA-256 compression engine. Runtime code performs no
 * SHA rounds; final digest bytes are also compared with Python hashlib. */
static uint32_t rotate(uint32_t x,unsigned n) {return (x>>n)|(x<<(32-n));}
static uint32_t big(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void compress(uint32_t state[8],const uint8_t *block)
{
    static const uint32_t k[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];for (unsigned i=0;i<16;++i) w[i]=big(block+4*i);
    for (unsigned i=16;i<64;++i) {
        uint32_t x=w[i-15],y=w[i-2];
        w[i]=w[i-16]+(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-7]+(rotate(y,17)^rotate(y,19)^(y>>10));
    }
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
    for (unsigned i=0;i<64;++i) {
        uint32_t t=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}
static void complete(struct model *m)
{
    assert(m->pending);*reg(m,NS+12)=0x10;
    if (le(m->visible+4,4)==0x203) {
        for (unsigned i=0;i<32;++i) m->pages[256+i]=(uint8_t)(m->rng_starts*32+i);
    } else {
        uint32_t state[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        if (le(m->visible,4)&256) for (unsigned i=0;i<8;++i) state[i]=big(m->visible+320+4*i);
        size_t n=(size_t)le(m->visible+36,4);
        for (size_t at=0;at<n;at+=64) compress(state,m->visible+384+at);
        for (unsigned i=0;i<32;++i) m->pages[256+i]=(uint8_t)(state[i/4]>>(24-8*(i%4)));
        memcpy(m->last_hash,m->pages+256,32);
    }
}
static void unchanged(const uint8_t *p,size_t n) {for (size_t i=0;i<n;++i) assert(p[i]==0xa5);}
static void zeroed(const uint8_t *p,size_t n) {for (size_t i=0;i<n;++i) assert(!p[i]);}
static enum a733_ce_hash_result hash_poll(struct model *m,uint64_t generation,uint8_t out[32])
{
    enum a733_ce_hash_result r=A733_CE_HASH_PENDING;
    for (unsigned i=0;i<30 && r==A733_CE_HASH_PENDING;++i) {
        if (m->pending) complete(m);
        r=a733_ce_hash_poll(&m->hash,generation,out);
        if (r!=A733_CE_HASH_DIGEST) unchanged(out,32);
    }
    return r;
}
static uint64_t hash_begin(struct model *m)
{
    uint64_t g=0;assert(a733_ce_hash_begin(&m->hash,10000,&g)==A733_CE_HASH_READY);assert(g);return g;
}
static void message(struct model *m,size_t length,unsigned print_digest)
{
    uint8_t input[2048],out[32];memset(out,0xa5,sizeof(out));uint64_t g=hash_begin(m),other=0;
    if (m->service.initialized) assert(a733_ce_entropy_request(&m->service,1,10000,&other)==A733_CE_ENTROPY_BUSY);
    for (size_t at=0;at<length;) {
        size_t n=(at%3==0)?11:2048;if (n>length-at) n=length-at;
        for (size_t i=0;i<n;++i) input[i]=(uint8_t)((at+i)*37+11);
        size_t accepted=0;
        enum a733_ce_hash_result r=a733_ce_hash_update(&m->hash,g,input,n,&accepted);
        assert((r==A733_CE_HASH_READY || r==A733_CE_HASH_PENDING) && accepted && accepted<=n);
        at+=accepted;memset(input,0xcc,sizeof(input)); /* Caller storage may change immediately. */
        if (r==A733_CE_HASH_PENDING) assert(hash_poll(m,g,out)==A733_CE_HASH_READY);
    }
    assert(a733_ce_hash_final(&m->hash,g)==A733_CE_HASH_PENDING);
    assert(hash_poll(m,g,out)==A733_CE_HASH_DIGEST);
    if (print_digest) {printf("digest %zu ",length);for (unsigned i=0;i<32;++i) printf("%02x",out[i]);puts("");}
    memset(out,0xa5,sizeof(out));assert(a733_ce_hash_poll(&m->hash,g,out)==A733_CE_HASH_CONSUMED);unchanged(out,32);
    assert(!m->engine->client);zeroed(m->hash.buffer,sizeof(m->hash.buffer));zeroed(m->hash.digest,32);
}
static void entropy(struct model *m,size_t n)
{
    uint8_t out[256];memset(out,0xa5,sizeof(out));unsigned first=m->rng_starts+1;uint64_t g=0,h=0;
    assert(a733_ce_entropy_request(&m->service,n,10000,&g)==A733_CE_ENTROPY_PENDING);
    assert(a733_ce_hash_begin(&m->hash,10000,&h)==A733_CE_HASH_BUSY);
    enum a733_ce_entropy_result r=A733_CE_ENTROPY_PENDING;
    for (unsigned i=0;i<40 && r==A733_CE_ENTROPY_PENDING;++i) {
        if (m->pending) complete(m);
        r=a733_ce_entropy_poll(&m->service,g,out,n);
        if (r!=A733_CE_ENTROPY_BYTES) unchanged(out,n);
    }
    assert(r==A733_CE_ENTROPY_BYTES);
    for (size_t i=0;i<n;++i) assert(out[i]==(uint8_t)(first*32+i));
    memset(out,0xa5,sizeof(out));assert(a733_ce_entropy_poll(&m->service,g,out,n)==A733_CE_ENTROPY_CONSUMED);unchanged(out,n);
    assert(!m->engine->client);zeroed(m->service.staging,sizeof(m->service.staging));
}
static void until_start(struct model *m,uint64_t g,uint8_t out[32])
{
    unsigned starts=m->starts;
    for (unsigned i=0;i<10 && m->starts==starts;++i)
        assert(a733_ce_hash_poll(&m->hash,g,out)==A733_CE_HASH_PENDING);
    assert(m->starts==starts+1 && m->pending);unchanged(out,32);
}
static void retained(struct model *m,uint64_t g,uint8_t out[32])
{
    unsigned reads=m->reads,writes=m->writes;
    assert(a733_ce_hash_poll(&m->hash,g,out)==A733_CE_HASH_RETAINED);
    assert(m->reads==reads && m->writes==writes);unchanged(out,32);
    zeroed(m->hash.buffer,sizeof(m->hash.buffer));zeroed(m->hash.digest,sizeof(m->hash.digest));
    struct device_memory_extent e;enum device_memory_state state;
    assert(device_memory_describe(&m->pool,m->lease,&e,&state) && state==DEVICE_MEMORY_RETAINED);
    assert(!device_memory_release(&m->pool,m->lease));
}
int main(void)
{
    struct model m;uint8_t out[32],input[2048];memset(out,0xa5,32);memset(input,0x7c,sizeof(input));
    const size_t lengths[]={0,3,55,56,63,64,65,2040,2048,5003};
    setup(&m,1,0);
    for (unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i) message(&m,lengths[i],1);
    assert(!m.health_reads && !m.health_writes && !m.lpc_reads && !m.rng_starts && m.authorizations==1);
    setup(&m,0,1);entropy(&m,65);message(&m,5003,0);entropy(&m,33);
    assert(m.health_writes==5 && m.authorizations==1);
    setup(&m,0,1);message(&m,56,0);assert(!m.health_reads && !m.health_writes && !m.lpc_reads);
    entropy(&m,33);assert(m.health_writes==5 && m.authorizations==1);
    /* Failed SHA work after a successful intermediate block poisons all clients. */
    for (unsigned kind=0;kind<3;++kind) {
        setup(&m,0,0);uint64_t g=hash_begin(&m);size_t accepted=0;
        assert(a733_ce_hash_update(&m.hash,g,input,sizeof(input),&accepted)==A733_CE_HASH_PENDING);
        assert(hash_poll(&m,g,out)==A733_CE_HASH_READY);
        assert(a733_ce_hash_final(&m.hash,g)==A733_CE_HASH_PENDING);until_start(&m,g,out);
        if (kind==0) {complete(&m);*reg(&m,SEC+0x18)=1;}
        if (kind==1) {complete(&m);m.finish_late=1;m.retain_busy=1;}
        if (kind==2) {m.now=10000;atomic_store(&m.registry.busy,1);}
        enum a733_ce_hash_result r=a733_ce_hash_poll(&m.hash,g,out);
        assert(r==(kind ? A733_CE_HASH_BUSY:A733_CE_HASH_RETAINED));unchanged(out,32);
        if (kind) {
            unsigned reads=m.reads,writes=m.writes;
            assert(a733_ce_hash_poll(&m.hash,g,out)==A733_CE_HASH_BUSY);
            assert(reads==m.reads && writes==m.writes);atomic_store(&m.registry.busy,0);
        }
        retained(&m,g,out);uint64_t e=0;
        assert(a733_ce_entropy_request(&m.service,32,20000,&e)==A733_CE_ENTROPY_RETAINED);
    }
    /* Uncertain launch keeps its reservation and produces no digest. */
    setup(&m,1,0);uint64_t g=hash_begin(&m);assert(a733_ce_hash_final(&m.hash,g)==A733_CE_HASH_PENDING);
    m.fail_next_start=1;assert(hash_poll(&m,g,out)==A733_CE_HASH_RETAINED);retained(&m,g,out);
    /* Stale handles, aliases and length overflow refuse before consumption. */
    setup(&m,1,0);g=hash_begin(&m);size_t accepted=99;
    assert(a733_ce_hash_update(&m.hash,g+1,input,1,&accepted)==A733_CE_HASH_INVALID);
    assert(a733_ce_hash_update(&m.hash,g,m.pages,1,&accepted)==A733_CE_HASH_INVALID);
    assert(a733_ce_hash_update(&m.hash,g,input,1,(size_t *)&m.hash)==A733_CE_HASH_INVALID);
    m.hash.message_bytes=A733_CE_HASH_MAX_BYTES;
    assert(a733_ce_hash_update(&m.hash,g,input,1,&accepted)==A733_CE_HASH_INVALID);assert(accepted==99 && !m.starts);
    assert(a733_ce_hash_poll(&m.hash,g,m.pages)==A733_CE_HASH_INVALID);
    struct a733_ce_hash copy=m.hash;
    assert(a733_ce_hash_poll(&copy,g,out)==A733_CE_HASH_RETAINED);assert(!m.starts);
    setup(&m,1,0);struct a733_ce_session copied=*m.engine;struct a733_ce_hash other={0};
    assert(!a733_ce_hash_bind(&other,&copied));
    /* Actual native reservation primitive: constructor rollback only before bind. */
    struct a733_ce_session first={0},second={0};
    assert(a733_ce_session_native_claim(&first));assert(!a733_ce_session_native_claim(&second));
    a733_ce_session_native_abort(&second);assert(!a733_ce_session_native_claim(&second));
    a733_ce_session_native_abort(&first);assert(a733_ce_session_native_claim(&second));
    second.initialized=1;a733_ce_session_native_abort(&second);
    assert(!a733_ce_session_native_claim(&first) && !a733_ce_session_native_claim(&second));
    puts("PASS shared real CE session, streamed descriptors/software padding, entropy exclusion/preservation and retained failures; no physical qualification");
}
