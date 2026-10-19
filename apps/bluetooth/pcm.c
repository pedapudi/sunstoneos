#include "pcm.h"
#include <string.h>
static void put16(uint8_t *p,int32_t x){uint16_t value=(uint16_t)x;p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);}
static void put32(uint8_t *p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(8*i));}
static int32_t sample(const uint8_t *p){uint32_t x=p[0]|(uint32_t)p[1]<<8;return x<32768?(int32_t)x:(int32_t)x-65536;}
static int disjoint(const void *a,size_t n,const void *b,size_t m)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&n<=UINTPTR_MAX-x&&m<=UINTPTR_MAX-y&&(x+n<=y||y+m<=x);}
int bluetooth_pcm_wav(const uint8_t *pcm,size_t bytes,uint8_t *out,size_t capacity,size_t *written)
{
    if(!bytes||(bytes&1)||bytes>48000||capacity<44+2*bytes||
        !disjoint(pcm,bytes,out,capacity)||!disjoint(pcm,bytes,written,sizeof(*written))||
        !disjoint(out,capacity,written,sizeof(*written)))return -1;
    memcpy(out,"RIFF",4);put32(out+4,(uint32_t)(36+2*bytes));memcpy(out+8,"WAVEfmt ",8);
    put32(out+16,16);put16(out+20,1);put16(out+22,1);put32(out+24,16000);put32(out+28,32000);
    put16(out+32,2);put16(out+34,16);memcpy(out+36,"data",4);put32(out+40,(uint32_t)(2*bytes));
    for(size_t at=0;at<bytes;at+=2){int32_t a=sample(pcm+at),b=at+2<bytes?sample(pcm+at+2):a;
        put16(out+44+2*at,a);put16(out+46+2*at,(a+b)/2);}
    *written=44+2*bytes;return 1;
}

/* Already decoded wideband PCM enters the unchanged16k recorded engine. */
int bluetooth_pcm16_wav(const uint8_t *pcm,size_t bytes,uint8_t *out,size_t capacity,size_t *written)
{
    if(!bytes||(bytes&1)||bytes>48000||capacity<44+bytes||
       !disjoint(pcm,bytes,out,capacity)||!disjoint(pcm,bytes,written,sizeof(*written))||
       !disjoint(out,capacity,written,sizeof(*written)))return -1;
    memcpy(out,"RIFF",4);put32(out+4,(uint32_t)(36+bytes));memcpy(out+8,"WAVEfmt ",8);
    put32(out+16,16);put16(out+20,1);put16(out+22,1);put32(out+24,16000);put32(out+28,32000);
    put16(out+32,2);put16(out+34,16);memcpy(out+36,"data",4);put32(out+40,(uint32_t)bytes);
    memcpy(out+44,pcm,bytes);*written=44+bytes;return 1;
}
