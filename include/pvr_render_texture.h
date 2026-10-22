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
/* Payload offsets after the render payload: guarded output, texture area,
 * vertex and index areas, the depth extent (profiles 3 and 4), the runtime's
 * 64 KiB upload chunk and the 64 KiB read-back chunk. copy is the frame inside
 * the output guards. The GPU neither loads nor stores depth, and no CPU code
 * touches the depth extent; it remains only because the graphics owner still
 * maps it at PVR_RENDER_DEPTH_ADDRESS, one page after depth. */
struct pvr_render_texture_layout {
    uint32_t output,image,geometry,copy,upload,download,total;
    uint32_t output_bytes,output_allocation;
    uint32_t depth,depth_bytes;
};
enum pvr_render_texture_operation { PVR_TEXTURE_BEGIN, PVR_TEXTURE_WRITE,
    PVR_TEXTURE_SUBMIT, PVR_TEXTURE_READ, PVR_TEXTURE_RELEASE, PVR_TEXTURE_PRESENT };
/* A presented frame lends the GPU's output frame in place to one in-image
 * consumer. Rows are RGBA8 with stride pixels each, including padding. The
 * CPU holds no dirty line of it, so a non-coherent DMA reader may fetch it
 * from physical. It stays immutable until RELEASE: replacement, READ and
 * CLOSE are refused while it is lent. */
struct pvr_render_texture_frame {
    const void *cpu;
    uint64_t physical;
    uint32_t bytes,width,height,stride,frame;
};
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

/* Profile 4 keeps the 24-byte position/UV vertex and RGBA8 texture. It adds
 * up to sixteen state batches per frame, perspective-correct texture
 * coordinates, a frame sampler mode, a packed D32S8 attachment and the fixed
 * blend equations below. Resident programs replace profile-2/3 programs. */
#define PVR_RENDER_BATCH_PROFILE 4u
#define PVR_RENDER_BATCH_MAX 16u
#define PVR_RENDER_BATCH_BYTES 16u
#define PVR_RENDER_BATCH_BLENDS 5u
#define PVR_RENDER_BATCH_PPP_WORDS 26u
#define PVR_RENDER_BATCH_VDM_STATE_BYTES 20u
#define PVR_RENDER_BATCH_VDM_OFFSET 0x11000u
#define PVR_RENDER_BATCH_PPP_OFFSET 0x11200u
#define PVR_RENDER_BATCH_PPP_STRIDE 128u
#define PVR_RENDER_BATCH_VDM_ADDRESS (PVR_RENDER_GENERAL_ADDRESS+PVR_RENDER_BATCH_VDM_OFFSET)
#define PVR_RENDER_BATCH_OPAQUE_USC 2560u
#define PVR_RENDER_BATCH_OPAQUE_PDS 2304u
#define PVR_RENDER_BATCH_BLEND_USC0 3072u
#define PVR_RENDER_BATCH_BLEND_CAP0 512u
#define PVR_RENDER_BATCH_BLEND_PDS0 2560u
#define PVR_RENDER_BATCH_BLEND_USC1 3584u
#define PVR_RENDER_BATCH_BLEND_CAP1 512u
#define PVR_RENDER_BATCH_BLEND_PDS1 2688u
#define PVR_RENDER_BATCH_BLEND_USC2 1344u
#define PVR_RENDER_BATCH_BLEND_CAP2 448u
#define PVR_RENDER_BATCH_BLEND_PDS2 2816u
#define PVR_RENDER_BATCH_BLEND_USC3 1920u
#define PVR_RENDER_BATCH_BLEND_CAP3 384u
#define PVR_RENDER_BATCH_BLEND_PDS3 2944u
/* Frame state on BEGIN: bits 0-4 batch count (1-16), bit 5 linear filtering,
 * bits 6-8 and 9-11 VkSamplerAddressMode for U and V (0-4), bit 12 sRGB
 * decode. Other bits are zero. */
#define PVR_RENDER_BATCH_LINEAR (1u<<5)
#define PVR_RENDER_BATCH_SRGB (1u<<12)
#define PVR_RENDER_BATCH_SRGB_PACK 287u
/* Batch state: bits 0-2 VkCompareOp for depth, bit 3 depth write, bits 4-5
 * cull (0 none, 1 clockwise, 2 counter-clockwise in framebuffer coordinates),
 * bits 6-8 blend (0 opaque, 1 straight source-over, 2 premultiplied
 * source-over, 3 additive, 4 multiply), bit 9 color writes disabled (opaque
 * only), bit 10 stencil test, bits 11-13 stencil VkCompareOp and bits 14-16,
 * 17-19 and 20-22 VkStencilOp for stencil fail, depth fail and pass. The
 * stencil word holds reference, compare mask and write mask in bytes 0-2. */
#define PVR_RENDER_BATCH_DEPTH_WRITE (1u<<3)
#define PVR_RENDER_BATCH_NO_COLOR (1u<<9)
#define PVR_RENDER_BATCH_STENCIL (1u<<10)
/* Canonical profile-4 input: vertices, indices, batch records, texels. Each
 * record draws count indices from first with its own state. */
struct pvr_render_batch { uint32_t first,count,state,stencil; };
int pvr_render_batch_frame_valid(uint32_t frame);
uint32_t pvr_render_batch_table_bytes(uint32_t profile,uint32_t frame);
int pvr_render_batch_valid(const struct pvr_render_batch *,uint32_t index_count);
int pvr_render_batch_input_valid(const struct pvr_render_texture_input *,uint32_t frame,const void *vertices,
    const void *indices,const void *batches,size_t texture_bytes);
int pvr_render_batch_descriptor(void *,const struct pvr_render_texture_input *,uint32_t frame);
/* Write per-batch PPP blocks and the VDM stream into the program payload's
 * general region. The caller cleans and publishes them before submission. */
int pvr_render_batch_commands(void *payload,uint32_t width,uint32_t height,uint32_t frame,
    const void *batches,uint32_t index_count);
int pvr_render_batch_depth_job(uint32_t,uint32_t,struct pvr_render_job_image *);
#endif
