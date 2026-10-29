/* SPDX-License-Identifier: Apache-2.0 */
#include "a2dp.h"
#include <sbc/sbc.h>
static struct {
    sbc_t decoder;
    _Alignas(16) uint8_t arena[6144];
    unsigned allocated,ready,failed,rate,channels;
    uint64_t media;
} capture;
void *bluetooth_a2dp_alloc(size_t bytes)
{
    if(capture.allocated||!bytes||bytes>sizeof(capture.arena))return NULL;
    capture.allocated=1;return capture.arena;
}
void bluetooth_a2dp_free(void *memory)
{
    if(memory!=capture.arena||!capture.allocated){capture.failed=1;return;}
    capture.allocated=0;
}
int bluetooth_a2dp_init(void)
{
    if(capture.ready||capture.failed||sbc_init(&capture.decoder,0))return 0;
    capture.decoder.endian=SBC_LE;capture.ready=1;return 1;
}
unsigned bluetooth_a2dp_rate(void){return capture.rate;}
unsigned bluetooth_a2dp_channels(void){return capture.channels;}
int bluetooth_a2dp_decode(struct bt_avdtp *sink,uint8_t pcm[BLUETOOTH_PCM_BYTES],unsigned target,
    struct bluetooth_pcm_snapshot *snapshot,struct bluetooth_pcm_record *record,uint64_t acquired)
{
    if(!capture.ready||capture.failed||!sink||!pcm||!snapshot||!record||!acquired||
       !target||target>BLUETOOTH_PCM_BYTES||(target&3)||snapshot->bytes>target||
       sink->error||sink->invalid||sink->dropped||sink->gaps)return -1;
    const struct bt_avdtp_media *packet=bt_avdtp_media(sink);
    if(!packet)return 0;
    if(!packet->frames||packet->frames>15||packet->gap||!sink->media||
       (capture.media&&capture.media!=sink->media)||snapshot->records>=1500)return -1;
    capture.media=sink->media;
    unsigned offset=0,before=snapshot->bytes;
    for(unsigned frame=0;frame<packet->frames&&snapshot->bytes<target;++frame){
        if(packet->length-offset<4)return -1;
        const uint8_t *bytes=packet->bytes+offset;
        static const unsigned rates[4]={16000,32000,44100,48000};
        unsigned rate=rates[bytes[1]>>6],channels=(bytes[1]&12)?2:1;
        if((capture.rate&&capture.rate!=rate)||(capture.channels&&capture.channels!=channels))return -1;
        size_t written=0;
        ssize_t consumed=sbc_decode(&capture.decoder,bytes,packet->length-offset,
            pcm+snapshot->bytes,target-snapshot->bytes,&written);
        if(consumed<=0||(size_t)consumed>packet->length-offset||!written||written>512||
           written>target-snapshot->bytes||written%(channels*2))return -1;
        capture.rate=rate;capture.channels=channels;
        offset+=(unsigned)consumed;snapshot->bytes+=(unsigned)written;
    }
    if(snapshot->bytes<target&&offset!=packet->length)return -1;
    *record=(struct bluetooth_pcm_record){acquired,before,(uint16_t)packet->length,
        (uint16_t)(snapshot->bytes-before),0,(capture.rate<<8)|capture.channels};
    ++snapshot->records;
    snapshot->audio=capture.media;
    if(bt_avdtp_release_media(sink,packet->serial)!=1)return -1;
    return 1;
}
