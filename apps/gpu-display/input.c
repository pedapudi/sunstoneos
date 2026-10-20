#include "input.h"
#include "a733_storage_digest.h"
#include <string.h>
int gpu_display_inputs_valid(const struct gpu_display_inputs *s)
{
    if(!s || s->magic!=GPU_DISPLAY_INPUT_MAGIC || s->version!=GPU_DISPLAY_INPUT_VERSION ||
       s->bytes<sizeof(*s) || s->bytes>GPU_DISPLAY_ENVELOPE_LIMIT)return 0;
    uint32_t next=sizeof(*s);
    for(unsigned i=0;i<2;++i){
        const struct gpu_display_input_frame *f=&s->frame[i];
        const struct pvr_render_texture_input *in=&f->input;
        uint32_t stride,bytes;
        if(f->reserved || f->offset!=next || !f->bytes || f->bytes>s->bytes-next ||
           !pvr_render_texture_geometry(in->width,in->height,&stride,&bytes) || in->stride!=stride ||
           !in->vertex_count || in->vertex_count>PVR_RENDER_TEXTURE_VERTICES ||
           !in->index_count || in->index_count>PVR_RENDER_TEXTURE_INDICES || in->index_count%3)return 0;
        uint64_t vertices=(uint64_t)in->vertex_count*sizeof(struct pvr_render_texture_vertex);
        uint64_t indices=(uint64_t)in->index_count*2;
        if(vertices+indices+bytes!=f->bytes)return 0;
        const uint8_t *payload=(const uint8_t *)s+next;
        if(!pvr_render_texture_input_valid(in,payload,payload+vertices,bytes))return 0;
        uint8_t digest[32];storage_digest_bytes(payload,f->bytes,digest);
        if(memcmp(digest,f->digest,32))return 0;
        next+=f->bytes;
    }
    return next==s->bytes;
}
