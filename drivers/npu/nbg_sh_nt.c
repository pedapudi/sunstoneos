#include "nbg_sh_nt.h"
#include "nbg_checksum.h"
#include "nbg_relocation.h"
#include <string.h>

/* Command prefix/footer are the existing SDK preparation profile. INIT is
 * recognized before omission; native cold bootstrap remains a separate command. */
static const uint32_t prefix[]={0x08010e02,0x701,0x48000000,0x701,0x0801022c,0x1f,
    0x08010e03,0xc23,0x08010e02,0x701,0x48000000,0x701};
static const uint32_t footer[]={0x08010e02,0x30000701,0x48000000,0x30000701,
    0x08010e03,0xc23,0x08010e01,0x40,0x10000000,0,0,0,0,0};
static const uint32_t initial[]={0x0801028a,0x11,0x08010e13,2,0x08010e4e,0,
    0x08010e4f,0,0x08010e50,0};
static const uint32_t nt_state[]={0x08010e4c,0x0493e040,0x08010e5a,0x00451440,
    0x08010428,0,0x08010429,0};
static uint32_t word(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static void words(unsigned char *p,const uint32_t *v,unsigned n)
{for(unsigned i=0;i<n;++i)put(p+4*i,v[i]);}
static int range(const void *p,size_t n)
{return p && n && n<=UINTPTR_MAX-(uintptr_t)p;}
static int overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{return an && bn && a<b+bn && b<a+an;}
static int separate(const void *a,size_t an,const void *b,size_t bn)
{return !overlap((uintptr_t)a,an,(uintptr_t)b,bn);}
static int result_space(const struct nbg_model *m,struct nbg_assembly_plan *p)
{return range(m,sizeof(*m)) && range(p,sizeof(*p)) && range(m->data,m->bytes) &&
 separate(p,sizeof(*p),m,sizeof(*m)) && separate(p,sizeof(*p),m->data,m->bytes);}
static int same(const unsigned char *p,const uint32_t *v,unsigned n)
{for(unsigned i=0;i<n;++i)if(word(p+4*i)!=v[i])return 0;return 1;}
static int tensor(const struct nbg_model *m,enum nbg_section_id id,const uint32_t shape[3])
{
 struct nbg_tensor t;
 if(!nbg_model_tensor(m,id,0,&t)||t.dimensions!=3||t.format||t.buffer_type||t.quantization)return 0;
 for(unsigned i=0;i<6;++i)if(t.sizes[i]!=(i<3?shape[i]:1))return 0;
 return 1;
}
/* The fixed compiled model has no NN coefficients, TP, software operation or
 * external SRAM. Shader code remains an authenticated declared model payload. */
static int profile(const struct nbg_model *m,struct nbg_assembly_plan *p)
{
 struct nbg_checksum crc;
 if(nbg_checksum_verify(m->data,m->bytes,&crc)!=NBG_CHECKSUM_MATCH || m->bytes!=3976 ||
  m->header.version!=0x20000 || m->header.target!=0x1000003b || m->header.flags!=0x0a ||
  m->header.core_count!=1 || m->header.operation_count!=5 || m->header.layer_count!=3 ||
  m->header.nn_bytes!=192 || m->header.input_count!=1 || m->header.output_count!=1 ||
  m->header.pool_bytes!=1047808 || m->header.pool_alignment!=64 || m->header.pool_base ||
  m->header.axi_sram_base || m->header.axi_sram_bytes || m->header.vip_sram_base ||
  m->header.vip_sram_bytes!=524288)return 0;
 static const uint32_t sizes[NBG_SECTION_COUNT]={
  [NBG_INPUT]=112,[NBG_OUTPUT]=112,[NBG_LAYER]=228,[NBG_OPERATION]=160,
  [NBG_NN]=576,[NBG_SH]=8,[NBG_PATCH]=364,[NBG_PPU_INSTR_TABLE]=8,
  [NBG_PPU_INSTR]=512,[NBG_LOAD_STATES_TABLE]=40,[NBG_LOAD_STATES]=648,
  [NBG_NETWORK_INFO]=220,[NBG_MEMORY_POOL]=8,[NBG_VIP_SRAM]=4,[NBG_SECTION_CHECKSUM]=24};
 for(unsigned i=0;i<NBG_SECTION_COUNT;++i)if(m->sections[i].bytes!=sizes[i])return 0;
 static const uint32_t in[]={255,128,4},out[]={128,128,4};
 if(!tensor(m,NBG_INPUT,in)||!tensor(m,NBG_OUTPUT,out))return 0;
 const unsigned char *sh=m->data+m->sections[NBG_SH].offset;
 if(word(sh)||word(sh+4))return 0;
 struct nbg_span span;
 if(!nbg_model_table(m,NBG_PPU_INSTR_TABLE,0,&span)||span.bytes!=368||
  span.data!=m->data+m->sections[NBG_PPU_INSTR].offset)return 0;
 for(unsigned i=368;i<512;++i)if(span.data[i])return 0;
 struct nbg_operation op;struct nbg_patch patch;
 if(!nbg_model_operation(m,0,&op)||op.kind!=0xffff||op.index!=0xffff||op.layer!=UINT32_MAX||
  op.state||op.first_patch||op.patch_count!=1||op.auxiliary[0]||op.auxiliary[1]!=UINT32_MAX||
  !nbg_model_table(m,NBG_LOAD_STATES_TABLE,0,&span)||span.bytes!=40||!same(span.data,initial,10)||
  !nbg_model_patch(m,0,&patch)||patch.kind!=1||patch.destination!=20||patch.source!=8||
  patch.index!=-1||patch.source_offset||patch.transformed||patch.name_word||
  patch.high_destination||patch.high_mask||patch.destination_base)return 0;
 memset(p,0,sizeof(*p));
 static const struct nbg_allocation_requirement requirements[6]={
  {576,576,4096},{512,512,4096},{1047808,1047808,1048576},
  {712,524280,524288},{522240,522304,524288},{262144,262208,266240}};
 memcpy(p->buffers,requirements,sizeof(requirements));p->nn_operations=3;
 p->descriptor_patches=6;p->state_patches=6;p->command_body_bytes=656;return 1;
}
struct operation_image {unsigned char state[512],nn[192];unsigned bytes;};
static int operation(const struct nbg_model *m,unsigned number,
 const struct nbg_assembly_buffer *buffers,const struct nbg_assembly_plan *plan,
 struct operation_image *image)
{
 struct nbg_operation op;struct nbg_span span;
 unsigned sh=number==1,index=number-2,layer=number<4?1:2;
 if(!nbg_model_operation(m,number,&op)||op.kind!=(sh?1:8)||op.index!=(sh?0:index)||
  op.layer!=(sh?0:layer)||op.state!=number||op.first_patch!=1+(number-1)*3||
  op.patch_count!=3||op.auxiliary[0]||op.auxiliary[1]!=(sh?0:layer)||
  !nbg_model_table(m,NBG_LOAD_STATES_TABLE,number,&span)||span.bytes!=(sh?512:32))return 0;
 image->bytes=(unsigned)span.bytes;memcpy(image->state,span.data,span.bytes);
 if(!sh){if(!same(span.data,nt_state,8))return 0;
  memcpy(image->nn,m->data+m->sections[NBG_NN].offset+index*192,192);}
 /* Each row is an observed destination/source/index/offset/transformation;
  * shader source0 names its PPU allocation, NT source0 names its descriptor. */
 static const int32_t expected[4][3][6]={
  {{0,236,0,-1,0,0},{0,28,1,0,0,0},{0,124,3,-1,256,0}},
  {{0,20,0,0,0,1},{1,24,3,-1,768,0},{1,28,3,-1,524544,0}},
  {{0,20,0,0,0,1},{1,24,3,-1,66304,0},{1,28,3,-1,590080,0}},
  {{0,20,0,0,0,1},{1,24,3,-1,524544,0},{1,28,2,0,0,0}}};
 for(unsigned i=0;i<3;++i){
  struct nbg_patch patch;if(!nbg_model_patch(m,op.first_patch+i,&patch))return 0;
  const int32_t *v=expected[number-1][i];
  if(patch.kind!=(uint32_t)v[0]||patch.destination!=(uint32_t)v[1]||patch.source!=(uint32_t)v[2]||
   patch.index!=v[3]||patch.source_offset!=(uint32_t)v[4]||patch.transformed!=(uint32_t)v[5]||
   patch.name_word||patch.high_destination||patch.high_mask||patch.destination_base)return 0;
  unsigned slot;uint32_t offset=patch.source_offset;
  if(!patch.source){slot=sh?NBG_SH_NT_PPU:NBG_SH_NT_NN;if(!sh)offset=index*192;}
  else slot=patch.source==1?NBG_SH_NT_INPUT:patch.source==2?NBG_SH_NT_OUTPUT:NBG_SH_NT_POOL;
  struct nbg_relocation r={plan->buffers[slot].minimum_bytes,1,
   buffers?buffers[slot].device:0x1000,offset,patch.destination,
   (!sh&&!i)?NBG_ADDRESS_FLAGS6:NBG_ADDRESS_WORD};
  /* Only patch destinations/address encodings are established here. The
   * trusted exact artifact and physical confinement govern complete accesses. */
  if(!nbg_relocate(sh||!i?image->state:image->nn,sh||!i?image->bytes:192,&r,1))return 0;
 }
 return 1;
}
static int inspect(const struct nbg_model *m,const struct nbg_assembly_buffer *b,
 struct nbg_assembly_plan *p)
{if(!profile(m,p))return 0;for(unsigned n=1;n<5;++n){struct operation_image image;
 if(!operation(m,n,b,p,&image))return 0;}return 1;}
int nbg_sh_nt_requirements(const struct nbg_model *m,struct nbg_assembly_plan *out)
{struct nbg_assembly_plan p;if(!result_space(m,out)||!inspect(m,NULL,&p))return 0;*out=p;return 1;}
int nbg_sh_nt_assemble(const struct nbg_model *m,const struct nbg_assembly_buffer b[6],
 struct nbg_assembly_plan *out)
{
 struct nbg_assembly_plan p;
 if(!result_space(m,out)||!range(b,6*sizeof(*b))||!separate(out,sizeof(*out),b,6*sizeof(*b))||
  !inspect(m,NULL,&p))return 0;
 for(unsigned i=0;i<6;++i){
  if(!range(b[i].cpu,b[i].bytes)||b[i].bytes!=p.buffers[i].allocation_bytes||
   ((uintptr_t)b[i].cpu&4095)||!b[i].device||(b[i].device&4095)||
   b[i].bytes>UINT64_C(0x100000000)-b[i].device||
   overlap(b[i].device,b[i].bytes,0x400000,0x80000)||
   !separate(b[i].cpu,b[i].bytes,m,sizeof(*m))||!separate(b[i].cpu,b[i].bytes,m->data,m->bytes)||
   !separate(b[i].cpu,b[i].bytes,b,6*sizeof(*b))||!separate(b[i].cpu,b[i].bytes,out,sizeof(*out)))return 0;
  for(unsigned j=0;j<i;++j)if(!separate(b[i].cpu,b[i].bytes,b[j].cpu,b[j].bytes)||
   overlap(b[i].device,b[i].bytes,b[j].device,b[j].bytes))return 0;
 }
 if(!inspect(m,b,&p))return 0;
 for(unsigned i=0;i<6;++i)memset(b[i].cpu,0,b[i].bytes);
 memcpy(b[NBG_SH_NT_PPU].cpu,m->data+m->sections[NBG_PPU_INSTR].offset,512);
 unsigned char *command=b[NBG_SH_NT_COMMAND].cpu;
 for(size_t i=0;i<b[NBG_SH_NT_COMMAND].bytes;i+=8)put(command+i,0x10000000);
 words(command,prefix,12);unsigned offset=48;
 for(unsigned n=1;n<5;++n){struct operation_image image;(void)operation(m,n,b,&p,&image);
  memcpy(command+offset,image.state,image.bytes);offset+=image.bytes;
  if(n>1)memcpy((unsigned char *)b[NBG_SH_NT_NN].cpu+(n-2)*192,image.nn,192);
 }
 words(command+offset,footer,14);*out=p;return 1;
}
