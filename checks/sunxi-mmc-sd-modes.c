/* Reuse the accepted card model and helpers; its old cases never execute. */
#define main accepted_mmc_model_main
#include "sunxi-mmc.c"
#undef main
struct sd_model {struct model base;uint8_t scr[8],switch_data[64];unsigned width,mode,divisor,module_calls,scr_bad,hs_missing,hs_busy,hs_bad,fail_width,fail_switch,fail_module,expire_module,lose_module;};
static int mode_read(void *v,uint32_t off,uint32_t *out)
{
 struct sd_model *s=v;struct model *m=&s->base;
 if(off==0x200&&((m->data_command&63)==51||((m->data_command&63)==6))){
  assert(++m->fifo_this_poll<=SUNXI_MMC_FIFO_WORDS&&m->fifo_pos<m->fifo_total);*out=0;
  for(unsigned b=0;b<4;++b)*out|=(uint32_t)((m->data_command&63)==51?s->scr[m->fifo_pos++]:s->switch_data[m->fifo_pos++])<<(8*b);
  return 1;
 }
 return read32(m,off,out);
}
static inline int mode_read_fifo(void *v,uint32_t *words,unsigned count)
{for(unsigned i=0;i<count;++i)if(mode_read(v,0x200,&words[i])!=1)return 0;return 1;}
static int mode_write(void *v,uint32_t off,uint32_t value)
{
 struct sd_model *s=v;struct model *m=&s->base;
 if(off==0x18&&!(value&(1u<<21))&&m->selected){
  unsigned cmd=value&63,arg=m->reg[0x1c/4];
  if(cmd==55||cmd==51||cmd==6){
   assert(!(value&(1u<<10)));m->commands[m->command_count]=cmd;m->args[m->command_count++]=arg;
   m->reg[off/4]=value&~(1u<<31);m->reg[0x38/4]=4;m->reg[0x20/4]=0x900;
   m->data_command=0;m->fifo_total=m->fifo_pos=0;
   if(cmd==55){assert(arg==0x12340000);m->app=1;m->reg[0x20/4]|=1u<<5;}
   else if(cmd==51){assert(m->app&&!arg&&m->reg[0x10/4]==8);m->app=0;m->data_command=value;m->fifo_total=8;}
   else if(!(value&(1u<<9))){assert(m->app&&arg==2);m->app=0;s->width=4;if(s->fail_width)return 0;}
   else {
    assert(!m->app&&s->width==4&&m->reg[0xc/4]==1&&m->reg[0x10/4]==64);
    assert(arg==0x00fffff1||arg==0x80fffff1);m->data_command=value;m->fifo_total=64;
    memset(s->switch_data,0,64);s->switch_data[13]=s->hs_missing?0:2;s->switch_data[29]=s->hs_busy?2:0;
    if(arg>>31){s->mode=1;s->switch_data[16]=s->hs_bad?0:1;if(s->fail_switch)return 0;}
   }
   return 1;
  }
 }
 return write32(m,off,value);
}
static int module(void *v,uintptr_t token,unsigned divisor)
{
 struct sd_model *s=v;struct model *m=&s->base;
 assert(token==0xa733&&m->reg[4/4]==0&&!(m->reg[0x18/4]&(1u<<31))&&s->width==4&&m->reg[0xc/4]==1);
 assert(divisor>=4&&divisor<=32);++s->module_calls;s->divisor=divisor;
 if(s->expire_module)m->now=1000000;if(s->lose_module)m->retain_result=0;return !s->fail_module;
}
static void start(struct sd_model *s,struct sunxi_mmc *h,unsigned profile,unsigned speed,unsigned sector)
{
 memset(s,0,sizeof(*s));seed(&s->base,h,SUNXI_MMC_SD,sector);
 field(s->base.csd,84,12,5|(1u<<10));field(s->base.csd,96,8,speed);s->scr[0]=2;s->scr[1]=5;
 struct sunxi_mmc_config c={{mode_read,mode_write,barrier,now_us,admit,retained,module,mode_read_fifo,model_write_fifo},s,0xa733,SUNXI_MMC_SD,800000,0x300000,(enum sunxi_mmc_sd_profile)profile};
 assert(sunxi_mmc_init(h,&c)&&sunxi_mmc_open(h,1000000));
}
static void expect_fault(struct sd_model *s,struct sunxi_mmc *h)
{run(&s->base);assert(h->state==SUNXI_MMC_FAULT);unsigned calls=s->base.calls,commands=s->base.command_count;sunxi_mmc_poll(h);assert(calls==s->base.calls&&commands==s->base.command_count);}
int main(void)
{
 struct sd_model s;struct sunxi_mmc h;uint8_t data[1024];struct sunxi_mmc_result result;
 for(unsigned profile=1;profile<=2;++profile){
  start(&s,&h,profile,0x32,profile-1);run(&s.base);assert(h.state==SUNXI_MMC_READY&&h.card.bus_width==4&&s.module_calls==1);
  assert(h.card.card_hz==(profile==1?25000000u:50000000u)&&s.divisor==(profile==1?8u:4u));
  assert((s.base.reg[0x5c/4]&0x80000331u)==(profile==1?0x80000000u:0x80000110u));
  assert(sunxi_mmc_read(&h,2,2,data,sizeof(data),1000000));run(&s.base);assert(h.state==SUNXI_MMC_DONE&&s.base.reg[0x10/4]==512);
  assert(sunxi_mmc_take(&h,&result)&&result.received==1024&&!result.write_started);
  assert(sunxi_mmc_read(&h,3,1,data,512,1000000));run(&s.base);assert(h.state==SUNXI_MMC_DONE&&sunxi_mmc_take(&h,&result));
 }
 start(&s,&h,1,0x2a,1);run(&s.base);assert(h.state==SUNXI_MMC_READY&&h.card.card_hz==20000000&&s.divisor==10);
 start(&s,&h,1,0x22,1);run(&s.base);assert(h.state==SUNXI_MMC_READY&&h.card.card_hz==400000000u/28&&s.divisor==14);
 start(&s,&h,1,0x09,1);expect_fault(&s,&h);assert(!s.module_calls);
 start(&s,&h,1,0x32,1);s.scr[0]=0x12;expect_fault(&s,&h);assert(!s.width);
 start(&s,&h,1,0x32,1);s.scr[1]=1;expect_fault(&s,&h);assert(!s.width);
 start(&s,&h,2,0x32,1);field(s.base.csd,84,12,5);expect_fault(&s,&h);assert(!s.mode&&!s.module_calls);
 start(&s,&h,2,0x32,1);s.hs_missing=1;expect_fault(&s,&h);assert(!s.mode&&!s.module_calls);
 start(&s,&h,2,0x32,1);s.hs_busy=1;expect_fault(&s,&h);assert(!s.mode&&!s.module_calls);
 start(&s,&h,2,0x32,1);s.hs_bad=1;expect_fault(&s,&h);assert(s.mode&&!s.module_calls);
 start(&s,&h,1,0x32,1);s.fail_width=1;expect_fault(&s,&h);assert(s.width&&!s.module_calls);
 start(&s,&h,2,0x32,1);s.fail_switch=1;expect_fault(&s,&h);assert(s.mode&&!s.module_calls);
 start(&s,&h,1,0x32,1);s.fail_module=1;expect_fault(&s,&h);assert(s.module_calls==1);
 start(&s,&h,1,0x32,1);s.expire_module=1;expect_fault(&s,&h);assert(s.module_calls==1);
 start(&s,&h,1,0x32,1);s.lose_module=1;expect_fault(&s,&h);assert(s.module_calls==1);
 puts("SD bus modes: actual SCR/ACMD6/CMD6 progression, exact8/64/512 blocks, CSD-capped rates, HS selection, repeated read and terminal command/clock/owner failures passed.");return 0;
}
