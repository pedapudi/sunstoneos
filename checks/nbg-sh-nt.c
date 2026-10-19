#include "nbg_sh_nt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t word(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v)
{for(unsigned i=0;i<4;++i)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t crc(const unsigned char *p,size_t n)
{uint32_t v=UINT32_MAX;for(size_t i=0;i<n;++i){v^=p[i];for(unsigned b=0;b<8;++b)v=(v>>1)^((v&1)?0xedb88320u:0);}return ~v;}
static void repair(unsigned char *p)
{
 unsigned sections=word(p+56),sums=0;
 for(unsigned i=0;i<sections;++i)if(word(p+60+i*44)==40)sums=word(p+64+i*44);
 assert(sums);
 for(unsigned i=0;i<3;++i){unsigned kind=word(p+sums+i*8),offset=0,n=0;
  for(unsigned j=0;j<sections;++j)if(word(p+60+j*44)==kind){offset=word(p+64+j*44);n=word(p+72+j*44);}
  assert(n);put(p+sums+i*8+4,crc(p+offset,n));}
 uint32_t v=UINT32_MAX;
 for(unsigned j=0;j<sections;++j){size_t offset=word(p+64+j*44),n=word(p+72+j*44);
  for(size_t i=offset;i<offset+n;++i){unsigned char b=i>=sums&&i<sums+24&&((i-sums)&7)>=4?0:p[i];v^=b;
   for(unsigned bit=0;bit<8;++bit)v=(v>>1)^((v&1)?0xedb88320u:0);}}
 put(p+12,~v);
}
int main(int argc,char **argv)
{
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);unsigned char data[3976],saved[3976];
 assert(fread(data,1,sizeof(data),f)==sizeof(data)&&fgetc(f)==EOF);fclose(f);memcpy(saved,data,sizeof(data));
 struct nbg_model m;struct nbg_assembly_plan p,output,old;
 assert(nbg_model_open(data,sizeof(data),&m)==NBG_DECODE_OK);
 assert(nbg_sh_nt_requirements(&m,&p));assert(p.nn_operations==3&&p.command_body_bytes==656);
 struct nbg_assembly_buffer buffers[6];
 const uint32_t devices[]={0xe0000000,0xe0002000,0xe0004000,0xe0105000,0xe0186000,0xe0207000};
 for(unsigned i=0;i<6;++i){size_t n=p.buffers[i].allocation_bytes;void *b=aligned_alloc(4096,n);assert(b);memset(b,0xa5,n);buffers[i]=(struct nbg_assembly_buffer){b,n,devices[i]};}
 memset(&output,0x5a,sizeof(output));old=output;
 /* Repaired checksums force the changed profile/patch branches to refuse,
  * including a final operation after earlier scratch construction passed. */
 const struct {unsigned at;uint32_t value;} bad[]={
  {2172+1*28+4,232},{2172+2*28+8,3},{2172+3*28+16,1047808},
  {2172+10*28+4,24},{2172+12*28+8,8},{1428+4*32+20,2},
  {940,1047807},{976+28,1},{1088+36,2}};
 unsigned cases=0;
 for(unsigned k=0;k<sizeof(bad)/sizeof(*bad);++k){put(data+bad[k].at,bad[k].value);repair(data);
  if(nbg_model_open(data,sizeof(data),&m)==NBG_DECODE_OK){
   assert(!nbg_sh_nt_assemble(&m,buffers,&output));assert(!memcmp(&output,&old,sizeof(old)));
   for(unsigned i=0;i<6;++i){unsigned char *b=buffers[i].cpu;for(size_t j=0;j<buffers[i].bytes;++j)assert(b[j]==0xa5);}}
  memcpy(data,saved,sizeof(data));++cases;
 }
 assert(nbg_model_open(data,sizeof(data),&m)==NBG_DECODE_OK);
 uint32_t device=buffers[5].device;buffers[5].device=buffers[4].device;
 assert(!nbg_sh_nt_assemble(&m,buffers,&output));buffers[5].device=device;++cases;
 void *cpu=buffers[5].cpu;buffers[5].cpu=buffers[4].cpu;
 assert(!nbg_sh_nt_assemble(&m,buffers,&output));buffers[5].cpu=cpu;++cases;
 assert(!nbg_sh_nt_assemble(&m,buffers,(void *)data));++cases;
 assert(nbg_sh_nt_assemble(&m,buffers,&output));assert(!memcmp(&p,&output,sizeof(p)));++cases;
 assert(word((unsigned char *)buffers[3].cpu+656+28)==0x40);
 for(unsigned i=0;i<6;++i)free(buffers[i].cpu);
 printf("PASS SH/NT ASan/UBSan focused profile/refusal/preservation: %u cases\n",cases);
}
