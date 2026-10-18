/* Reuse provider/card fixtures; no previous test main executes. */
#include "mmc-platform-fixture.h"
static unsigned pf_writes,fail_pf;
static int observed_pin_write(void *v,uintptr_t a,uint32_t value)
{
 if(a>=0x02000300&&a<=0x02000324){
  assert(!*pmic_model_word(&m.power,m.platform.module));
  assert(!*pmic_model_word(&m.power,m.platform.bus));
  if(a!=0x02000300)assert((*pmic_model_word(&m.power,0x02000300)&0xfffffff)==0xfffffff);
  ++pf_writes;
 }
 int r=pmic_model_pin_write(v,a,value);
 return fail_pf&&a==0x02000300?0:r;
}
static void setup_rom(void)
{
 storage_setup(SUNXI_MMC_SD);pf_writes=fail_pf=0;
 struct a733_mmc_platform_config c=m.platform.config;
 struct a733_mmc_platform_ops ops=m.platform.ops;
 const unsigned rom[]={3,3,15,3,15,3,15};
 for(unsigned i=0;i<7;++i)c.expected[i].function=rom[i];
 memset(&m.platform,0,sizeof(m.platform));
 assert(a733_mmc_platform_init(&m.platform,&c,&ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
 *pmic_model_word(&m.power,0x02000300)=0x0f3f3f33;
 m.power.pins.operations.write=observed_pin_write;
}
int main(int argc,char **argv)
{
 (void)argv;enum a733_mmc_platform_result r=A733_MMC_PENDING;
 if(argc==1){
 setup_rom();ready_storage();assert(pf_writes);
 assert(m.platform.pin.masks[A733_PIN_F]==0x7f);
 assert((*pmic_model_word(&m.power,0x02000300)&0xfffffff)==0x2220);
 uint8_t buffer[512];assert(a733_mmc_platform_read(&m.platform,0,1,buffer,sizeof(buffer),1000000)==A733_MMC_PENDING);
 r=A733_MMC_PENDING;
 for(unsigned i=0;i<200&&r!=A733_MMC_COMPLETE;++i)r=step();
 assert(r==A733_MMC_COMPLETE&&!m.platform.core.result.write_started);
 }
 if(argc==1||!strcmp(argv[1],"--mismatch")){
 /* Exact inherited mismatch is refused before any PF write or card command. */
 setup_rom();*pmic_model_word(&m.power,0x02000300)^=1;
 for(unsigned i=0;i<1000;++i){r=step();assert(r!=A733_MMC_RETAINED);}
 assert(!pf_writes&&!m.card.command_count&&!m.platform.pin.id&&m.platform.stage==7&&r==A733_MMC_BUSY);
 }
 if(argc==1||!strcmp(argv[1],"--remaining")){
 setup_rom();r=A733_MMC_PENDING;*pmic_model_word(&m.power,m.platform.module)=1u<<31;
 for(unsigned i=0;i<1000&&r!=A733_MMC_RETAINED;++i)r=step();
 assert(r==A733_MMC_RETAINED&&!pf_writes&&!m.card.command_count);retained();
 setup_rom();fail_pf=1;r=A733_MMC_PENDING;
 for(unsigned i=0;i<1000&&r!=A733_MMC_RETAINED;++i)r=step();
 assert(r==A733_MMC_RETAINED&&m.power.pins.failed&&m.platform.pin.id&&!m.card.command_count);retained();
 setup_rom();struct a733_mmc_platform_config c=m.platform.config;
 struct a733_mmc_platform_ops ops=m.platform.ops;c.expected[2].function=3;
 memset(&m.platform,0,sizeof(m.platform));
 assert(!a733_mmc_platform_init(&m.platform,&c,&ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
 assert(!pf_writes);
 }
 puts("PASS selected ROM JTAG pin handoff cases");
}
