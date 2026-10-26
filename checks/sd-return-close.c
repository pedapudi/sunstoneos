/* Existing card/PMIC fixture, with HIGH_SPEED4 selected at construction. */
#include "a733_mmc_platform.h"
static enum sunxi_mmc_sd_profile selected_profile=SUNXI_MMC_SD_HIGH_SPEED4;
#include "mmc-high-speed-fixture.h"
#include "sunxi_smhc.h"
static unsigned closing,stall_update,fail_gate,gate_order;
static int close_write(void *v,uintptr_t a,uint32_t value)
{
    if(closing&&a==m.platform.module){
        assert(!(value&(1u<<31)));
        if(!gate_order){assert(value==0x01000003);gate_order=1;}
        else {assert(gate_order==3&&value==0);gate_order=4;}
        *pmic_model_word(&m.power,a)=value;return !fail_gate;
    }
    if(closing&&a==m.platform.bus){
        assert((gate_order==1&&value==0x10000)||(gate_order==2&&!value));
        ++gate_order;*pmic_model_word(&m.power,a)=value;return 1;
    }
    if(closing&&stall_update&&a==m.platform.base+SUNXI_SMHC_CMD){
        assert(value==SUNXI_SMHC_UPDATE);u.base.reg[SUNXI_SMHC_CMD/4]=value;return 1;
    }
    return uhs_platform_write(v,a,value);
}
static void setup_read(void)
{
    static uint8_t buffer[4096];
    closing=stall_update=fail_gate=gate_order=0;
    begin(2,1,2);assert(settle()==A733_MMC_READY);
    assert(a733_mmc_platform_close(&m.platform,9000000)==A733_MMC_INVALID);
    read_ok(buffer);m.platform.ops.write=close_write;closing=1;
}
static enum a733_mmc_platform_result close_step(void)
{m.power.now+=100;return a733_mmc_platform_close(&m.platform,9000000);}
int main(int argc,char **argv)
{
    assert(argc==1||(argc==2&&(!strcmp(argv[1],"--failures-only")||!strcmp(argv[1],"--admission"))));
    if(argc==2&&!strcmp(argv[1],"--admission")){
        static uint8_t buffer[4096];struct sunxi_mmc_result result;
        setup_read();u.base.reg[SUNXI_SMHC_DMAC/4]=0x80;
        assert(close_step()==A733_MMC_RETAINED&&!gate_order);retained();
        setup_read();m.power.worker=1;unsigned before=m.accesses;
        assert(close_step()==A733_MMC_INVALID&&m.accesses==before);m.power.worker=0;
        assert(a733_mmc_platform_read(&m.platform,8,8,buffer,sizeof(buffer),9000000)==A733_MMC_PENDING);
        before=m.accesses;assert(close_step()==A733_MMC_INVALID&&m.accesses==before);
        enum a733_mmc_platform_result r=A733_MMC_PENDING;
        for(unsigned i=0;i<400&&r!=A733_MMC_COMPLETE;++i)r=uhs_step();
        assert(r==A733_MMC_COMPLETE&&m.platform.core.borrowed==buffer);
        before=m.accesses;assert(close_step()==A733_MMC_INVALID&&m.accesses==before);
        assert(a733_mmc_platform_take(&m.platform,&result,9000000)==A733_MMC_READY);
        selected_profile=SUNXI_MMC_SD_UHS_SDR104;setup_read();
        before=m.accesses;assert(close_step()==A733_MMC_INVALID&&m.accesses==before);
        puts("PASS SD close admission: DMA enable alone, wrong owner, active/unreaped read and UHS refusal");return 0;
    }
    unsigned before;
    if(argc==1){
    setup_read();before=m.accesses;
    enum a733_mmc_platform_result r=A733_MMC_PENDING;
    for(unsigned i=0;i<12&&r!=A733_MMC_CLOSED;++i){r=close_step();assert(r!=A733_MMC_RETAINED);}
    assert(r==A733_MMC_CLOSED&&gate_order==4&&!m.platform.pin.id&&!m.platform.rail.id&&!m.platform.clock.id);
    assert(!*pmic_model_word(&m.power,m.platform.module)&&!*pmic_model_word(&m.power,m.platform.bus));
    assert(!(*pmic_model_word(&m.power,0x02000300)&0x0fffffff));
    assert(!(*pmic_model_word(&m.power,0x02000330)&0x3fff));
    printf("normal close MMIO callbacks: %u\n",m.accesses-before);
    before=m.accesses;assert(close_step()==A733_MMC_INVALID&&a733_mmc_platform_poll(&m.platform)==A733_MMC_INVALID&&m.accesses==before);
    setup_read();assert(close_step()==A733_MMC_PENDING);
    before=m.accesses;assert(a733_mmc_platform_close(&m.platform,9000001)==A733_MMC_INVALID&&m.accesses==before);
    m.power.now=9000000;assert(close_step()==A733_MMC_RETAINED);retained();
    setup_read();u.base.reg[SUNXI_SMHC_IDST/4]=1u<<4;
    assert(close_step()==A733_MMC_RETAINED&&!gate_order);retained();
    setup_read();u.base.reg[SUNXI_SMHC_DMAC/4]=SUNXI_SMHC_DMAC_ON;
    assert(close_step()==A733_MMC_RETAINED&&!gate_order);retained();
    }
    setup_read();u.base.busy=1;
    assert(close_step()==A733_MMC_RETAINED&&!gate_order);retained();
    setup_read();stall_update=1;assert(close_step()==A733_MMC_PENDING);
    assert(close_step()==A733_MMC_PENDING&&!gate_order);
    m.power.now=9000000;assert(close_step()==A733_MMC_RETAINED);retained();
    setup_read();fail_gate=1;assert(close_step()==A733_MMC_PENDING);
    assert(close_step()==A733_MMC_RETAINED&&gate_order==1&&m.platform.pin.id);retained();
    setup_read();assert(close_step()==A733_MMC_PENDING);assert(close_step()==A733_MMC_PENDING);
    assert(close_step()==A733_MMC_PENDING&&!m.platform.pin.id&&m.platform.rail.id);
    m.power.now=9000000;assert(close_step()==A733_MMC_RETAINED&&m.platform.rail.id&&m.platform.clock.id);retained();
    puts(argc==2?"PASS SD close: idle, update-stall, gate and partial-release failures":
         "PASS SD close: collected read, restoration, deadline identity, duplicate and retained failures");
    return 0;
}
