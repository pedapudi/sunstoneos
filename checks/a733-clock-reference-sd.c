#include "mmc-platform-fixture.h"
#include "a733_clock_reference.h"
static void actual(void)
{
 storage_setup(SUNXI_MMC_SD);struct a733_mmc_platform_config c=m.platform.config;struct a733_mmc_platform_ops ops=m.platform.ops;
 c.fabric[0]=0x03000002;c.fabric[1]=0x83000000;c.fabric[2]=0x81000000;
 c.fabric_hz[0]=200000000;c.fabric_hz[1]=c.fabric_hz[2]=600000000;
 memset(&m.platform,0,sizeof(m.platform));assert(a733_mmc_platform_init(&m.platform,&c,&ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
 *pmic_model_word(&m.power,0x07090160)=0x983fb0f7;*pmic_model_word(&m.power,0x02002000)=0xf8675f00;
 *pmic_model_word(&m.power,0x07010010)=0x04000000;
 *pmic_model_word(&m.power,0x02002500)=c.fabric[0];*pmic_model_word(&m.power,0x02002580)=0xc3000000;*pmic_model_word(&m.power,0x02002588)=c.fabric[2];
 *pmic_model_word(&m.power,0x020020a0)=0xff126310;*pmic_model_word(&m.power,0x020020a8)=0;*pmic_model_word(&m.power,0x020020ac)=0x30000000;
 *pmic_model_word(&m.power,0x02003908)=0x8fff0fff;*pmic_model_word(&m.power,0x02003988)=0x0fff0000;
}
int main(void)
{
 assert(a733_clock_ref24_valid(0x983fb0f7,0xf8675f00));
 assert(a733_clock_ref24_valid(0x10004002,0xf84f6300)); /*19.2*100/80*/
 assert(!a733_clock_ref24_valid(0x983fb0f7,0xf8675e00));
 assert(!a733_clock_ref24_valid(0x983fb0f7,0xe8675f00));
 assert(!a733_clock_ref24_valid(0x983fb0f5,0xf8675f00));
 assert(!a733_clock_ref24_valid(0x883fb0f7,0xf8675f00));
 actual();ready_storage();uint8_t data[512];struct sunxi_mmc_result result;
 assert(a733_mmc_platform_read(&m.platform,0,1,data,sizeof(data),1000000)==A733_MMC_PENDING);
 enum a733_mmc_platform_result r=A733_MMC_PENDING;
 for(unsigned i=0;i<200&&r!=A733_MMC_COMPLETE;++i)r=step();
 assert(r==A733_MMC_COMPLETE&&a733_mmc_platform_take(&m.platform,&result,1000000)==A733_MMC_READY&&!result.write_started);
 assert(a733_mmc_platform_read(&m.platform,0,1,data,sizeof(data),1000000)==A733_MMC_PENDING);
 unsigned commands=m.card.command_count;*pmic_model_word(&m.power,0x02002000)^=1u<<8;
 assert(step()==A733_MMC_RETAINED&&m.platform.core.borrowed==data&&m.card.command_count==commands);retained();
 puts("PASS exact REF decoder and actual26MHz SD/PERI0 source with retained ratio loss");
}
