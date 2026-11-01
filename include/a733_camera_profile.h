#ifndef SUNSTONE_A733_CAMERA_PROFILE_H
#define SUNSTONE_A733_CAMERA_PROFILE_H
#include <stdint.h>
#include "camera_sensor.h"
#include "sunstone_board.h"
#ifndef SUNSTONE_CAMERA_SIZE
#define SUNSTONE_CAMERA_SIZE 3
#endif
#ifndef SUNSTONE_CAMERA_WIDTH
#define SUNSTONE_CAMERA_WIDTH 640u
#endif
#ifndef SUNSTONE_CAMERA_HEIGHT
#define SUNSTONE_CAMERA_HEIGHT 480u
#endif
#define A733_CAMERA_WIDTH SUNSTONE_CAMERA_WIDTH
#define A733_CAMERA_HEIGHT SUNSTONE_CAMERA_HEIGHT
#define A733_CAMERA_NATIVE_PLANE_BYTES (A733_CAMERA_WIDTH*A733_CAMERA_HEIGHT)
#define A733_CAMERA_NATIVE_GUARD_BYTES 4096u
#define A733_CAMERA_PAGE_ROUND(n) (((n)+4095u)&~4095u)
#define A733_CAMERA_SLOT_BYTES (A733_CAMERA_PAGE_ROUND(A733_CAMERA_NATIVE_PLANE_BYTES)+4096u)
#define A733_CAMERA_TABLE_BYTES (8192u+4u*A733_CAMERA_SLOT_BYTES>16777216u?36864u:32768u)
#define A733_CAMERA_NATIVE_BYTES (A733_CAMERA_TABLE_BYTES+8192u+2u*A733_CAMERA_SLOT_BYTES)
#define A733_CAMERA_NATIVE_STREAM_BYTES (A733_CAMERA_TABLE_BYTES+8192u+4u*A733_CAMERA_SLOT_BYTES)
#define A733_CAMERA_FRAME_BYTES (2u*A733_CAMERA_NATIVE_PLANE_BYTES)
#ifdef SUNSTONE_CAMERA_STREAM
#define A733_CAMERA_IS_STREAM 1u
#define A733_CAMERA_SOURCE_BYTES A733_CAMERA_NATIVE_STREAM_BYTES
#else
#define A733_CAMERA_IS_STREAM 0u
#define A733_CAMERA_SOURCE_BYTES A733_CAMERA_NATIVE_BYTES
#endif
#define A733_CAMERA_LARGE_MEMORY (A733_CAMERA_SOURCE_BYTES+A733_CAMERA_PAGE_ROUND(A733_CAMERA_FRAME_BYTES)>2097152u)
#define A733_CAMERA_POOL_START UINT64_C(0x48000000)
#define A733_CAMERA_POOL_END UINT64_C(0x4a000000)
#define A733_CAMERA_PROFILE_MAGIC UINT32_C(0x43414d50)
#define A733_CAMERA_NV16 UINT32_C(0x3631564e)
struct a733_camera_image_profile {
    uint32_t magic,version,size,width,height,input_format,output_format,stream;
    uint32_t plane,slot,table,source,output,bytes;
    uint64_t pool_start,pool_end;
};
extern const struct a733_camera_image_profile a733_camera_image_profile;
/* Derive timing from the existing sensor implementation; dimensions must agree
 * before the native allocation or the image's CPU mappings are constructed. */
static inline int a733_camera_selected_mode_for_port(struct camera_mode *m,unsigned port)
{
    const struct sunstone_board_description *board=sunstone_board_orangepi_zero3w();
    if(!board || port>=board->camera_count)return 0;
    for(size_t i=0;i<board->camera_sensor_count;i++){
        const struct camera_sensor_driver *d=camera_sensor_find(board->camera_sensors[i]);
        if(!d)continue;
        for(unsigned index=0;index<d->mode_count;index++){
            if(camera_sensor_mode(d,index,CAMERA_UYVY,0,m)==1 &&
               m->width==A733_CAMERA_WIDTH && m->height==A733_CAMERA_HEIGHT &&
               m->xclk_hz==board->camera[port].xclk_hz && m->lanes==board->camera[port].lanes)return 1;
        }
    }
    return 0;
}
static inline int a733_camera_selected_mode(struct camera_mode *m)
{return a733_camera_selected_mode_for_port(m,0);}
#endif
