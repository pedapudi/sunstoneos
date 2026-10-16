#include "result.h"
struct a733_observe_identity a733_observe_identity;
void a733_observe_boot_identity(uint64_t epoch,uint64_t generation,uint64_t lease,
    uint64_t armed,uint64_t deadline)
{
    a733_observe_identity.epoch=epoch;a733_observe_identity.generation=generation;
    a733_observe_identity.lease=lease;a733_observe_identity.armed_at=armed;
    a733_observe_identity.deadline=deadline;
}
static uint32_t ror(uint32_t v,unsigned n){return (v>>n)|(v<<(32-n));}
static uint32_t be32(const uint8_t *p)
{return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static uint64_t le64(const uint8_t *p)
{uint64_t v=0;for(unsigned i=0;i<8;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put64(uint8_t *p,uint64_t v)
{for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(v>>(8*i));}
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
    uint32_t w[64];for(unsigned i=0;i<16;++i)w[i]=be32(p+4*i);
    for(unsigned i=16;i<64;++i){uint32_t a=w[i-15],b=w[i-2];
        w[i]=w[i-16]+(ror(a,7)^ror(a,18)^(a>>3))+w[i-7]+(ror(b,17)^ror(b,19)^(b>>10));}
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
    for(unsigned i=0;i<64;++i){
        uint32_t t=z+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^((~e)&g))+k[i]+w[i];
        uint32_t u=(ror(a,2)^ror(a,13)^ror(a,22))+((a&b)^(a&c)^(b&c));
        z=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
}
void a733_observe_digest(const uint8_t *p,size_t n,uint8_t out[32])
{
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t bits=(uint64_t)n*8;while(n>=64){compress(h,p);p+=64;n-=64;}
    uint8_t tail[128]={0};for(size_t i=0;i<n;++i)tail[i]=p[i];tail[n]=128;
    unsigned total=n<56?64:128;
    for(unsigned i=0;i<8;++i)tail[total-1-i]=(uint8_t)(bits>>(8*i));
    compress(h,tail);if(total==128)compress(h,tail+64);
    for(unsigned i=0;i<32;++i)out[i]=(uint8_t)(h[i/4]>>(24-8*(i%4)));
}
int a733_observe_request(const uint8_t p[64],const struct a733_observe_identity *id,
    uint32_t *sequence,uint8_t challenge[32])
{
    if(le32(p)!=A733_OBSERVE_MESSAGE_MAGIC || !le32(p+4) ||
       le32(p+8)!=A733_OBSERVE_COMMAND || le32(p+12)!=48 ||
       le64(p+16)!=id->epoch || le64(p+24)!=id->generation)return 0;
    unsigned nonzero=0;for(unsigned i=0;i<32;++i)nonzero|=p[32+i];if(!nonzero)return 0;
    *sequence=le32(p+4);for(unsigned i=0;i<32;++i)challenge[i]=p[32+i];return 1;
}
int a733_observe_frame(uint8_t out[512],const struct a733_observe_identity *id,
    const struct a733_observe_snapshot *s,const uint8_t challenge[32],const uint8_t build[32])
{
    if(!id->epoch || !id->generation || !id->lease || id->armed_at>=id->deadline)return 0;
    if(s){
        if(s->start<id->armed_at || s->end<s->start || s->end>=id->deadline)return 0;
        for(unsigned c=0;c<8;++c)if(s->core[c][0]!=32 || !s->core[c][1] ||
            !s->core[c][2] || s->core[c][3] || (!c && s->core[c][4]<32))return 0;
    }
    for(unsigned i=0;i<512;++i)out[i]=0;
    put64(out,A733_OBSERVE_FRAME_MAGIC);put64(out+8,UINT64_C(512)<<32|1);
    put64(out+16,s?0:1);put64(out+24,id->epoch);put64(out+32,id->generation);
    put64(out+40,id->lease);put64(out+48,s?s->start:0);put64(out+56,s?s->end:0);
    put64(out+64,id->deadline);put64(out+72,s?255:0);
    for(unsigned i=0;i<32;++i){out[80+i]=challenge[i];out[112+i]=build[i];}
    if(s)for(unsigned c=0;c<8;++c)for(unsigned f=0;f<5;++f)put64(out+160+40*c+8*f,s->core[c][f]);
    a733_observe_digest(out,480,out+480);return 1;
}
