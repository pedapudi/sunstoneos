#include "a733_storage_digest.h"
#include <string.h>
static uint32_t ror(uint32_t v,unsigned n){return (v>>n)|(v<<(32-n));}
static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static const uint32_t k[64]={
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static void compress(uint32_t h[8],const uint8_t p[64])
{
    uint32_t w[64];
#if defined(__BYTE_ORDER__)&&__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    /* Word loads and byte reversal; strict-alignment targets need 4-byte alignment. */
    if(!((uintptr_t)p&3)){const uint8_t *q=__builtin_assume_aligned(p,4);
        for(unsigned i=0;i<16;++i){uint32_t v;__builtin_memcpy(&v,q+4*i,4);w[i]=__builtin_bswap32(v);}}
    else
#endif
    for(unsigned i=0;i<16;++i)w[i]=be32(p+4*i);
    for(unsigned i=16;i<64;++i){uint32_t a=w[i-15],b=w[i-2];
        w[i]=w[i-16]+(ror(a,7)^ror(a,18)^(a>>3))+w[i-7]+(ror(b,17)^ror(b,19)^(b>>10));}
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
    /* Eight rounds per iteration rename the working variables instead of
     * shifting all eight through registers after every round. */
#define ROUND(a,b,c,d,e,f,g,z,i) do{ \
        uint32_t t=z+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^((~e)&g))+k[i]+w[i]; \
        d+=t;z=t+(ror(a,2)^ror(a,13)^ror(a,22))+((a&b)^(a&c)^(b&c));}while(0)
    for(unsigned i=0;i<64;i+=8){
        ROUND(a,b,c,d,e,f,g,z,i);ROUND(z,a,b,c,d,e,f,g,i+1);
        ROUND(g,z,a,b,c,d,e,f,i+2);ROUND(f,g,z,a,b,c,d,e,i+3);
        ROUND(e,f,g,z,a,b,c,d,i+4);ROUND(d,e,f,g,z,a,b,c,i+5);
        ROUND(c,d,e,f,g,z,a,b,i+6);ROUND(b,c,d,e,f,g,z,a,i+7);
    }
#undef ROUND
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
}

void storage_digest_init(struct storage_digest *d)
{*d=(struct storage_digest){.h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};}
int storage_digest_add(struct storage_digest *d,const void *data,size_t n)
{
    const uint8_t *p=data;
    if(!d||(!p&&n)||n>UINT64_MAX/8-d->bytes)return 0;
    d->bytes+=n;
    while(n){
        /* Compress complete blocks in place; copy only partial blocks. */
        if(!d->used&&n>=64){compress(d->h,p);p+=64;n-=64;continue;}
        size_t k=64-d->used;if(k>n)k=n;memcpy(d->tail+d->used,p,k);d->used+=(unsigned)k;p+=k;n-=k;
        if(d->used==64){compress(d->h,d->tail);d->used=0;}}
    return 1;
}
void storage_digest_end(const struct storage_digest *source,uint8_t out[32])
{
    struct storage_digest d=*source;uint8_t tail[128]={0};
    memcpy(tail,d.tail,d.used);tail[d.used]=128;unsigned n=d.used<56?64:128;
    uint64_t bits=d.bytes*8;for(unsigned i=0;i<8;++i)tail[n-1-i]=(uint8_t)(bits>>(8*i));
    compress(d.h,tail);if(n==128)compress(d.h,tail+64);
    for(unsigned i=0;i<32;++i)out[i]=(uint8_t)(d.h[i/4]>>(24-8*(i%4)));
}
void storage_digest_bytes(const void *p,size_t n,uint8_t out[32])
{struct storage_digest d;storage_digest_init(&d);(void)storage_digest_add(&d,p,n);storage_digest_end(&d,out);}
