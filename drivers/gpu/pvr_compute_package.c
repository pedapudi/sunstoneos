#include "pvr_compute_package.h"
#include "pvr_firmware.h"
static uint32_t word(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
unsigned pvr_compute_package_temps(const struct pvr_compute_package *p)
{return p->precomp[0];}
int pvr_compute_package_valid(const struct pvr_compute_package *p)
{
    if(!p||(uintptr_t)p%_Alignof(struct pvr_compute_package)||
       p->magic!=PVR_COMPUTE_PACKAGE_MAGIC||p->version!=1||p->bytes!=sizeof(*p)||p->profile!=1||
       p->bvnc!=PVR_FW_A733_BVNC||!p->code_bytes||p->code_bytes>PVR_COMPUTE_PACKAGE_CODE||
       (p->code_bytes&3)||p->input_bytes<4||p->input_bytes>PVR_COMPUTE_PACKAGE_INPUT||
       !p->output_bytes||p->output_bytes>PVR_COMPUTE_PACKAGE_OUTPUT||(p->output_bytes&3)||
       !p->count||word(p->input)!=p->count||p->entry_offset||p->scratch||p->spilled_temps||
       p->unsupported||p->local_size[0]!=32||p->local_size[1]!=1||p->local_size[2]!=1||p->reserved0||
       p->precomp[1]>4||p->precomp[2]||p->precomp[3]||p->precomp[4]||p->precomp[7]||
       ((unsigned)p->precomp[5]|(unsigned)p->precomp[6]<<8)!=p->code_bytes/4)return 0;
    /* DOUTU uses a six-bit count of four-register temporary blocks. */
    if((pvr_compute_package_temps(p)+3)/4>63)return 0;
    unsigned source=0,compiler=0;
    for(unsigned i=0;i<32;++i){source|=p->source_digest[i];compiler|=p->compiler_digest[i];}
    if(!source||!compiler)return 0;
    if(p->reserved||p->input_header_bytes<4||p->input_header_bytes>p->input_bytes||
       !p->input_element_bytes||!p->output_element_bytes||
       p->count>(p->input_bytes-p->input_header_bytes)/p->input_element_bytes||
       p->count>p->output_bytes/p->output_element_bytes||
       p->input_bytes-p->input_header_bytes!=p->count*p->input_element_bytes||
       p->output_bytes!=p->count*p->output_element_bytes)return 0;
    for(unsigned i=p->code_bytes;i<sizeof(p->code);++i)if(p->code[i])return 0;
    for(unsigned i=p->input_bytes;i<sizeof(p->input);++i)if(p->input[i])return 0;
    return 1;
}
