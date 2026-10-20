#ifndef SUNSTONE_PVR_RENDER_TEXTURE_H
#define SUNSTONE_PVR_RENDER_TEXTURE_H
#include "pvr_render_draw.h"
/* Separate trusted shader/profile ABI; existing draw bytes retain profile 1. */
#define PVR_RENDER_TEXTURE_PROFILE 2u
#define PVR_RENDER_DEPTH_BLEND_PROFILE 3u
#define PVR_RENDER_DEPTH_BYTES (16u*1024u*1024u)
#define PVR_RENDER_DEPTH_ADDRESS UINT64_C(0x800d000000)
/* State bits: compare OFF/LESS/LEQUAL/ALWAYS, depth write, straight source-over. */
#define PVR_RENDER_DEPTH_WRITE 4u
#define PVR_RENDER_SOURCE_OVER 8u
#define PVR_RENDER_TEXTURE_MAX_WIDTH 1920u
#define PVR_RENDER_TEXTURE_MAX_HEIGHT 1080u
#define PVR_RENDER_TEXTURE_MAX_PIXELS (1920u*1080u)
#define PVR_RENDER_TEXTURE_MAX_BYTES (PVR_RENDER_TEXTURE_MAX_PIXELS*4u)
#define PVR_RENDER_TEXTURE_VERTICES 65536u
#define PVR_RENDER_TEXTURE_VERTEX_BYTES (PVR_RENDER_TEXTURE_VERTICES*24u)
#define PVR_RENDER_TEXTURE_INDICES 65535u
#define PVR_RENDER_TEXTURE_INDEX_BYTES 131072u
#define PVR_RENDER_TEXTURE_GEOMETRY_BYTES (PVR_RENDER_TEXTURE_VERTEX_BYTES+PVR_RENDER_TEXTURE_INDEX_BYTES)
#define PVR_RENDER_TEXTURE_CHUNK 65536u
#define PVR_RENDER_TEXTURE_OUTPUT_ADDRESS UINT64_C(0x800a000040)
#define PVR_RENDER_TEXTURE_IMAGE_ADDRESS UINT64_C(0x800b000000)
#define PVR_RENDER_TEXTURE_VERTEX_ADDRESS UINT64_C(0x800c000000)
#define PVR_RENDER_TEXTURE_INDEX_ADDRESS (PVR_RENDER_TEXTURE_VERTEX_ADDRESS+PVR_RENDER_TEXTURE_VERTEX_BYTES)
#define PVR_RENDER_TEXTURE_DESCRIPTOR_ADDRESS (PVR_RENDER_GENERAL_ADDRESS+UINT64_C(0x10100))
#define PVR_RENDER_TEXTURE_COUNT_OFFSET 0xf024u
struct pvr_render_texture_vertex { float position[4],uv[2]; };
/* Row strides are in pixels and include padding. Geometry is immutable for the
 * complete draw; all indices must name a supplied vertex. */
struct pvr_render_texture_input {
    uint32_t width,height,stride,vertex_count,index_count;
};
struct pvr_render_texture_layout {
    uint32_t output,image,geometry,draft,copy,upload,download,total;
    uint32_t output_bytes,output_allocation;
    uint32_t depth;
};
enum pvr_render_texture_operation { PVR_TEXTURE_BEGIN, PVR_TEXTURE_WRITE,
    PVR_TEXTURE_SUBMIT, PVR_TEXTURE_READ, PVR_TEXTURE_RELEASE };
struct pvr_render_texture_request {
    uint64_t generation;
    uint32_t operation,previous,offset,bytes;
    struct pvr_render_texture_input input;
    uint32_t state;
};
int pvr_render_texture_layout(uint32_t,uint32_t,struct pvr_render_texture_layout *);
int pvr_render_texture_layout_profile(uint32_t,uint32_t,uint32_t,struct pvr_render_texture_layout *);
int pvr_render_texture_state_valid(uint32_t profile,uint32_t state);
int pvr_render_texture_input_valid_profile(uint32_t,const struct pvr_render_texture_input *,
    const void *,const void *,size_t);
int pvr_render_texture_geometry(uint32_t,uint32_t,uint32_t *stride,uint32_t *bytes);
int pvr_render_texture_descriptor(void *,const struct pvr_render_texture_input *);
struct pvr_render_job_image;
int pvr_render_texture_programs_build_profile(void *,size_t,uint32_t,uint32_t,uint32_t,struct pvr_render_program_image *);
int pvr_render_texture_state_apply(void *,uint32_t);
int pvr_render_texture_depth_job(uint32_t,uint32_t,struct pvr_render_job_image *);
int pvr_render_texture_programs_build(void *,size_t,uint32_t,uint32_t,struct pvr_render_program_image *);
int pvr_render_texture_input_valid(const struct pvr_render_texture_input *,const void *vertices,
    const void *indices,size_t texture_bytes);
#endif
