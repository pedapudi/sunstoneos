/* SPDX-License-Identifier: GPL-2.0-only */
#include "../drivers/camera/isp0-replay.h"
#include <string.h>
#define CHECK(x) do { if(!(x))return __LINE__; } while(0)
#define TOP UINT32_C(0x05800800)
#define ISP UINT32_C(0x05900000)
#define VIPP UINT32_C(0x05910000)
struct reg {uintptr_t address;uint32_t value;};
struct model {
 struct reg regs[96];unsigned count,starts,stuck,refuse,missing;
 uint64_t now;
};
static uint32_t *reg(struct model *m,uintptr_t address)
{
 for(unsigned i=0;i<m->count;i++)if(m->regs[i].address==address)return &m->regs[i].value;
 if(m->count==96)return NULL;
 m->regs[m->count].address=address;return &m->regs[m->count++].value;
}
static int now(void *context,uint64_t *value)
{struct model *m=context;*value=++m->now;return 1;}
static int read32(void *context,uintptr_t address,uint32_t *value)
{uint32_t *r=reg(context,address);if(!r)return -1;*value=*r;return 1;}
static int write32(void *context,uintptr_t address,uint32_t value)
{
 struct model *m=context;uint32_t *r=reg(m,address);if(!r)return -1;
 if(m->refuse&&address==ISP+0x20)return -1;
 if(address==ISP+0x2c||address==VIPP+0x30){
  if(m->stuck!=(address==ISP+0x2c?1u:2u))*r&=~value;
 }else *r=value;
 if(address==ISP+0x20)*r&=~2u;
 if(address==VIPP+0x40)*r&=~4u;
 if(address==TOP+4&&(value&0x10)){
  ++m->starts;*r=1;
  *reg(m,ISP+0x2c)=m->missing?8:12;
  *reg(m,VIPP+0x30)=0x100;
 }
 return 1;
}
int isp0_rearm_check(unsigned fault)
{
 struct model m={0};struct a733_isp0 engine={0};
 struct a733_isp0_ops ops={now,read32,write32};
 CHECK(a733_isp0_init(&engine,&ops,&m,100000)==1);
 int result=0;
 for(unsigned i=0;i<100&&result==0;i++)result=a733_isp0_poll(&engine);
 CHECK(result==1&&engine.armed);
 m.missing=fault==4;
 CHECK(a733_isp0_trigger(&engine)==1&&m.starts==1);
 if(fault==4){
  CHECK(a733_isp0_complete(&engine)==0);
  m.now=100000;CHECK(a733_isp0_complete(&engine)==-1);return 0;
 }
 CHECK(a733_isp0_complete(&engine)==1);
 m.stuck=fault==1?1:fault==2?2:0;
 if(fault==3)*reg(&m,ISP+0x2c)|=1u<<16;
 if(fault==5)*reg(&m,TOP+4)|=0x10;
 if(fault==6)m.refuse=1;
 if(fault==7)m.now=100000;
 for(unsigned frame=0;frame<16;frame++){
  result=0;
  for(unsigned step=0;step<12&&result==0;step++)result=a733_isp0_rearm(&engine);
  if(fault){CHECK(result==-1&&engine.fault&&m.starts==1);return 0;}
  CHECK(result==1&&engine.armed&&!engine.started&&!engine.phase);
  CHECK(*reg(&m,ISP+0x2c)==0&&*reg(&m,VIPP+0x30)==0);
  CHECK(a733_isp0_complete(&engine)==-1);
  CHECK(a733_isp0_trigger(&engine)==1&&m.starts==frame+2);
  CHECK(a733_isp0_complete(&engine)==1);
 }
 return 0;
}
#ifndef ISP0_QEMU_CHECK
#include <stdio.h>
int main(void)
{
 for(unsigned fault=0;fault<8;fault++){
  int line=isp0_rearm_check(fault);
  if(line){printf("FAIL ISP0 case %u line %d\n",fault,line);return 1;}
 }
 puts("PASS ISP0 retained rearm: repeated capture, stale/error/deadline refusals");return 0;
}
#endif
