#include "pvr_compute_package.h"
#include "pvr_firmware.h"
static uint32_t word(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
unsigned pvr_compute_package_temps(const struct pvr_compute_package *p)
{return p->precomp[0];}
const unsigned char *pvr_compute_package_code(const struct pvr_compute_package *p)
{return p->profile==3?p->grid.code:p->code;}
int pvr_compute_package_input_valid(const struct pvr_compute_package *p,const void *input,size_t bytes)
{
    if(!p||!input||bytes!=p->input_bytes||bytes<4||bytes>PVR_COMPUTE_PACKAGE_INPUT)return 0;
    const unsigned char *data=input;
    if(word(data)!=p->count)return 0;
    return p->profile!=3||(bytes>=12&&word(data+4)==p->grid.global_extent[0]&&
        word(data+8)==p->grid.global_extent[1]);
}
int pvr_compute_package_valid(const struct pvr_compute_package *p)
{
    if(!p||(uintptr_t)p%_Alignof(struct pvr_compute_package)||
       p->magic!=PVR_COMPUTE_PACKAGE_MAGIC||p->bytes!=sizeof(*p)||
       !((p->version==1&&p->profile==1)||(p->version==2&&p->profile==2)||(p->version==3&&p->profile==3))||
       p->bvnc!=PVR_FW_A733_BVNC||!p->code_bytes||p->code_bytes>PVR_COMPUTE_PACKAGE_CODE-(p->profile==3?32u:0u)||
       (p->code_bytes&3)||p->input_bytes<4||p->input_bytes>PVR_COMPUTE_PACKAGE_INPUT||
       !p->output_bytes||p->output_bytes>PVR_COMPUTE_PACKAGE_OUTPUT||(p->output_bytes&3)||
       !p->count||!pvr_compute_package_input_valid(p,p->input,p->input_bytes)||p->entry_offset||p->scratch||p->spilled_temps||
       p->unsupported||p->local_size[2]!=1||
       (p->profile==3?(p->precomp[1]!=5||p->precomp[2]!=7):
           (p->precomp[1]>(p->profile==2?6:4)||p->precomp[2]))||
       p->precomp[3]||p->precomp[4]||p->precomp[7]||
       ((unsigned)p->precomp[5]|(unsigned)p->precomp[6]<<8)!=p->code_bytes/4)return 0;
    /* DOUTU uses a six-bit count of four-register temporary blocks. */
    if((pvr_compute_package_temps(p)+3)/4>63)return 0;
    unsigned source=0,compiler=0;
    for(unsigned i=0;i<32;++i){source|=p->source_digest[i];compiler|=p->compiler_digest[i];}
    if(!source||!compiler)return 0;
    if(p->profile!=3&&(p->local_size[0]!=32||p->local_size[1]!=1))return 0;
    if(p->profile==3) {
        const uint32_t *g=p->grid.groups,*n=p->grid.global_extent,*l=p->local_size;
        if(p->grid.sysval_abi!=1||p->grid.reserved||!n[0]||!n[1]||n[2]!=1||
           !l[0]||l[0]>32||!l[1]||l[1]>32||l[0]*l[1]!=32||
           p->count>992||n[0]>p->count/n[1]||n[0]*n[1]!=p->count||
           g[0]!=(n[0]+l[0]-1)/l[0]||g[1]!=(n[1]+l[1]-1)/l[1]||g[2]!=1||
           !g[0]||!g[1]||g[0]>31/g[1]||p->dispatch_count!=1||p->tile_elements!=32||
           p->input_header_bytes<12)return 0;
    } else if(p->profile==1) {
        if(p->dispatch_count||p->tile_elements)return 0;
    } else if(p->tile_elements!=PVR_COMPUTE_DISPATCH_LANES||
        p->count>PVR_COMPUTE_DISPATCH_ELEMENTS||
        p->dispatch_count!=(p->count+PVR_COMPUTE_DISPATCH_LANES-1)/PVR_COMPUTE_DISPATCH_LANES||
        !p->dispatch_count||p->dispatch_count>PVR_COMPUTE_DISPATCH_MAX)return 0;
    if(p->input_header_bytes<4||p->input_header_bytes>p->input_bytes||
       !p->input_element_bytes||!p->output_element_bytes||
       p->count>(p->input_bytes-p->input_header_bytes)/p->input_element_bytes||
       p->count>p->output_bytes/p->output_element_bytes||
       p->input_bytes-p->input_header_bytes!=p->count*p->input_element_bytes||
       p->output_bytes!=p->count*p->output_element_bytes)return 0;
    const unsigned char *code=pvr_compute_package_code(p);
    for(unsigned i=p->code_bytes;i<sizeof(p->code)-(p->profile==3?32u:0u);++i)if(code[i])return 0;
    for(unsigned i=p->input_bytes;i<sizeof(p->input);++i)if(p->input[i])return 0;
    return 1;
}
