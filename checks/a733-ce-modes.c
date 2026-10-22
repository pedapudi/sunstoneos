/* Hash methods, HMAC and AES key sizes/modes through the real CE controller,
 * session and services. A model engine executes the submitted descriptors.
 * argv[1] names a request file written by a733-ce-modes.py; that script also
 * supplies the SHA round constants and initial values and checks every result
 * line against hashlib, hmac, OpenSSL and published vectors. */
#include "a733_ce_hash.h"
#include "a733_ce_cipher.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CCU UINT32_C(0x02002000)
#define NS UINT32_C(0x04603000)
#define SEC UINT32_C(0x04603800)
#define PHYS UINT64_C(0x41000000)
struct reg {uintptr_t a;uint32_t v;};
struct model {
    struct a733_clock_leases registry;
    struct a733_clock_token token;
    struct device_memory_pool pool;
    struct device_memory_lease lease;
    struct a733_ce_session session;
    struct a733_ce_hash hash;
    struct a733_ce_cipher cipher;
    struct a733_ce_config config;
    uint8_t visible[sizeof(struct a733_ce_memory)],last_state[64];
    uint64_t hash_processed,now;
    _Alignas(4096) uint8_t pages[8192];
    struct reg regs[40];unsigned count;
    unsigned held,pending,published,starts,hash_starts,cipher_starts,key_cleans,fail_cipher_number,fail_status;
};
static uint32_t *reg(struct model *m,uintptr_t a)
{
    for (unsigned i=0;i<m->count;++i) if (m->regs[i].a==a) return &m->regs[i].v;
    assert(m->count<40);m->regs[m->count]=(struct reg){a,0};return &m->regs[m->count++].v;
}
static uint64_t le(const uint8_t *p,unsigned n)
{uint64_t v=0;for (unsigned i=0;i<n;++i) v|=(uint64_t)p[i]<<(8*i);return v;}
static int now(void *v,uint64_t *n) {*n=((struct model *)v)->now;return 1;}
static int authorize(void *v,const struct a733_ce_config *c,const struct a733_clock_token *t,const struct a733_ce_memory *mem)
{struct model *m=v;assert(c->profile==A733_CE_SHA256_AES128 && t->id==m->token.id && mem==(void *)m->pages);return 1;}
static int enter(void *v,uint64_t d) {struct model *m=v;(void)d;assert(!m->held);m->held=1;return 1;}
static void leave(void *v) {struct model *m=v;assert(m->held);m->held=0;}
static void retain(void *v) {(void)v;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{struct model *m=v;assert(m->held && a!=NS+0xd0 && !(a>=SEC+0x90 && a<=SEC+0x9c));*out=*reg(m,a);return 1;}

/* SHA tables come from the request file; the script derives them from primes. */
static uint64_t k512[80],initial[6][8];
static uint32_t rotate(uint32_t x,unsigned n) {return (x>>n)|(x<<(32-n));}
static uint64_t rotate64(uint64_t x,unsigned n) {return (x>>n)|(x<<(64-n));}
static uint64_t big(const uint8_t *p,unsigned n) {uint64_t v=0;for (unsigned i=0;i<n;++i) v=v<<8|p[i];return v;}
static void put(uint8_t *p,uint64_t v,unsigned n) {for (unsigned i=0;i<n;++i) p[i]=(uint8_t)(v>>(8*(n-1-i)));}
static void sha1(uint64_t s[8],const uint8_t *block)
{
    uint32_t w[80],a=(uint32_t)s[0],b=(uint32_t)s[1],c=(uint32_t)s[2],d=(uint32_t)s[3],e=(uint32_t)s[4];
    for (unsigned i=0;i<16;++i) w[i]=(uint32_t)big(block+4*i,4);
    for (unsigned i=16;i<80;++i) w[i]=rotate(w[i-3]^w[i-8]^w[i-14]^w[i-16],31);
    for (unsigned i=0;i<80;++i) {
        uint32_t f=i<20?((b&c)|(~b&d)):(i<40||i>=60?(b^c^d):((b&c)|(b&d)|(c&d)));
        uint32_t k=i<20?0x5a827999:(i<40?0x6ed9eba1:(i<60?0x8f1bbcdc:0xca62c1d6));
        uint32_t t=rotate(a,27)+f+e+k+w[i];e=d;d=c;c=rotate(b,2);b=a;a=t;
    }
    s[0]=(uint32_t)(s[0]+a);s[1]=(uint32_t)(s[1]+b);s[2]=(uint32_t)(s[2]+c);s[3]=(uint32_t)(s[3]+d);s[4]=(uint32_t)(s[4]+e);
}
static void sha256(uint64_t s[8],const uint8_t *block)
{
    uint32_t w[64],v[8];for (unsigned i=0;i<8;++i) v[i]=(uint32_t)s[i];
    for (unsigned i=0;i<16;++i) w[i]=(uint32_t)big(block+4*i,4);
    for (unsigned i=16;i<64;++i) {
        uint32_t x=w[i-15],y=w[i-2];
        w[i]=w[i-16]+(rotate(x,7)^rotate(x,18)^(x>>3))+w[i-7]+(rotate(y,17)^rotate(y,19)^(y>>10));
    }
    for (unsigned i=0;i<64;++i) {
        uint32_t e=v[4],a=v[0];
        uint32_t t=v[7]+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&v[5])^(~e&v[6]))+(uint32_t)(k512[i]>>32)+w[i];
        uint32_t u=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&v[1])^(a&v[2])^(v[1]&v[2]));
        memmove(v+1,v,7*sizeof(v[0]));v[4]+=t;v[0]=t+u;
    }
    for (unsigned i=0;i<8;++i) s[i]=(uint32_t)(s[i]+v[i]);
}
static void sha512(uint64_t s[8],const uint8_t *block)
{
    uint64_t w[80],v[8];memcpy(v,s,sizeof(v));
    for (unsigned i=0;i<16;++i) w[i]=big(block+8*i,8);
    for (unsigned i=16;i<80;++i) {
        uint64_t x=w[i-15],y=w[i-2];
        w[i]=w[i-16]+(rotate64(x,1)^rotate64(x,8)^(x>>7))+w[i-7]+(rotate64(y,19)^rotate64(y,61)^(y>>6));
    }
    for (unsigned i=0;i<80;++i) {
        uint64_t e=v[4],a=v[0];
        uint64_t t=v[7]+(rotate64(e,14)^rotate64(e,18)^rotate64(e,41))+((e&v[5])^(~e&v[6]))+k512[i]+w[i];
        uint64_t u=(rotate64(a,28)^rotate64(a,34)^rotate64(a,39))+((a&v[1])^(a&v[2])^(v[1]&v[2]));
        memmove(v+1,v,7*sizeof(v[0]));v[4]+=t;v[0]=t+u;
    }
    for (unsigned i=0;i<8;++i) s[i]+=v[i];
}

