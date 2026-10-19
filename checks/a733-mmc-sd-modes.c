/* Actual PMIC/rails/pins/clocks/MMC platform/core; model only card/MMIO/time. */
#include "mmc-platform-fixture.h"
#include "mmc-sd-mode-model.h"
static struct sd_model card_mode;
static unsigned clock_writes,lose_clock,fail_clock;
static int new_read(void *v,uintptr_t a,uint32_t *out)
{
 struct storage_model *s=v;
 if(a>=s->platform.base&&a<=s->platform.base+0x300){
  assert(s->power.register_held&&s->platform.pin.id&&s->platform.rail.id);
  assert((*pmic_model_word(&s->power,s->platform.module)&0x87001f1f)==s->platform.running_module);
  ++s->accesses;return mode_read(&card_mode,(uint32_t)(a-s->platform.base),out);
 }
 return storage_read(v,a,out);
}
static int new_write(void *v,uintptr_t a,uint32_t value)
{
 struct storage_model *s=v;
 if(a>=s->platform.base&&a<=s->platform.base+0x300){
  assert(s->power.register_held&&s->platform.pin.id&&s->platform.rail.id);
  assert((*pmic_model_word(&s->power,s->platform.module)&0x87001f1f)==s->platform.running_module);
  ++s->accesses;++s->writes;return mode_write(&card_mode,(uint32_t)(a-s->platform.base),value);
 }
 if(a==s->platform.module&&s->platform.core.sd_ios==1){
  assert(card_mode.base.reg[1]==0&&!(card_mode.base.reg[0x18/4]&(1u<<31)));
  assert(s->power.register_held&&(s->platform.clock.shared&A733_CLOCK_PERI0));++clock_writes;
  if(lose_clock)atomic_store(&s->power.clocks.failed,1);
  int r=storage_write(v,a,value);return fail_clock?0:r;
 }
 return storage_write(v,a,value);
}
static void setup_mode(unsigned profile)
{
 storage_setup(SUNXI_MMC_SD);card_mode=(struct sd_model){.base=m.card};clock_writes=lose_clock=fail_clock=0;
 field(card_mode.base.csd,84,12,5|(1u<<10));field(card_mode.base.csd,96,8,0x32);card_mode.scr[0]=2;card_mode.scr[1]=5;
 *pmic_model_word(&m.power,0x020020a0)=0xfe000000u|(99u<<8)|(1u<<20);
 *pmic_model_word(&m.power,0x02003908)=((1u<<11)|(1u<<2)|(1u<<1))*0x10001;
 *pmic_model_word(&m.power,0x02003988)=((1u<<11)|(1u<<2)|(1u<<1))<<16;
 struct a733_mmc_platform_config c=m.platform.config;c.sd_profile=(enum sunxi_mmc_sd_profile)profile;
 for(unsigned i=0;i<7;++i)c.desired[i].function=i<6?2:0;
 struct a733_mmc_platform_ops ops=m.platform.ops;ops.read=new_read;ops.write=new_write;
 memset(&m.platform,0,sizeof(m.platform));assert(a733_mmc_platform_init(&m.platform,&c,&ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
}
static enum a733_mmc_platform_result advance(void)
{card_mode.base.fifo_this_poll=0;return step();}
static enum a733_mmc_platform_result terminal(void)
{
 enum a733_mmc_platform_result r=A733_MMC_PENDING;
 for(unsigned i=0;i<3000&&r!=A733_MMC_READY&&r!=A733_MMC_RETAINED;++i)r=advance();return r;
}
int main(void)
{
 (void)ready_storage;
 for(unsigned profile=1;profile<=2;++profile){
  setup_mode(profile);assert(terminal()==A733_MMC_READY&&clock_writes==3);
  assert(m.platform.clock.shared&A733_CLOCK_PERI0);assert(m.platform.pin.masks[A733_PIN_F]==0x7f);
  assert((*pmic_model_word(&m.power,0x02000300)&0xfffffff)==0x0222222);
  assert(m.platform.running_module==(profile==1?0x81000007u:0x81000003u));
  struct sunxi_mmc_card copied;assert(a733_mmc_platform_card(&m.platform,&copied,900000)==A733_MMC_READY&&copied.bus_width==4);
  uint8_t buffer[1024];assert(a733_mmc_platform_read(&m.platform,0,2,buffer,sizeof(buffer),1000000)==A733_MMC_PENDING);
  enum a733_mmc_platform_result r=A733_MMC_PENDING;
  for(unsigned i=0;i<200&&r!=A733_MMC_COMPLETE;++i)r=advance();assert(r==A733_MMC_COMPLETE);
  struct sunxi_mmc_result result;assert(a733_mmc_platform_take(&m.platform,&result,1000000)==A733_MMC_READY&&result.received==1024);
 }
 setup_mode(1);*pmic_model_word(&m.power,0x02003908)&=~(1u<<1);assert(terminal()==A733_MMC_RETAINED&&!card_mode.base.command_count);retained();
 setup_mode(1);fail_clock=1;assert(terminal()==A733_MMC_RETAINED&&clock_writes==1);retained();
 setup_mode(1);lose_clock=1;assert(terminal()==A733_MMC_RETAINED&&clock_writes==1);retained();
 setup_mode(1);assert(terminal()==A733_MMC_READY);*pmic_model_word(&m.power,m.platform.module)^=1;
 struct sunxi_mmc_card copied;assert(a733_mmc_platform_card(&m.platform,&copied,900000)==A733_MMC_RETAINED);retained();
 puts("SD modes actual providers: full PF data lease, PERI0 ownership/branch proof, gated module transition, copied card/repeated block reads and retained parent/write/rate loss passed.");return 0;
}
