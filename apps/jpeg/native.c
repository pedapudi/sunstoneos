#include "jpeg.h"
struct jpeg_application jpeg;
#include "native-shared.h"
/* A sequence image is published only after the previous result was
 * delivered, so the previous copy was verified: guards intact, no IOMMU
 * fault and no codec error. Under the same retained mappings the next job
 * rewrites the input and restores the poison that the guard and tail checks
 * expect, as H.264 refills its input after a healthy completion. Nothing is
 * unmapped, released or reset. */
static int jpeg_next(void)
{
    if(jpeg_input_ready(&jpeg.supplied)!=jpeg.decoded+1||jpeg.run.state!=A733_VIDEO_JPEG_COPIED)return -1;
    uint32_t input=jpeg_input_mapped(&jpeg.supplied,0);
    memset(jpeg.input,0,input);memcpy(jpeg.input,jpeg.supplied.bytes,jpeg.supplied.input_bytes);
    memset(jpeg.luma,0xa5,jpeg_input_mapped(&jpeg.supplied,1));memset(jpeg.chroma,0xa5,jpeg_input_mapped(&jpeg.supplied,2));
    jpeg.copy_step=0;jpeg.copy_row=0;
    __asm__ volatile("dsb sy":::"memory");
    struct a733_video_jpeg_job job=jpeg_job();
    if(!board_live(NULL)||!a733_video_jpeg_next(&jpeg.run,&job))return -1;
    ++jpeg.decoded;return 0;
}
/* The controller performs at most one register operation per poll. Repeat
 * polls while each one advances its state or index, so programming sequences
 * finish in one wake. Waiting for hardware, a copy step or a busy resource
 * makes no progress and sleeps a tick; the budget bounds one wake's work. */
#define JPEG_POLL_BUDGET 1024u
static int jpeg_advance(void)
{
    for(unsigned n=0;;){
        enum a733_video_jpeg_state state=jpeg.run.state;unsigned index=jpeg.run.index;
        int r=a733_video_jpeg_poll(&jpeg.run);
        if(r||(jpeg.run.state==state&&jpeg.run.index==index)||++n==JPEG_POLL_BUDGET||!board_live(NULL))return r;
    }
}
void jpeg_task(void *unused)
{
    (void)unused;
    for(;;){
        if(!board_live(NULL)){atomic_store_explicit(&jpeg.result,2,memory_order_release);vTaskDelay(1);continue;}
        if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire)&&!atomic_load_explicit(&jpeg.result,memory_order_acquire)){
            if(!jpeg_input_ready(&jpeg.supplied)){vTaskDelay(1);continue;}
            /* COPIED with a pending result means the USB task published the
             * next sequence image after delivering this one. */
            int r=!jpeg.stage?jpeg_setup():jpeg.run.state==A733_VIDEO_JPEG_COPIED?jpeg_next():jpeg_advance();
            if(r<0)atomic_store_explicit(&jpeg.result,2,memory_order_release);
            else if(!jpeg.stage&&r==1)jpeg.stage=1;
            else if(jpeg.stage&&r==1&&jpeg.run.state==A733_VIDEO_JPEG_COPIED)atomic_store_explicit(&jpeg.result,1,memory_order_release);
        }
        vTaskDelay(1);
    }
}
