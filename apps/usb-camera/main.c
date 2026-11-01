/* Checksum the first frame in place, then release its retained USB slices. */
#include "usb_camera-app.h"
void usb_camera_main(const struct ss_grant *grant)
{
    (void)grant;struct ss_usb_camera_frame frame;
    for(;;){int r=ss_usb_camera_acquire(&frame);if(r<0)ss_fail(1);if(r==1)break;ss_progress();ss_usb_wait(20000);}
    uint32_t checksum=2166136261u;size_t bytes=0;
    for(size_t i=0;i<frame.slice_count;++i){
        const struct ss_usb_frame_slice *slice=&frame.slices[i];
        for(size_t n=0;n<slice->bytes;++n)checksum=(checksum^slice->data[n])*16777619u;
        bytes+=slice->bytes;ss_progress();
    }
    if(bytes!=frame.bytes)ss_fail(2);
#ifdef SUNSTONE_A733_OS_USB_JPEG
    if(frame.format==SS_USB_FRAME_MJPEG){
        struct ss_usb_decoded_frame decoded;
        for(;;){int r=ss_usb_camera_decode(frame.token,&decoded);if(r<0)ss_fail(3);if(r==1)break;ss_progress();ss_usb_wait(20000);}
        checksum=2166136261u;
        for(size_t n=0;n<decoded.bytes;++n)checksum=(checksum^decoded.data[n])*16777619u;
        bytes=decoded.bytes;
    }
#endif
    if(ss_usb_camera_release(frame.token)!=1)ss_fail(2);
    uint64_t result[4]={frame.sequence,bytes,checksum,((uint64_t)frame.width<<32)|frame.height};
    ss_finish(result,sizeof(result));
}
