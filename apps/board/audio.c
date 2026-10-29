#include "audio.h"
#include <string.h>
static void put32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n)
{put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
int board_audio_encode(uint8_t out[BOARD_AUDIO_BYTES],const struct a733_usb2_audio_block *b)
{
 if(!out||!b||!b->generation||!b->sequence||b->rate!=16000||!b->frames||b->frames>512||
    b->channels!=1||b->sample_bytes!=2||!b->address||b->address>127||b->interface>=16||
    (b->endpoint&~0x8f)||!(b->endpoint&128)||!(b->endpoint&15)||
    (b->speed!=1&&b->speed!=3)||b->reserved[0]||b->reserved[1])return 0;
 memset(out,0,BOARD_AUDIO_BYTES);
 put32(out,UINT32_C(0x31445541));put32(out+4,1);put32(out+8,BOARD_AUDIO_BYTES);put32(out+12,b->frames*2);
 put64(out+16,b->generation);put64(out+24,b->sequence);
 put32(out+32,b->rate);put32(out+36,b->frames);put32(out+40,b->dropped);put32(out+44,b->overruns);put32(out+48,b->overwritten);
 out[52]=b->address;out[53]=b->interface;out[54]=b->endpoint;out[55]=b->speed;out[56]=b->channels;out[57]=b->sample_bytes;
 memcpy(out+64,b->data,b->frames*2);return 1;
}
