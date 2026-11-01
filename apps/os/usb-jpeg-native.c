#include "../jpeg/jpeg.h"
#include "sunstone_usb_jpeg.h"
struct jpeg_application jpeg;
#define JPEG_NATIVE_CAMERA 1
#include "../jpeg/native-shared.h"
static unsigned native_started,native_failed;
static uint64_t native_frame;
/* Called only by the serialized OS camera owner while retaining the immutable
 * frame. The typed session is independently authenticated by OS admission;
 * camera contents acquire a runtime digest, never a fabricated fixture grant. */
int ss_usb_jpeg_native_begin(const struct a733_jpeg_session *session,
                             const struct ss_usb_jpeg_frame *frame)
{
    if(native_started||!session||!frame||!frame->token||!frame->slices||
       !frame->slice_count||frame->slice_count>1024||!frame->bytes||
       frame->bytes>JPEG_INPUT_CAPACITY||!board_live(NULL))return 0;
    size_t total=0;
    for(size_t i=0;i<frame->slice_count;++i){
        if(!frame->slices[i].data||!frame->slices[i].bytes||frame->slices[i].bytes>frame->bytes-total)return 0;
        total+=frame->slices[i].bytes;
    }
    if(total!=frame->bytes)return 0;
    /* Admission must bind these exact board facts, independently of input. */
    if(session->board.epoch!=board.session.epoch||session->board.generation!=board.session.generation||
       session->board.deadline_us!=board.session.deadline_us||!session->owner||!session->authority||
       session->module!=A733_VIDEO_VENDOR_MODULE||session->module_hz!=A733_VIDEO_VENDOR_HZ||
       session->supply_uv!=800000||session->supply_rail!=A733_RAIL_DCDC2||
       session->iommu_gates[0]!=0x10007||session->iommu_gates[1]!=0x10007)return 0;
    const uint8_t *facts[]={session->initial,session->memory,session->electrical,session->supervision};
    for(unsigned i=0;i<4;++i){uint8_t present=0;for(unsigned j=0;j<32;++j)present|=facts[i][j];if(!present)return 0;}
    jpeg.session=*session;native_started=1;native_frame=frame->token;
    jpeg.pool=a733_memory_uncached_init();if(!jpeg.pool)return -1;
    struct device_memory_extent extent;
    if(!device_memory_acquire_aligned(jpeg.pool,session->owner,JPEG_INPUT_CAPACITY+8192,4096,
       &jpeg.leases[1],&extent))return -1;
    memset((void *)extent.cpu,0xa5,extent.bytes);jpeg.input=(void *)(extent.cpu+4096);
    memset(jpeg.input,0,JPEG_INPUT_CAPACITY);total=0;
    for(size_t i=0;i<frame->slice_count;++i){
        memcpy(jpeg.input+total,frame->slices[i].data,frame->slices[i].bytes);total+=frame->slices[i].bytes;
    }
    struct a733_video_jpeg picture;
    if(!a733_video_jpeg_parse(jpeg.input,total,&picture)||picture.width!=frame->width||
       picture.height!=frame->height||!jpeg_input_geometry(&jpeg.supplied,&picture))return -1;
    jpeg.supplied.input_bytes=total;jpeg.supplied.sequence=1;
    jpeg.supplied.mapped[0]=JPEG_INPUT_CAPACITY;
    jpeg.supplied.mapped[1]=(jpeg.supplied.luma_bytes+4095u)&~4095u;
    jpeg.supplied.mapped[2]=(jpeg.supplied.chroma_bytes+4095u)&~4095u;
    storage_digest_bytes(jpeg.input,total,jpeg.supplied.input_hash);
    atomic_store_explicit(&jpeg.supplied.committed,1,memory_order_release);
    return 1;
}
int ss_usb_jpeg_native_poll(struct ss_usb_jpeg_view *view)
{
    if(!native_started||native_failed||!view||!board_live(NULL))return -1;
    if(!jpeg_input_ready(&jpeg.supplied))return -1;
    if(!jpeg.stage){int r=jpeg_setup();if(r==1)jpeg.stage=1;if(r<0)native_failed=1;return r<0?-1:0;}
    /* Complete ready register programming in this wake. A pending hardware
     * completion or copy callback returns immediately to the USB owner. */
    int r=a733_video_jpeg_advance(&jpeg.run,1024);
    if(r==1&&jpeg.run.state==A733_VIDEO_JPEG_COPIED)
        *view=(struct ss_usb_jpeg_view){jpeg.payload,jpeg.supplied.output_bytes,
            jpeg.supplied.width,jpeg.supplied.height,native_frame};
    return r;
}
