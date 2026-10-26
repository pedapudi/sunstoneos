#include "a733_camera_profile.h"
#include "snapshot.h"
#include "boot.h"
#include <string.h>
extern unsigned char __device_pool_start[],__device_pool_end[];
extern unsigned char __uncached_pool_start[],__uncached_pool_end[];
#define PROFILE {A733_CAMERA_PROFILE_MAGIC,1,SUNSTONE_CAMERA_SIZE,A733_CAMERA_WIDTH,\
 A733_CAMERA_HEIGHT,OV5640_UYVY,A733_CAMERA_NV16,A733_CAMERA_IS_STREAM,\
 A733_CAMERA_NATIVE_PLANE_BYTES,A733_CAMERA_SLOT_BYTES,A733_CAMERA_TABLE_BYTES,\
 A733_CAMERA_SOURCE_BYTES,A733_CAMERA_FRAME_BYTES,sizeof(struct a733_camera_image_profile),\
 (uintptr_t)__device_pool_start,(uintptr_t)__device_pool_end}
const struct a733_camera_image_profile a733_camera_image_profile=PROFILE;
int camera_memory_profile_valid(void)
{
    /* Volatile reads keep the admission check independent of the compiler's
     * constant folding and merging of identical readonly initializers. */
    const volatile struct a733_camera_image_profile *p=&a733_camera_image_profile;
    struct ov5640_mode mode;
    if(!a733_camera_selected_mode(&mode) || p->magic!=A733_CAMERA_PROFILE_MAGIC || p->version!=1 ||
       p->size!=SUNSTONE_CAMERA_SIZE || p->width!=mode.width || p->height!=mode.height ||
       p->input_format!=OV5640_UYVY || p->output_format!=A733_CAMERA_NV16 ||
       p->stream!=A733_CAMERA_IS_STREAM || p->plane!=A733_CAMERA_NATIVE_PLANE_BYTES ||
       p->slot!=A733_CAMERA_SLOT_BYTES || p->table!=A733_CAMERA_TABLE_BYTES ||
       p->source!=A733_CAMERA_SOURCE_BYTES || p->output!=A733_CAMERA_FRAME_BYTES ||
       p->bytes!=sizeof(*p) || p->pool_start!=(uintptr_t)__device_pool_start ||
       p->pool_end!=(uintptr_t)__device_pool_end ||
       (uintptr_t)__uncached_pool_start!=(uintptr_t)__uncached_pool_end)return 0;
    uintptr_t first=(uintptr_t)__device_pool_start,last=(uintptr_t)__device_pool_end;
#if A733_CAMERA_LARGE_MEMORY
    return first==A733_CAMERA_POOL_START && last==A733_CAMERA_POOL_END;
#else
    return first>=0x41200000 && last<=0x41f00000 && last>first &&
        last-first==2097152 && !((first|last)&4095);
#endif
}
int a733_camera_memory_admitted(void)
{
    const struct camera_session *s=camera_admitted_session();
    return s && camera_memory_profile_valid() && !memcmp(s->image,a733_smp_build_id,32);
}
