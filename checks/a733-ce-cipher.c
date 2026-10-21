#include "a733_ce_entropy.h"
#include "a733_ce_hash.h"
#include "a733_ce_cipher.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define NS UINT32_C(0x04603000)
#define SEC UINT32_C(0x04603800)
_Static_assert(offsetof(struct a733_ce_memory,output)==256 &&
    offsetof(struct a733_ce_memory,iv)==320 && offsetof(struct a733_ce_memory,input)==384 &&
    offsetof(struct a733_ce_memory,cipher_output)==2432 &&
    offsetof(struct a733_ce_memory,key)==3456 && sizeof(struct a733_ce_memory)==3520,"CE pool layout");
struct reg {uintptr_t a;uint32_t v;};
struct model {
    struct a733_clock_leases registry;
    struct a733_clock_token token;
    struct device_memory_pool pool;
    struct device_memory_lease lease;
    struct a733_ce_entropy service;
    struct a733_ce_session hash_session,*engine;
    struct a733_ce_hash hash;
    struct a733_ce_cipher cipher;
    uint8_t visible[sizeof(struct a733_ce_memory)],last_hash[32];
    uint64_t hash_processed;
    unsigned hash_starts,rng_starts,health_reads,lpc_reads,fail_next_start;
    struct a733_ce_config config;
    _Alignas(4096) uint8_t pages[8192];
    struct reg regs[40];unsigned count;
    uint64_t now;
    unsigned scrubs,held,reads,writes,starts,pending,published,acquired,health_writes,authorizations;
    unsigned fail_health_write,finish_late,retain_busy,retained;
    unsigned cipher_starts,key_cleans,fail_sync,fail_cipher_number,clean_late,clean_fault,clean_busy,publish_fault,publish_busy;
};
static uint32_t *reg(struct model *m,uintptr_t a)
{
    for (unsigned i=0;i<m->count;++i) if (m->regs[i].a==a) return &m->regs[i].v;
    assert(m->count<40);m->regs[m->count]=(struct reg){a,0};return &m->regs[m->count++].v;
}
static int now(void *v,uint64_t *n) {
    struct model *m=v;*n=m->now;
    if (m->engine && m->key_cleans && !m->engine->controller.key_dirty) {
        if (m->publish_fault) m->registry.failed=1;
        if (m->publish_busy) atomic_store(&m->registry.busy,1);
    }
    return 1;
}
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
    if (m->clean_fault && m->key_cleans>=m->clean_fault) m->registry.failed=1;
    if (m->clean_busy && m->key_cleans>=m->clean_busy) atomic_store(&m->registry.busy,1);
    if ((m->finish_late && m->acquired) || (m->clean_late && m->key_cleans>=m->clean_late)) {
        m->now=m->engine->controller.config.deadline_us;
        if (m->retain_busy) atomic_store(&m->registry.busy,1);
    }
}
static void retain(void *v) {struct model *m=v;assert(m->held);++m->retained;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{struct model *m=v;assert(m->held);++m->reads;
 if (!(a733_ce_capabilities(m->config.profile)&A733_CE_CAP_TRNG))
     assert(a!=NS+0xd0 && !(a>=SEC+0x90 && a<=SEC+0x9c));
 if (a>=SEC+0x90 && a<=SEC+0x9c) ++m->health_reads;
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
        assert((value==0x400 || value==1) && m->published==m->starts+1 && !m->pending);
        assert(*reg(m,NS)==0x41000000 && (*reg(m,NS+4)&255)==0);
        assert((*reg(m,NS+8)&15)==4);
        const uint8_t *d=m->visible;uint32_t control=(uint32_t)le(d,4);
        if (value==1) {
            assert(control==2 && (le(d+4,4)==0x80000000 || le(d+4,4)==0x80000100));
            assert(!le(d+8,4) && !le(d+12,4) && le(d+16,5)==0x41000d80);
            for (unsigned i=21;i<32;++i) assert(!d[i]);
            size_t n=(size_t)le(d+32,4);assert(n && n<=1024 && n%16==0);
            assert(le(d+36,5)==0x41000180 && le(d+41,5)==0x41000980);
            assert(!d[46] && !d[47] && le(d+48,4)==n && le(d+52,4)==n);
            for (unsigned i=56;i<256;++i) assert(!d[i]);
            for (unsigned i=16;i<64;++i) assert(!m->visible[3456+i]);
            assert(m->engine->controller.key_dirty);
            assert(m->cipher_starts==m->key_cleans);++m->cipher_starts;
        } else if (le(d+4,4)==0x203) {
            assert(le(d+29,5)==0x41000100 && le(d+40,4)==32);
            assert(control==0x11002 && !le(d+24,5) && !le(d+36,4));++m->rng_starts;
        } else {
            assert(le(d+29,5)==0x41000100 && le(d+40,4)==32);
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
        if (m->fail_next_start || (m->fail_cipher_number && m->cipher_starts==m->fail_cipher_number)) return -1;
    } else *reg(m,a)=value;
    return 1;
}
static int sync_memory(void *v,struct a733_ce_memory *mem,enum a733_ce_sync op)
{
    struct model *m=v;struct device_memory_extent e;enum device_memory_state state;
    assert(m->held && mem==(void *)m->pages);
    assert(device_memory_describe(&m->pool,m->lease,&e,&state) && state==DEVICE_MEMORY_DEVICE);
    if (op==A733_CE_PUBLISH || op==A733_CE_CIPHER_PUBLISH) {
        assert(!m->pending);memcpy(m->visible,mem,sizeof(*mem));++m->published;
    } else if (op==A733_CE_KEY_CLEAN) {
        assert(!m->pending && m->engine->controller.key_dirty);
        assert(!(*reg(m,NS+12)&255) && !(*reg(m,NS+8)&15));
        /* Key, plaintext/ciphertext input and cipher output lines are cleaned
         * together; the model's DRAM view receives the zeroed lines. */
        for (size_t i=384;i<3520;++i) assert(!((uint8_t *)mem)[i]);
        memcpy(m->visible+384,(uint8_t *)mem+384,3520-384);++m->key_cleans;
    } else if (op==A733_CE_SCRUB) {
        assert(!(*reg(m,NS+12)&0x30) && !(*reg(m,NS+8)&4));
        for (size_t i=256;i<2432;++i) assert(!((uint8_t *)mem)[i]);
        memcpy(m->visible+256,(uint8_t *)mem+256,2432-256);++m->scrubs;
    } else {
        assert(op==A733_CE_ACQUIRE || op==A733_CE_CIPHER_ACQUIRE);
        assert(m->pending && (*reg(m,NS+12)&0x30)==0x10);++m->acquired;
    }
    if (m->fail_sync==(unsigned)op+1) return -1;
    return 1;
}
static const struct a733_ce_ops ops={now,authorize,enter,leave,retain,read_reg,write_reg,sync_memory};
static void setup(struct model *m,enum a733_ce_profile profile,unsigned cold)
{
    unsigned hash_only=!(a733_ce_capabilities(profile)&A733_CE_CAP_TRNG);
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
        .health={9,325,0,0x30200},.configure_health=cold,.profile=profile};
    if (hash_only) {m->config.health_profile=0;m->config.configure_health=0;memset(m->config.health,0,sizeof(m->config.health));}
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
    if (a733_ce_capabilities(profile)&A733_CE_CAP_SHA256) assert(a733_ce_hash_bind(&m->hash,m->engine));
    assert(a733_ce_cipher_bind(&m->cipher,m->engine));
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
static void aes(const uint8_t *,const uint8_t *,uint8_t *,size_t,unsigned);
static void complete(struct model *m)
{
    assert(m->pending);if (*reg(m,NS+12)) return;*reg(m,NS+12)=0x10;
    if (le(m->visible,4)==2) {
        aes(m->visible+3456,m->visible+384,m->pages+2432,(size_t)le(m->visible+32,4),!!(le(m->visible+4,4)&256));
    } else if (le(m->visible+4,4)==0x203) {
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
    assert(a733_ce_cipher_begin(&m->cipher,(const uint8_t[16]){0},input,16,0,10000,&other)==A733_CE_CIPHER_BUSY);
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
    assert(a733_ce_cipher_begin(&m->cipher,(const uint8_t[16]){0},out,16,0,10000,&h)==A733_CE_CIPHER_BUSY);
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
/* Model-only AES rounds. Runtime executes no software cipher. Published NIST
 * vectors and an independent OpenSSL command verify the resulting bytes. */
static uint8_t multiply(uint8_t a,uint8_t b)
{uint8_t p=0;while (b) {if (b&1) p^=a;a=(uint8_t)((a<<1)^((a&128)?0x1b:0));b>>=1;}return p;}
static uint8_t rot8(uint8_t v,unsigned n) {return (uint8_t)((v<<n)|(v>>(8-n)));}
static void aes(const uint8_t *key,const uint8_t *input,uint8_t *output,size_t bytes,unsigned decrypt)
{
    uint8_t box[256],inverse[256],expanded[176];
    for (unsigned i=0;i<256;++i) {
        uint8_t x=1;for (unsigned n=0;n<254;++n) x=multiply(x,(uint8_t)i);
        box[i]=x^rot8(x,1)^rot8(x,2)^rot8(x,3)^rot8(x,4)^0x63;inverse[box[i]]=(uint8_t)i;
    }
    memcpy(expanded,key,16);uint8_t rc=1;
    for (unsigned at=16;at<176;at+=4) {
        uint8_t t[4];memcpy(t,expanded+at-4,4);
        if (!(at%16)) {uint8_t first=t[0];for (unsigned i=0;i<3;++i) t[i]=box[t[i+1]];t[3]=box[first];t[0]^=rc;rc=multiply(rc,2);}
        for (unsigned i=0;i<4;++i) expanded[at+i]=expanded[at+i-16]^t[i];
    }
    for (size_t at=0;at<bytes;at+=16) {
        uint8_t s[16],t[16];
        for (unsigned i=0;i<16;++i) s[i]=input[at+i]^expanded[(decrypt?160:0)+i];
        for (unsigned round=1;round<=10;++round) {
            for (unsigned col=0;col<4;++col) for (unsigned row=0;row<4;++row) {
                unsigned from=(col+(decrypt?4-row:row))%4;
                t[col*4+row]=(decrypt?inverse:box)[s[from*4+row]];
            }
            unsigned key_at=decrypt?(10-round)*16:round*16;
            if (decrypt) for (unsigned i=0;i<16;++i) t[i]^=expanded[key_at+i];
            if (round<10) {
                for (unsigned col=0;col<4;++col) for (unsigned row=0;row<4;++row) {
                    const uint8_t forward[4]={2,3,1,1},backward[4]={14,11,13,9};
                    const uint8_t *matrix=decrypt?backward:forward;uint8_t v=0;
                    for (unsigned j=0;j<4;++j) v^=multiply(t[col*4+j],matrix[(j+4-row)%4]);
                    s[col*4+row]=v;
                }
            } else memcpy(s,t,16);
            if (!decrypt) for (unsigned i=0;i<16;++i) s[i]^=expanded[key_at+i];
        }
        memcpy(output+at,s,16);
    }
}
static const uint8_t vector_key[16]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
static const uint8_t vector_plain[64]={
    0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
    0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
    0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
    0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10};
static const uint8_t vector_cipher[64]={
    0x3a,0xd7,0x7b,0xb4,0x0d,0x7a,0x36,0x60,0xa8,0x9e,0xca,0xf3,0x24,0x66,0xef,0x97,
    0xf5,0xd3,0xd5,0x85,0x03,0xb9,0x69,0x9d,0xe7,0x85,0x89,0x5a,0x96,0xfd,0xba,0xaf,
    0x43,0xb1,0xcd,0x7f,0x59,0x8e,0xce,0x23,0x88,0x1b,0x00,0xe3,0xed,0x03,0x06,0x88,
    0x7b,0x0c,0x78,0x5e,0x27,0xe8,0xad,0x3f,0x82,0x23,0x20,0x71,0x04,0x72,0x5d,0xd4};
static uint64_t cipher_begin(struct model *m,size_t n,unsigned decrypt)
{
    uint8_t key[16],input[2048];memcpy(key,vector_key,16);
    for (size_t i=0;i<n;++i) input[i]=(decrypt?vector_cipher:vector_plain)[i%64];
    uint64_t g=0;assert(a733_ce_cipher_begin(&m->cipher,key,input,n,decrypt,10000,&g)==A733_CE_CIPHER_PENDING);
    memset(key,0xcc,16);memset(input,0xcc,sizeof(input));assert(g);return g;
}
static enum a733_ce_cipher_result cipher_poll(struct model *m,uint64_t g,uint8_t *out,size_t n)
{
    enum a733_ce_cipher_result r=A733_CE_CIPHER_PENDING;
    for (unsigned i=0;i<35 && r==A733_CE_CIPHER_PENDING;++i) {
        if (m->pending) complete(m);
        r=a733_ce_cipher_poll(&m->cipher,g,out,n);
        if (r!=A733_CE_CIPHER_OUTPUT) unchanged(out,n);
    }
    return r;
}
static void transform(struct model *m,size_t n,unsigned decrypt,unsigned print_result)
{
    uint8_t out[2048];memset(out,0xa5,sizeof(out));unsigned first=m->cipher_starts;uint64_t g=cipher_begin(m,n,decrypt),other=0;
    if (m->hash.initialized) assert(a733_ce_hash_begin(&m->hash,10000,&other)==A733_CE_HASH_BUSY);
    if (m->service.initialized) assert(a733_ce_entropy_request(&m->service,32,10000,&other)==A733_CE_ENTROPY_BUSY);
    assert(cipher_poll(m,g,out,n)==A733_CE_CIPHER_OUTPUT);
    for (size_t i=0;i<n;++i) assert(out[i]==(decrypt?vector_plain:vector_cipher)[i%64]);
    assert(m->cipher_starts-first==(n+1023)/1024 && m->key_cleans==m->cipher_starts);
    zeroed(m->pages+384,3520-384);zeroed(m->visible+384,3520-384);
    zeroed(m->cipher.key,16);zeroed(m->cipher.input,sizeof(m->cipher.input));zeroed(m->cipher.output,sizeof(m->cipher.output));
    assert(!m->engine->client && a733_ce_idle(&m->engine->controller));
    if (print_result) {printf("cipher %u %zu ",decrypt,n);for (size_t i=0;i<n;++i) printf("%02x",out[i]);puts("");}
    memset(out,0xa5,sizeof(out));assert(a733_ce_cipher_poll(&m->cipher,g,out,n)==A733_CE_CIPHER_CONSUMED);unchanged(out,n);
}
static void until_cipher(struct model *m,uint64_t g,uint8_t *out,size_t n,unsigned wanted)
{
    for (unsigned i=0;i<30 && m->cipher_starts<wanted;++i) {
        if (m->pending) complete(m);
        assert(a733_ce_cipher_poll(&m->cipher,g,out,n)==A733_CE_CIPHER_PENDING);unchanged(out,n);
    }
    assert(m->cipher_starts==wanted && m->pending);
}
static void retained_cipher(struct model *m,uint64_t g,uint8_t *out,size_t n)
{
    unsigned reads=m->reads,writes=m->writes,cleans=m->key_cleans;
    assert(a733_ce_cipher_poll(&m->cipher,g,out,n)==A733_CE_CIPHER_RETAINED);
    assert(reads==m->reads && writes==m->writes && cleans==m->key_cleans);unchanged(out,n);
    zeroed(m->cipher.key,16);zeroed(m->cipher.input,sizeof(m->cipher.input));zeroed(m->cipher.output,sizeof(m->cipher.output));
    struct device_memory_extent e;enum device_memory_state state;
    assert(device_memory_describe(&m->pool,m->lease,&e,&state) && state==DEVICE_MEMORY_RETAINED);
    assert(!device_memory_release(&m->pool,m->lease) && m->engine->client==&m->cipher);
}
int main(void)
{
    struct model m;uint8_t out[2048],input[2048];memset(out,0xa5,sizeof(out));memset(input,0x3c,sizeof(input));
    setup(&m,A733_CE_AES128_ONLY,0);
    const size_t lengths[]={16,64,1024,1040,2048};
    for (unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i) transform(&m,lengths[i],0,1);
    transform(&m,64,1,1);transform(&m,2048,1,1);
    assert(!m.health_reads && !m.health_writes && !m.lpc_reads && m.authorizations==1);
    assert(!a733_ce_entropy_attach(&m.service,m.engine));assert(!a733_ce_hash_bind(&m.hash,m.engine));
    /* Deferred health provisioning and all three clients use the identical page/token. */
    setup(&m,A733_CE_TRNG_SHA256_AES128,1);struct a733_ce_session *engine=m.engine;
    uint64_t token=m.token.id;transform(&m,16,0,0);message(&m,0,1);
    assert(!m.health_reads && !m.health_writes && !m.lpc_reads);
    entropy(&m,33);transform(&m,64,1,0);
    assert(m.engine==engine && m.token.id==token && m.health_writes==5 && m.authorizations==1);
    /* Symmetric task errors, uncertain acceptance and cache/cleanup failures never publish. */
    for (unsigned kind=0;kind<8;++kind) {
        setup(&m,A733_CE_TRNG_SHA256_AES128,0);uint64_t g=cipher_begin(&m,1040,0);
        if (kind==1) m.fail_cipher_number=2;
        if (kind==1) assert(cipher_poll(&m,g,out,1040)==A733_CE_CIPHER_RETAINED);
        else {
            until_cipher(&m,g,out,1040,2);complete(&m);
            if (kind==0) *reg(&m,NS+12)=0x20;
            if (kind==2) m.fail_sync=A733_CE_CIPHER_ACQUIRE+1;
            if (kind==3) m.fail_sync=A733_CE_KEY_CLEAN+1;
            if (kind==4) {m.clean_late=2;m.retain_busy=1;}
            if (kind==5) *reg(&m,SEC+0x18)=1;
            if (kind==6) m.clean_fault=2;
            if (kind==7) m.publish_fault=1;
            enum a733_ce_cipher_result r=cipher_poll(&m,g,out,1040);
            assert(r==(kind==4?A733_CE_CIPHER_BUSY:A733_CE_CIPHER_RETAINED));
            if (kind==4) {
                unsigned reads=m.reads,writes=m.writes;
                assert(a733_ce_cipher_poll(&m.cipher,g,out,1040)==A733_CE_CIPHER_BUSY);
                assert(reads==m.reads && writes==m.writes);atomic_store(&m.registry.busy,0);
            }
        }
        retained_cipher(&m,g,out,1040);
        if (kind!=3 && kind!=4 && kind!=6 && kind!=7) assert(!memcmp(m.pages+3456,vector_key,16));
        if (kind!=7) assert(m.engine->controller.key_dirty);
        uint64_t other=0;assert(a733_ce_hash_begin(&m.hash,20000,&other)==A733_CE_HASH_RETAINED);
        assert(a733_ce_entropy_request(&m.service,32,20000,&other)==A733_CE_ENTROPY_RETAINED);
    }
    /* Completed cleanup and staged output survive metadata BUSY without replay. */
    for (unsigned publication=0;publication<2;++publication) {
        setup(&m,A733_CE_AES128_ONLY,0);uint64_t g=cipher_begin(&m,16,0);
        if (publication) m.publish_busy=1;else m.clean_busy=1;
        assert(cipher_poll(&m,g,out,16)==A733_CE_CIPHER_BUSY);
        assert(m.key_cleans==1 && m.cipher_starts==1 && !m.engine->fault);unchanged(out,16);
        unsigned reads=m.reads,writes=m.writes;
        assert(a733_ce_cipher_poll(&m.cipher,g,out,16)==A733_CE_CIPHER_BUSY);
        assert(m.reads==reads && m.writes==writes && m.key_cleans==1);
        m.publish_busy=0;m.clean_busy=0;atomic_store(&m.registry.busy,0);
        assert(a733_ce_cipher_poll(&m.cipher,g,out,16)==A733_CE_CIPHER_OUTPUT);
        assert(!memcmp(out,vector_cipher,16) && m.key_cleans==1 && m.cipher_starts==1);
        memset(out,0xa5,sizeof(out));
    }
    /* Bounds and alias refusals precede task admission. */
    setup(&m,A733_CE_AES128_ONLY,0);uint64_t g=99;
    assert(a733_ce_cipher_begin(&m.cipher,vector_key,input,0,0,10000,&g)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_begin(&m.cipher,vector_key,input,17,0,10000,&g)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_begin(&m.cipher,vector_key,input,2064,0,10000,&g)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_begin(&m.cipher,vector_key,input,16,2,10000,&g)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_begin(&m.cipher,m.pages,input,16,0,10000,&g)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_begin(&m.cipher,vector_key,input,16,0,10000,(uint64_t *)input)==A733_CE_CIPHER_INVALID);
    assert(g==99 && !m.starts && !m.engine->client);
    g=cipher_begin(&m,16,0);
    assert(a733_ce_cipher_poll(&m.cipher,g+1,out,16)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_poll(&m.cipher,g,m.pages,16)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_poll(&m.cipher,g,out,32)==A733_CE_CIPHER_INVALID);
    assert(!m.starts);until_cipher(&m,g,out,16,1);complete(&m);
    assert(a733_ce_cipher_poll(&m.cipher,g,out,16)==A733_CE_CIPHER_PENDING);
    assert(!a733_ce_idle(&m.engine->controller));
    assert(a733_ce_request(&m.engine->controller,10000)==A733_CE_INVALID);
    assert(a733_ce_cipher_request(&m.engine->controller,vector_key,input,16,0,10000)==A733_CE_BUSY);
    assert(!a733_ce_session_done(m.engine,&m.cipher,A733_CE_CLIENT_AES128,m.cipher.session_generation));
    assert(cipher_poll(&m,g,out,16)==A733_CE_CIPHER_OUTPUT);
    puts("PASS AES known answers, task boundaries, shared SHA/entropy authority, key cleanup and retained failures; no physical qualification");
}
