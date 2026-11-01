/* Generic application link fixture. Input is a 44-byte container header and
 * 320 mono S16LE frames. Replay never opens a physical capture device. */
#include "sunstone_app.h"
#include "audio_replay-app.h"
#include "../apps/os/capture.h"
void audio_replay_main(const struct ss_grant *grant)
{
    if(grant->devices||!ss_capture_replay_open(0,44,160,1))ss_fail(0xa01);
    uint32_t a=1,b=0,frames=0;
    for(uint32_t sequence=1;sequence<=2;++sequence){
        void *pcm;size_t bytes;int r;
        while(!(r=ss_capture(sequence,&pcm,&bytes))){ss_progress();ss_usb_wait(1000);}
        if(r!=1||bytes!=320)ss_fail(0xa02);
        const uint8_t *p=pcm;
        for(size_t i=0;i<bytes;++i){a=(a+p[i])%65521;b=(b+a)%65521;}
        frames+=bytes/2;
    }
    const uint32_t result[]={frames,(b<<16)|a};ss_finish(result,sizeof(result));
}