/* Model-only AES (FIPS 197) for 128/192/256-bit keys. */
static uint8_t box[256],inverse[256];
static uint8_t multiply(uint8_t a,uint8_t b)
{uint8_t p=0;while (b) {if (b&1) p^=a;a=(uint8_t)((a<<1)^((a&128)?0x1b:0));b>>=1;}return p;}
static void tables(void)
{
    for (unsigned i=0;i<256;++i) {
        uint8_t x=1;for (unsigned n=0;n<254;++n) x=multiply(x,(uint8_t)i);
        uint8_t r=x;for (unsigned n=1;n<5;++n) r^=(uint8_t)((x<<n)|(x>>(8-n)));
        box[i]=r^0x63;inverse[box[i]]=(uint8_t)i;
    }
}
static unsigned expand(const uint8_t *key,unsigned key_bytes,uint8_t w[240])
{
    unsigned nk=key_bytes/4,rounds=nk+6;uint8_t rc=1;memcpy(w,key,key_bytes);
    for (unsigned i=nk;i<4*(rounds+1);++i) {
        uint8_t t[4];memcpy(t,w+4*(i-1),4);
        if (i%nk==0) {uint8_t f=t[0];t[0]=(uint8_t)(box[t[1]]^rc);t[1]=box[t[2]];t[2]=box[t[3]];t[3]=box[f];rc=multiply(rc,2);}
        else if (nk>6 && i%nk==4) for (unsigned j=0;j<4;++j) t[j]=box[t[j]];
        for (unsigned j=0;j<4;++j) w[4*i+j]=w[4*(i-nk)+j]^t[j];
    }
    return rounds;
}
static void block_cipher(const uint8_t *w,unsigned rounds,const uint8_t in[16],uint8_t out[16],unsigned decrypt)
{
    static const uint8_t forward[4]={2,3,1,1},backward[4]={14,11,13,9};
    uint8_t s[16],t[16];
    for (unsigned i=0;i<16;++i) s[i]=in[i]^w[(decrypt?rounds:0)*16+i];
    for (unsigned round=1;round<=rounds;++round) {
        for (unsigned c=0;c<4;++c) for (unsigned r=0;r<4;++r)
            t[c*4+r]=(decrypt?inverse:box)[s[((c+(decrypt?4-r:r))%4)*4+r]];
        unsigned key=decrypt?rounds-round:round;
        if (decrypt) for (unsigned i=0;i<16;++i) t[i]^=w[key*16+i];
        if (round<rounds) {
            for (unsigned c=0;c<4;++c) for (unsigned r=0;r<4;++r) {
                uint8_t v=0;
                for (unsigned j=0;j<4;++j) v^=multiply(t[c*4+j],(decrypt?backward:forward)[(j+4-r)%4]);
                s[c*4+r]=v;
            }
        } else memcpy(s,t,16);
        if (!decrypt) for (unsigned i=0;i<16;++i) s[i]^=w[key*16+i];
    }
    memcpy(out,s,16);
}
static unsigned symmetric_tasks,width_seen[4];
static void complete(struct model *m)
{
    assert(m->pending);if (*reg(m,NS+12)) return;*reg(m,NS+12)=m->fail_status?0x20:0x10;
    const uint8_t *d=m->visible;
    if (le(d,4)==2) {
        uint32_t control=(uint32_t)le(d+8,4);unsigned mode=(control>>8)&15,decrypt=!!(le(d+4,4)&256);
        uint8_t w[240],chain[16],next[16];unsigned rounds=expand(d+3456,16+8*(control&3),w);
        size_t n=(size_t)le(d+32,4);memcpy(chain,d+320,16);
        for (size_t at=0;at<n;at+=16) {
            const uint8_t *in=d+384+at;uint8_t *out=m->pages+2432+at,x[16];
            if (mode==A733_CE_AES_ECB) block_cipher(w,rounds,in,out,decrypt);
            else if (mode==A733_CE_AES_CBC && !decrypt) {
                for (unsigned i=0;i<16;++i) x[i]=in[i]^chain[i];
                block_cipher(w,rounds,x,out,0);memcpy(chain,out,16);
            } else if (mode==A733_CE_AES_CBC) {
                block_cipher(w,rounds,in,x,1);memcpy(next,in,16);
                for (unsigned i=0;i<16;++i) out[i]=x[i]^chain[i];
                memcpy(chain,next,16);
            } else {
                block_cipher(w,rounds,chain,x,0);
                for (unsigned i=0;i<16;++i) out[i]=in[i]^x[i];
                unsigned bytes=((control>>2)&3)==1?4:16,carry=1;
                for (unsigned i=0;i<bytes && carry;++i) {chain[15-i]=(uint8_t)(chain[15-i]+1);carry=!chain[15-i];}
            }
        }
        /* The engine reports its next counter block. The driver must chain
         * from its own arithmetic, so the model stores an unusable value. */
        if (mode) memset(m->pages+256,0x5a,16);
        ++symmetric_tasks;
        return;
    }
    unsigned method=(unsigned)le(d+4,4),state_bytes=a733_ce_hash_state(method),word=method>=4?8:4;
    uint64_t state[8];memcpy(state,initial[method],sizeof(state));
    if (le(d,4)&256) for (unsigned i=0;i<state_bytes/word;++i) state[i]=big(d+320+word*i,word);
    size_t n=(size_t)le(d+36,4);
    for (size_t at=0;at<n;at+=a733_ce_hash_block(method))
        (method==1?sha1:(method<4?sha256:sha512))(state,d+384+at);
    for (unsigned i=0;i<state_bytes/word;++i) put(m->pages+256+word*i,state[i],word);
    memcpy(m->last_state,m->pages+256,state_bytes);
}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{
    struct model *m=v;assert(m->held && a>=NS && a<SEC);
    if (a==NS+12) {*reg(m,a)&=~value;m->pending=0;return 1;}
    if (a!=NS+0x10) {*reg(m,a)=value;return 1;}
    assert(m->published==m->starts+1 && !m->pending && *reg(m,NS)==(uint32_t)PHYS && (*reg(m,NS+8)&15)==4);
    const uint8_t *d=m->visible;
    if (value==1) {
        uint32_t comm=(uint32_t)le(d+4,4),control=(uint32_t)le(d+8,4);
        unsigned mode=(control>>8)&15,width=(control>>2)&3,key_bytes=16+8*(control&3);
        assert(le(d,4)==2 && (comm&~UINT32_C(0x100))==0x80000000 && !le(d+12,4));
        assert((control&~UINT32_C(0xf0f))==0 && (control&3)<3 && mode<=A733_CE_AES_CTR);
        assert(mode==A733_CE_AES_CTR ? (!(comm&256) && (width==1 || width==3)):!width);
        assert(le(d+16,5)==PHYS+3456);
        if (mode) assert(le(d+21,5)==PHYS+320 && le(d+26,5)==PHYS+256);
        else for (unsigned i=21;i<31;++i) assert(!d[i]);
        size_t n=(size_t)le(d+32,4);assert(n && n<=1024 && n%16==0);
        assert(le(d+36,5)==PHYS+384 && le(d+41,5)==PHYS+2432 && le(d+48,4)==n && le(d+52,4)==n);
        for (unsigned i=56;i<256;++i) assert(!d[i]);
        for (unsigned i=key_bytes;i<64;++i) assert(!d[3456+i]);
        if (!mode) for (unsigned i=0;i<64;++i) assert(!d[320+i]);
        assert(m->cipher_starts==m->key_cleans);++m->cipher_starts;++width_seen[width];
    } else {
        assert(value==0x400);
        unsigned method=(unsigned)le(d+4,4),state_bytes=a733_ce_hash_state(method),control=(unsigned)le(d,4);
        assert(state_bytes && (control==0x10002 || control==0x10102) && le(d+40,4)==state_bytes);
        size_t n=(size_t)le(d+36,4);assert(n && n<=2048 && n%a733_ce_hash_block(method)==0);
        assert(le(d+8,5)==n*8 && le(d+24,5)==PHYS+384 && le(d+29,5)==PHYS+256);
        if (control&256) {assert(le(d+18,5)==PHYS+320 && !memcmp(d+320,m->last_state,state_bytes));}
        else {assert(!le(d+18,5));m->hash_processed=0;}
        for (unsigned i=state_bytes;i<64;++i) assert(!d[320+i]);
        m->hash_processed+=n;assert((le(d+200,4)<<32|le(d+204,4))==m->hash_processed*8);
        ++m->hash_starts;
    }
    ++m->starts;m->pending=1;
    return m->fail_cipher_number && m->cipher_starts==m->fail_cipher_number ? -1:1;
}
static int sync_memory(void *v,struct a733_ce_memory *mem,enum a733_ce_sync op)
{
    struct model *m=v;assert(m->held && mem==(void *)m->pages);
    if (op==A733_CE_PUBLISH || op==A733_CE_CIPHER_PUBLISH) {assert(!m->pending);memcpy(m->visible,mem,sizeof(*mem));++m->published;}
    else if (op==A733_CE_KEY_CLEAN) {
        /* Output (next counter), IV, input, cipher output and key lines. */
        for (size_t i=256;i<3520;++i) assert(!((uint8_t *)mem)[i]);
        memcpy(m->visible+256,(uint8_t *)mem+256,3520-256);++m->key_cleans;
    } else if (op==A733_CE_SCRUB) {
        for (size_t i=256;i<2432;++i) assert(!((uint8_t *)mem)[i]);
        memcpy(m->visible+256,(uint8_t *)mem+256,2432-256);
    } else assert(op==A733_CE_ACQUIRE || op==A733_CE_CIPHER_ACQUIRE);
    return 1;
}
static const struct a733_ce_ops ops={now,authorize,enter,leave,retain,read_reg,write_reg,sync_memory};
static void setup(struct model *m)
{
    memset(m,0,sizeof(*m));m->now=1;
    assert(a733_clock_leases_init(&m->registry,now,m));
    assert(a733_clock_acquire(&m->registry,5,A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC,
        A733_CLOCK_CE,10000,&m->token)==A733_CLOCK_OK);
    assert(device_memory_pool_init(&m->pool,(uintptr_t)m->pages,PHYS,sizeof(m->pages)));
    struct device_memory_extent e;assert(device_memory_acquire(&m->pool,5,4096,&m->lease,&e));
    m->config=(struct a733_ce_config){.owner=5,.handoff=7,.deadline_us=10000,
        .fabric={0x03000001,0x83000000,0x84000000},.fabric_hz={300000000,600000000,400000000},
        .profile=A733_CE_SHA256_AES128};
    *reg(m,0x07090160)=0x10000002;*reg(m,CCU)=0xf8000000|79u<<8|79u<<16;
    *reg(m,CCU+0xa0)=0xfe000000|99u<<8|1u<<20;*reg(m,CCU+0x1908)=0x0a060a06;*reg(m,CCU+0x1988)=0x0a060000;
    *reg(m,CCU+0xac0)=0x81000000;*reg(m,CCU+0xac4)=0x30003;*reg(m,CCU+0x5e4)=4;
    for (unsigned i=0;i<3;++i) *reg(m,CCU+(unsigned[]){0x500,0x580,0x588}[i])=m->config.fabric[i];
    assert(a733_ce_session_bind(&m->session,&m->pool,m->lease,&m->config,&ops,m,&m->registry,&m->token));
    assert(a733_ce_hash_bind(&m->hash,&m->session) && a733_ce_cipher_bind(&m->cipher,&m->session));
}
static void unchanged(const uint8_t *p,size_t n) {for (size_t i=0;i<n;++i) assert(p[i]==0xa5);}
static void zeroed(const uint8_t *p,size_t n) {for (size_t i=0;i<n;++i) assert(!p[i]);}
static void hex(const uint8_t *p,size_t n) {for (size_t i=0;i<n;++i) printf("%02x",p[i]);if (!n) putchar('-');}
static size_t unhex(const char *text,uint8_t *out,size_t room)
{
    if (!strcmp(text,"-")) return 0;
    size_t n=strlen(text)/2;assert(strlen(text)%2==0 && n<=room);
    for (size_t i=0;i<n;++i) {unsigned v;assert(sscanf(text+2*i,"%2x",&v)==1);out[i]=(uint8_t)v;}
    return n;
}
static void hash_case(struct model *m,unsigned method,const uint8_t *key,size_t key_bytes,size_t length)
{
    uint8_t input[2048],out[64];memset(out,0xa5,sizeof(out));uint64_t g=0;
    enum a733_ce_hash_result r=a733_ce_hash_start(&m->hash,method,key,key_bytes,10000,&g);
    unsigned digest=a733_ce_hash_digest(method);
    assert(g && (r==A733_CE_HASH_READY || (r==A733_CE_HASH_PENDING && key_bytes>a733_ce_hash_block(method))));
    for (unsigned i=0;i<40 && r==A733_CE_HASH_PENDING;++i) {
        if (m->pending) complete(m);
        r=a733_ce_hash_poll_digest(&m->hash,g,out,digest);unchanged(out,digest);
    }
    assert(r==A733_CE_HASH_READY);
    for (size_t at=0;at<length;) {
        size_t n=(at%3==0)?11:2048;if (n>length-at) n=length-at;
        for (size_t i=0;i<n;++i) input[i]=(uint8_t)((at+i)*37+11);
        size_t accepted=0;r=a733_ce_hash_update(&m->hash,g,input,n,&accepted);
        assert((r==A733_CE_HASH_READY || r==A733_CE_HASH_PENDING) && accepted);
        at+=accepted;memset(input,0xcc,sizeof(input));
        for (unsigned i=0;i<40 && r==A733_CE_HASH_PENDING;++i) {
            if (m->pending) complete(m);
            r=a733_ce_hash_poll_digest(&m->hash,g,out,digest);
        }
        assert(r==A733_CE_HASH_READY);
    }
    assert(a733_ce_hash_final(&m->hash,g)==A733_CE_HASH_PENDING);r=A733_CE_HASH_PENDING;
    for (unsigned i=0;i<80 && r==A733_CE_HASH_PENDING;++i) {
        if (m->pending) complete(m);
        r=a733_ce_hash_poll_digest(&m->hash,g,out,digest);
        if (r!=A733_CE_HASH_DIGEST) unchanged(out,digest);
    }
    assert(r==A733_CE_HASH_DIGEST);unchanged(out+digest,sizeof(out)-digest);
    printf("hash %u ",method);if (key) hex(key,key_bytes);else putchar('*');printf(" %zu ",length);hex(out,digest);puts("");
    assert(!m->session.client && a733_ce_idle(&m->session.controller));
    zeroed(m->hash.buffer,sizeof(m->hash.buffer));zeroed(m->hash.digest,64);zeroed(m->hash.key,128);
    zeroed(m->pages+256,2432-256);zeroed(m->visible+256,2432-256);
}
static enum a733_ce_cipher_result cipher_poll(struct model *m,uint64_t g,uint8_t *out,size_t n)
{
    enum a733_ce_cipher_result r=A733_CE_CIPHER_PENDING;
    for (unsigned i=0;i<60 && r==A733_CE_CIPHER_PENDING;++i) {
        if (m->pending) complete(m);
        r=a733_ce_cipher_poll(&m->cipher,g,out,n);
        if (r!=A733_CE_CIPHER_OUTPUT) unchanged(out,n);
    }
    return r;
}
static void idle_after_cipher(struct model *m)
{
    assert(!m->session.client && a733_ce_idle(&m->session.controller) && m->key_cleans==m->cipher_starts);
    zeroed(m->pages+256,3520-256);zeroed(m->visible+256,3520-256);
    zeroed(m->cipher.key,32);zeroed(m->cipher.iv,16);zeroed(m->cipher.hash_subkey,16);zeroed(m->cipher.j0,16);
    zeroed(m->cipher.tag,16);zeroed(m->cipher.aad,sizeof(m->cipher.aad));zeroed(m->cipher.nonce,sizeof(m->cipher.nonce));
    zeroed(m->cipher.input,sizeof(m->cipher.input));zeroed(m->cipher.output,sizeof(m->cipher.output));
}
/* Returns the result; prints one line. */
static enum a733_ce_cipher_result cipher_case(struct model *m,const char *line)
{
    char text[7][4200];unsigned mode,decrypt;size_t tag_bytes;
    assert(sscanf(line,"cipher %u %u %4199s %4199s %4199s %zu %4199s",&mode,&decrypt,text[0],text[1],text[2],&tag_bytes,text[3])==7);
    uint8_t key[32],iv[64],aad[512],input[2100],out[2100];
    struct a733_ce_cipher_params c={.mode=mode,.decrypt=decrypt,.tag_bytes=tag_bytes};
    c.key_bytes=unhex(text[0],key,sizeof(key));c.key=key;
    c.iv_bytes=unhex(text[1],iv,sizeof(iv));if (c.iv_bytes) c.iv=iv;
    c.aad_bytes=unhex(text[2],aad,sizeof(aad));if (c.aad_bytes) c.aad=aad;
    size_t n=unhex(text[3],input,sizeof(input));
    size_t outn=mode==A733_CE_CIPHER_GCM?(decrypt?n-tag_bytes:n+tag_bytes):n;
    unsigned first=m->cipher_starts;uint64_t g=0,other=0;
    assert(a733_ce_cipher_start(&m->cipher,&c,input,n,10000,&g)==A733_CE_CIPHER_PENDING && g);
    memset(key,0xcc,sizeof(key));memset(iv,0xcc,sizeof(iv));memset(aad,0xcc,sizeof(aad));memset(input,0xcc,sizeof(input));
    assert(a733_ce_hash_begin(&m->hash,10000,&other)==A733_CE_HASH_BUSY);
    memset(out,0xa5,sizeof(out));
    enum a733_ce_cipher_result r=cipher_poll(m,g,out,outn);
    assert(r==A733_CE_CIPHER_OUTPUT || (r==A733_CE_CIPHER_AUTH && mode==A733_CE_CIPHER_GCM && decrypt));
    if (r==A733_CE_CIPHER_AUTH) unchanged(out,sizeof(out));else unchanged(out+outn,sizeof(out)-outn);
    size_t tasks=((mode==A733_CE_CIPHER_GCM?16:0)+n-(mode==A733_CE_CIPHER_GCM && decrypt?tag_bytes:0)+15)/16*16;
    assert(m->cipher_starts-first==(tasks+1023)/1024+(mode==A733_CE_CIPHER_GCM));
    idle_after_cipher(m);
    printf("%s ",line);
    if (r==A733_CE_CIPHER_AUTH) puts("auth");else {hex(out,outn);puts("");}
    memset(out,0xa5,sizeof(out));assert(a733_ce_cipher_poll(&m->cipher,g,out,outn)==A733_CE_CIPHER_CONSUMED);unchanged(out,outn);
    return r;
}
static void refusals(struct model *m)
{
    setup(m);struct a733_ce *p=&m->session.controller;uint8_t data[256]={0},state[64]={0},key[32]={0},iv[16]={0},out[64];uint64_t g=77;
    /* Controller: unknown method, wrong block multiple, state/total mismatches. */
    assert(a733_ce_digest_request(p,0,data,64,0,0,512,10000)==A733_CE_INVALID);
    assert(a733_ce_digest_request(p,6,data,64,0,0,512,10000)==A733_CE_INVALID);
    assert(a733_ce_digest_request(p,A733_CE_SHA512,data,64,0,0,512,10000)==A733_CE_INVALID);
    assert(a733_ce_digest_request(p,A733_CE_SHA384,data,128,state,1,1024,10000)==A733_CE_INVALID);
    assert(a733_ce_digest_request(p,A733_CE_SHA1,data,64,0,1,1024,10000)==A733_CE_INVALID);
    /* Key size, IV presence, CTR direction and counter width. */
    const struct a733_ce_symmetric bad[]={
        {.key=key,.key_bytes=20},{.key=key,.key_bytes=16,.iv=iv},{.key=key,.key_bytes=32,.mode=A733_CE_AES_CBC},
        {.key=key,.key_bytes=16,.iv=iv,.mode=A733_CE_AES_CTR,.counter_bits=128,.decrypt=1},
        {.key=key,.key_bytes=16,.iv=iv,.mode=A733_CE_AES_CTR,.counter_bits=64},
        {.key=key,.key_bytes=16,.iv=iv,.mode=A733_CE_AES_CBC,.counter_bits=32},{.key=key,.key_bytes=24,.mode=3,.iv=iv},
        {.key=m->pages,.key_bytes=16}};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) assert(a733_ce_symmetric_request(p,bad+i,data,16,10000)==A733_CE_INVALID);
    const struct a733_ce_symmetric good={.key=key,.key_bytes=32,.iv=iv,.mode=A733_CE_AES_CTR,.counter_bits=32};
    assert(a733_ce_symmetric_request(p,&good,data,17,10000)==A733_CE_INVALID);
    /* Services: method, key length and digest size. */
    uint8_t long_key[A733_CE_HMAC_KEY_MAX+1]={0};
    assert(a733_ce_hash_start(&m->hash,0,0,0,10000,&g)==A733_CE_HASH_INVALID);
    assert(a733_ce_hash_start(&m->hash,6,0,0,10000,&g)==A733_CE_HASH_INVALID);
    assert(a733_ce_hash_start(&m->hash,A733_CE_SHA1,0,5,10000,&g)==A733_CE_HASH_INVALID);
    assert(a733_ce_hash_start(&m->hash,A733_CE_SHA1,long_key,sizeof(long_key),10000,&g)==A733_CE_HASH_INVALID);
    assert(a733_ce_hash_start(&m->hash,A733_CE_SHA1,(const uint8_t *)&g,4,10000,&g)==A733_CE_HASH_INVALID);
    assert(g==77 && !m->starts && !m->session.client);
    assert(a733_ce_hash_start(&m->hash,A733_CE_SHA384,0,0,10000,&g)==A733_CE_HASH_READY);
    memset(out,0xa5,sizeof(out));
    assert(a733_ce_hash_poll(&m->hash,g,out)==A733_CE_HASH_INVALID);
    assert(a733_ce_hash_poll_digest(&m->hash,g,out,64)==A733_CE_HASH_INVALID);
    assert(a733_ce_hash_poll_digest(&m->hash,g,out,48)==A733_CE_HASH_READY);unchanged(out,64);
    assert(a733_ce_hash_final(&m->hash,g)==A733_CE_HASH_PENDING);
    enum a733_ce_hash_result h=A733_CE_HASH_PENDING;
    for (unsigned i=0;i<10 && h==A733_CE_HASH_PENDING;++i) {if (m->pending) complete(m);h=a733_ce_hash_poll_digest(&m->hash,g,out,48);}
    assert(h==A733_CE_HASH_DIGEST && !m->session.client);
    /* Cipher parameters: each refusal starts nothing and leaves the slot free. */
    uint8_t aad[513]={0};unsigned starts=m->starts;g=77;
    const struct a733_ce_cipher_params params[]={
        {.key=key,.key_bytes=20,.mode=A733_CE_CIPHER_ECB},
        {.key=key,.key_bytes=16,.iv=iv,.iv_bytes=16,.mode=A733_CE_CIPHER_ECB},
        {.key=key,.key_bytes=16,.iv=iv,.iv_bytes=12,.mode=A733_CE_CIPHER_CBC},
        {.key=key,.key_bytes=16,.mode=A733_CE_CIPHER_CTR},
        {.key=key,.key_bytes=16,.iv=iv,.iv_bytes=16,.mode=A733_CE_CIPHER_CTR,.tag_bytes=16},
        {.key=key,.key_bytes=16,.iv=iv,.iv_bytes=12,.mode=A733_CE_CIPHER_GCM,.tag_bytes=11},
        {.key=key,.key_bytes=16,.iv=iv,.iv_bytes=12,.mode=A733_CE_CIPHER_GCM,.tag_bytes=17},
        {.key=key,.key_bytes=16,.mode=A733_CE_CIPHER_GCM,.tag_bytes=16},
        {.key=key,.key_bytes=16,.iv=data,.iv_bytes=65,.mode=A733_CE_CIPHER_GCM,.tag_bytes=16},
        {.key=key,.key_bytes=16,.iv=iv,.iv_bytes=12,.aad=aad,.aad_bytes=513,.mode=A733_CE_CIPHER_GCM,.tag_bytes=16},
        {.key=key,.key_bytes=16,.iv=iv,.iv_bytes=12,.aad_bytes=4,.mode=A733_CE_CIPHER_GCM,.tag_bytes=16},
        {.key=key,.key_bytes=16,.iv=iv,.iv_bytes=12,.mode=A733_CE_CIPHER_GCM,.tag_bytes=16,.decrypt=1},
        {.key=key,.key_bytes=16,.iv=m->pages,.iv_bytes=12,.mode=A733_CE_CIPHER_GCM,.tag_bytes=16},
        {.key=key,.key_bytes=16,.mode=4}};
    const size_t lengths[]={16,16,16,16,16,16,16,16,16,16,16,15,16,16};
    for (unsigned i=0;i<sizeof(params)/sizeof(params[0]);++i)
        assert(a733_ce_cipher_start(&m->cipher,params+i,data,lengths[i],10000,&g)==A733_CE_CIPHER_INVALID);
    const struct a733_ce_cipher_params ctr={.key=key,.key_bytes=16,.iv=iv,.iv_bytes=16,.mode=A733_CE_CIPHER_CTR};
    assert(a733_ce_cipher_start(&m->cipher,&ctr,data,0,10000,&g)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_start(&m->cipher,&ctr,data,2049,10000,&g)==A733_CE_CIPHER_INVALID);
    assert(a733_ce_cipher_start(&m->cipher,&ctr,(const uint8_t *)&g,8,10000,&g)==A733_CE_CIPHER_INVALID);
    assert(g==77 && m->starts==starts && !m->session.client);
    /* A started CTR request rejects mismatched output sizes. */
    assert(a733_ce_cipher_start(&m->cipher,&ctr,data,5,10000,&g)==A733_CE_CIPHER_PENDING);
    assert(a733_ce_cipher_poll(&m->cipher,g,out,16)==A733_CE_CIPHER_INVALID);
    memset(out,0xa5,sizeof(out));assert(cipher_poll(m,g,out,5)==A733_CE_CIPHER_OUTPUT);idle_after_cipher(m);
}
/* A failed GCM data task retains the session and publishes nothing. */
static void retained_gcm(struct model *m)
{
    setup(m);uint8_t key[16]={1},iv[12]={2},data[1100]={3},out[1200];uint64_t g=0;
    const struct a733_ce_cipher_params c={.key=key,.key_bytes=16,.iv=iv,.iv_bytes=12,.mode=A733_CE_CIPHER_GCM,.tag_bytes=16};
    m->fail_cipher_number=3;
    assert(a733_ce_cipher_start(&m->cipher,&c,data,sizeof(data),10000,&g)==A733_CE_CIPHER_PENDING);
    memset(out,0xa5,sizeof(out));
    assert(cipher_poll(m,g,out,sizeof(data)+16)==A733_CE_CIPHER_RETAINED);unchanged(out,sizeof(out));
    zeroed(m->cipher.key,32);zeroed(m->cipher.hash_subkey,16);zeroed(m->cipher.input,sizeof(m->cipher.input));
    zeroed(m->cipher.output,sizeof(m->cipher.output));
    struct device_memory_extent e;enum device_memory_state state;
    assert(device_memory_describe(&m->pool,m->lease,&e,&state) && state==DEVICE_MEMORY_RETAINED);
    assert(m->session.controller.key_dirty && m->session.client==&m->cipher);
    uint64_t other=0;assert(a733_ce_hash_begin(&m->hash,20000,&other)==A733_CE_HASH_RETAINED);
    /* A completion error status during an HMAC stream is retained as well. */
    setup(m);m->fail_status=1;
    assert(a733_ce_hash_start(&m->hash,A733_CE_SHA512,key,16,10000,&g)==A733_CE_HASH_READY);
    assert(a733_ce_hash_final(&m->hash,g)==A733_CE_HASH_PENDING);
    enum a733_ce_hash_result h=A733_CE_HASH_PENDING;memset(out,0xa5,sizeof(out));
    for (unsigned i=0;i<10 && h==A733_CE_HASH_PENDING;++i) {if (m->pending) complete(m);h=a733_ce_hash_poll_digest(&m->hash,g,out,64);}
    assert(h==A733_CE_HASH_RETAINED);unchanged(out,64);zeroed(m->hash.key,128);zeroed(m->hash.buffer,sizeof(m->hash.buffer));
}
int main(int argc,char **argv)
{
    assert(argc==2);FILE *f=fopen(argv[1],"r");assert(f);tables();
    static struct model m;static char line[20000];unsigned auth=0,cases=0;
    setup(&m);
    while (fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\n")]=0;
        if (!strncmp(line,"k512 ",5)) {
            char *at=line+5;for (unsigned i=0;i<80;++i) k512[i]=strtoull(at,&at,16);
        } else if (!strncmp(line,"initial ",8)) {
            char *at=line+8;unsigned method=(unsigned)strtoul(at,&at,10);
            for (unsigned i=0;i<8;++i) initial[method][i]=strtoull(at,&at,16);
        } else if (!strncmp(line,"hash ",5)) {
            unsigned method;char key[2200];size_t length;uint8_t bytes[1100];
            assert(sscanf(line,"hash %u %2199s %zu",&method,key,&length)==3);
            size_t n=strcmp(key,"*")?unhex(key,bytes,sizeof(bytes)):0;
            hash_case(&m,method,strcmp(key,"*")?bytes:0,n,length);++cases;
        } else if (!strncmp(line,"cipher ",7)) {
            auth+=cipher_case(&m,line)==A733_CE_CIPHER_AUTH;++cases;
        } else assert(!line[0]);
    }
    fclose(f);
    assert(width_seen[1] && width_seen[3] && auth);
    refusals(&m);retained_gcm(&m);
    printf("PASS %u hash/HMAC/AES-mode requests, %u rejected GCM tags, refusals and retained failures; no physical qualification\n",cases,auth);
}
