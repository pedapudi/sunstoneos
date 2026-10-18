/* Only the existing provider/card fixture is included; prior tests are absent. */
#include "mmc-platform-fixture.h"
static void adaptive(void)
{
 storage_setup(SUNXI_MMC_SD);
 uint32_t *mode=pmic_model_word(&m.power,0x02000040);
 *mode=(*mode&~(3u<<10))|(1u<<10);
 ready_storage();assert(m.platform.voltage_mode==1);
}
int main(void)
{
 uint8_t buffer[512];struct sunxi_mmc_result result;
 adaptive();assert(a733_mmc_platform_read(&m.platform,0,1,buffer,sizeof(buffer),1000000)==A733_MMC_PENDING);
 enum a733_mmc_platform_result r=A733_MMC_PENDING;
 for(unsigned i=0;i<200&&r!=A733_MMC_COMPLETE;++i){r=step();assert(r!=A733_MMC_RETAINED);}
 assert(r==A733_MMC_COMPLETE);
 assert(a733_mmc_platform_take(&m.platform,&result,1000000)==A733_MMC_READY&&result.received==512&&!result.write_started);
 assert(((*pmic_model_word(&m.power,0x02000040)>>10)&3)==1);
 for(unsigned loss=0;loss<3;++loss){
  adaptive();assert(a733_mmc_platform_read(&m.platform,0,1,buffer,sizeof(buffer),1000000)==A733_MMC_PENDING);
  unsigned commands=m.card.command_count;
  if(loss==0){uint32_t *v=pmic_model_word(&m.power,0x02000040);*v=(*v&~(3u<<10))|(2u<<10);}
  else if(loss==1)*pmic_model_word(&m.power,0x02000048)|=1u<<10;
  else *pmic_model_word(&m.power,0x02000070)&=~1u;
  assert(step()==A733_MMC_RETAINED&&m.platform.core.borrowed==buffer&&m.card.command_count==commands);
  retained();
 }
 puts("PASS adaptive 3.3V SD read and retained mode/status/selector loss without further card commands");
}
