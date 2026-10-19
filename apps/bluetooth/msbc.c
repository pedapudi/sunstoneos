#include "msbc.h"
#include <sbc/sbc.h>
#include <string.h>
static struct {
    sbc_t decoder,encoder;
    uint8_t frame[60],silence[60];
    uint64_t acquired[60];
    unsigned ready,used,discarded,locked,sequence,tx_sequence;
} codec;
static const uint8_t sequence[4]={0x08,0x38,0xc8,0xf8};
int bluetooth_msbc_init(void)
{
    if(codec.ready)return 0;
    if(sbc_init_msbc(&codec.decoder,0))return 0;
    if(sbc_init_msbc(&codec.encoder,0)){sbc_finish(&codec.decoder);return 0;}
    codec.decoder.endian=codec.encoder.endian=SBC_LE;
    uint8_t zero[240]={0};ssize_t bytes=0;
    if(sbc_encode(&codec.encoder,zero,sizeof(zero),codec.silence+2,57,&bytes)!=240||bytes!=57){
        sbc_finish(&codec.decoder);sbc_finish(&codec.encoder);return 0;}
    codec.silence[0]=1;codec.ready=1;return 1;
}
int bluetooth_msbc_encode(const uint8_t pcm[240],uint8_t out[60])
{
    if(!codec.ready||!pcm||!out)return 0;ssize_t bytes=0;
    if(sbc_encode(&codec.encoder,pcm,240,out+2,57,&bytes)!=240||bytes!=57)return 0;
    out[0]=1;out[1]=sequence[codec.tx_sequence];out[59]=0;
    codec.tx_sequence=(codec.tx_sequence+1)&3;return 1;
}
int bluetooth_msbc_silence(uint8_t out[60])
{
    if(!codec.ready||!out)return 0;
    memcpy(out,codec.silence,60);out[1]=sequence[codec.tx_sequence];
    codec.tx_sequence=(codec.tx_sequence+1)&3;return 1;
}
int bluetooth_msbc_byte(uint8_t byte,uint64_t acquired,uint8_t out[240],uint64_t *first)
{
    if(!codec.ready||!acquired||!out||!first||codec.used>=60)return -1;
    codec.frame[codec.used]=byte;codec.acquired[codec.used++]=acquired;
    if(codec.used<60)return 0;
    unsigned seq=4;for(unsigned i=0;i<4;++i)if(codec.frame[1]==sequence[i])seq=i;
    size_t written=0;
    unsigned candidate=codec.frame[0]==1&&seq<4&&codec.frame[59]==0;
    if(codec.frame[0]==1&&seq<4&&(!codec.locked||seq==codec.sequence)&&codec.frame[59]==0&&
       sbc_decode(&codec.decoder,codec.frame+2,57,out,240,&written)==57&&written==240){
        *first=codec.acquired[0];codec.used=0;codec.locked=1;codec.sequence=(seq+1)&3;return 1;}
    if(candidate||codec.locked||++codec.discarded>120)return -1;
    /* Before the first header candidate, slide across arbitrary HCI packet
     * alignment without invoking the decoder. A rejected candidate stops; no
     * damaged-frame state is reused or passed through libsbc reinit_msbc. */
    memmove(codec.frame,codec.frame+1,59);memmove(codec.acquired,codec.acquired+1,59*sizeof(uint64_t));codec.used=59;return 0;
}
