/* The capture client of a baseline revision, before the capture stream and
 * the recognizer's WAV adapter separated, against the same modeled DMA
 * service as checks/os-image-audio.c. checks/os-image-audio.py writes that
 * revision's apps/os/audio.c and apps/os/os.h into a directory that it puts
 * first on the quoted include path, and compares the two WAV outputs. */
#include "audio.c"
#include "os-image-audio-model.h"
uint8_t os_check_wav[2][44+2*OS_AUDIO_MAX_SAMPLES];
static int run(unsigned generation)
{
    for(unsigned n=0;n<2000;++n){
        if(atomic_load(&audio.captured)==generation)return 1;
        if(step()<0)return -1;
    }
    return 0;
}
void os_check_run(unsigned selected,unsigned samples)
{
    mode=selected;audio.samples=samples;phase=DMA_SERVICE_CLOSED;
    for(unsigned i=0;i<PERIODS;++i)audio.pool.stream_mappings[i].cpu=memory[i];
    atomic_store(&audio.pool.state,A733_DMA_POOL_READY);
    for(unsigned g=1;g<=2;++g){
        const uint8_t *wav;size_t bytes;
        check(!os_audio_capture(g,&wav,&bytes)&&run(g)==1,20+g);
        check(os_audio_capture(g,&wav,&bytes)==1&&bytes==44+2*samples,30+g);
        memcpy(os_check_wav[g-1],wav,bytes);
    }
    os_check_result=1;
}
