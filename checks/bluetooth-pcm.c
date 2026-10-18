#include "../apps/bluetooth/pcm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int value(const uint8_t *p){unsigned x=p[0]|(unsigned)p[1]<<8;return x<32768?(int)x:(int)x-65536;}
static void check(const uint8_t *pcm,size_t n,const int *expected)
{uint8_t out[256];size_t written=0;assert(bluetooth_pcm_wav(pcm,n,out,sizeof(out),&written)==1&&written==44+2*n);
 assert(!memcmp(out,"RIFF",4)&&!memcmp(out+8,"WAVEfmt ",8)&&out[22]==1&&out[23]==0&&out[24]==0x80&&out[25]==0x3e&&out[34]==16);
 for(unsigned i=0;i<n;++i)assert(value(out+44+2*i)==expected[i]);}
int main(void)
{
 const uint8_t extrema[]={0,0x80,0xff,0x7f};const int e[]={-32768,0,32767,32767};check(extrema,4,e);
 const uint8_t negative[]={0xff,0xff,0,0};const int n[]={-1,0,0,0};check(negative,4,n);
 const uint8_t ramp[]={0xfc,0xff,0,0,4,0};const int r[]={-4,-2,0,2,4,4};check(ramp,6,r);
 const uint8_t single[]={0x34,0x12};const int s[]={0x1234,0x1234};check(single,2,s);
 static uint8_t pcm[48000],wav[BLUETOOTH_WAV_BYTES+1];for(unsigned i=0;i<48000;i+=2){pcm[i]=0x61;pcm[i+1]=0x1e;}
 size_t written=0;wav[BLUETOOTH_WAV_BYTES]=0xa5;
 assert(bluetooth_pcm_wav(pcm,sizeof(pcm),wav,BLUETOOTH_WAV_BYTES,&written)==1&&written==96044&&wav[96044]==0xa5);
 for(unsigned i=44;i<96044;i+=2)assert(value(wav+i)==7777);
 assert(bluetooth_pcm_wav(pcm,3,wav,sizeof(wav),&written)==-1);
 assert(bluetooth_pcm_wav(pcm,48002,wav,sizeof(wav),&written)==-1);
 assert(bluetooth_pcm_wav(pcm,48000,wav,96043,&written)==-1);
 assert(bluetooth_pcm_wav(pcm,8,pcm,48000,&written)==-1);
 assert(bluetooth_pcm_wav(pcm,0,wav,sizeof(wav),&written)==-1);
 puts("PASS finite8k→16k signed LE interpolation, rounding, endpoints, mono/rate, maximum96044B and invalid extents");
}
