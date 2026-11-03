/* Existing card/PMIC fixture, with HIGH_SPEED4 selected at construction. */
#include "a733_mmc_platform.h"
#include "a733_mmc_platform_native.h"
#include "a733_platform_locks.h"
static enum sunxi_mmc_sd_profile selected_profile=SUNXI_MMC_SD_HIGH_SPEED4;
#include "mmc-high-speed-fixture.h"
#include "sunxi_smhc.h"
static unsigned closing,stall_update,fail_gate,gate_order;
static int close_write(void *v,uintptr_t a,uint32_t value)
{
    if(closing&&a==m.platform.module){
        /* The native adapter's own rule for this write (include/a733_mmc_platform_native.h), applied to the model's registers: the board's
         * adapter refused the UHS-I close's restore of the inherited module setting, which the platform's checks did not see. */
        assert(a733_mmc_platform_native_module_write_allowed(SUNXI_MMC_SD,m.platform.config.sd_profile,*pmic_model_word(&m.power,a),value,
                                                            *pmic_model_word(&m.power,m.platform.bus)));
        assert(!(value&(1u<<31)));
        if(!gate_order){assert(value==(m.platform.running_module&~(1u<<31)));gate_order=1;}
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
/* The native adapter's own register filter (drivers/storage/a733_mmc_platform_native.c, built for the host) applied to every register write of a
 * lifetime. The models write through their own hook; the board writes through the adapter, which refused the UHS-I close's last write and which no
 * host check had applied. Each write here is first offered to a shadow adapter with the model's current module, bus and clock-control words. */
uint32_t a733_native_host_load(uintptr_t a)
{
    if(a==m.platform.module||a==m.platform.bus)return *pmic_model_word(&m.power,a);
    if(a>=m.platform.base&&a<m.platform.base+0x300)return u.base.reg[(a-m.platform.base)/4];     /* the model's controller registers */
    return 0;
}
_Noreturn void platform_panic(const char *file,unsigned line){fprintf(stderr,"platform panic %s:%u\n",file,line);abort();}
static unsigned shadow_stores;
void a733_native_host_store(uintptr_t a,uint32_t v){(void)a;(void)v;++shadow_stores;}
int a733_native_host_current(void *n){(void)n;return 1;}
int a733_platform_lock_enter(struct a733_platform_locks *l,enum a733_platform_lock k,uint64_t d){(void)l;(void)k;(void)d;return 1;}
void a733_platform_lock_leave(struct a733_platform_locks *l,enum a733_platform_lock k){(void)l;(void)k;}
int a733_platform_lock_retain(struct a733_platform_locks *l,enum a733_platform_lock k){(void)l;(void)k;return 1;}
static struct a733_platform_locks shadow_locks;
static struct a733_mmc_platform_native shadow;
static int shadow_task;
static int shadow_admit(void *c,const struct a733_mmc_platform_config *cfg,const struct a733_clock_token *t){(void)c;(void)cfg;(void)t;return 1;}
static void shadow_begin(void)
{
    memset(&shadow_locks,0,sizeof shadow_locks);shadow_locks.initialized=1;memset(&shadow,0,sizeof shadow);
    assert(a733_mmc_platform_native_init_selected(&shadow,&shadow_locks,&shadow_task,SUNXI_MMC_SD,A733_MMC_EMMC_SDC2,shadow_admit,NULL));
    shadow.sd_profile=m.platform.config.sd_profile;assert(shadow.operations.enter(&shadow,1000));
}
static unsigned shadow_refusals;
static int shadow_write(void *v,uintptr_t a,uint32_t value)
{
    shadow_stores=0;
    if(!shadow.operations.write(&shadow,a,value)){
        ++shadow_refusals;
        if(getenv("SHADOW_DEBUG"))fprintf(stderr,"adapter would refuse a write of 0x%x to 0x%lx\n",(unsigned)value,(unsigned long)a);
        return 0;
    }
    return close_write(v,a,value);
}
static void setup_read(void)
{
    static uint8_t buffer[4096];
    closing=stall_update=fail_gate=gate_order=0;cfg_dma=0;
    begin(2,1,2);assert(settle()==A733_MMC_READY);
    assert(a733_mmc_platform_close(&m.platform,9000000)==A733_MMC_INVALID);
    read_ok(buffer);m.platform.ops.write=close_write;closing=1;
}
static enum a733_mmc_platform_result close_step(void)
{m.power.now+=100;return a733_mmc_platform_close(&m.platform,9000000);}
/* The UHS-I lifetimes of docs/designs/sd-uhs-recovery.md. Lifetime 1 switches the card to 1.8 V, runs SDR25, reads and closes with the bank
 * put back at 3.3 V; lifetime 2 starts with the card already at 1.8 V, moves the bank to 1.8 V before the core opens, identifies without
 * CMD11, reads and closes the same way. run_lifetime stops ("a cut") after `cut` polls and checks the state a watchdog reset leaves. */
/* variant 0 SDR25 only; 1 SDR104 at 150 MHz; 2 SDR104 at 100 MHz (no 300 MHz branch); 3 SDR104 with no passing delay, which falls back to SDR25 at 50 MHz. */
static int run_lifetime(unsigned start18,int cut,unsigned variant)
{
    unsigned sdr104=variant!=0;cfg_nowindow=variant==3;cfg_dma=variant==5;
    static uint8_t buffer[4096];struct sunxi_mmc_result result;unsigned n=0,uhs_pf_value;
    selected_profile=SUNXI_MMC_SD_UHS_SDR104;cfg_sdr25=!sdr104;cfg_start18=start18;
    closing=stall_update=fail_gate=gate_order=0;
#define TICK() do{if((int)n++==cut)goto cut_point;}while(0)
    if(variant==2)begin(1,0,1);else begin(1,1,3);
    if(variant==4)u.sdr50=1;       /* a card with SDR50 as well: the restarts walk SDR104, SDR50, SDR25 */
    shadow_begin();shadow_refusals=0;m.platform.ops.write=shadow_write;
    enum a733_mmc_platform_result r=A733_MMC_PENDING;
    for(unsigned i=0;i<40000&&r!=A733_MMC_READY;++i){TICK();r=uhs_step();assert(r==A733_MMC_PENDING||r==A733_MMC_READY);}
    assert(r==A733_MMC_READY);
    if(variant==3){
        /* No passing delay: the sweep ran, the card goes back to SDR25 at 50 MHz and 1.8 V with no power cycle. */
        assert(m.platform.core.card.signal_1v8&&m.platform.core.card.timing==1&&m.platform.core.card.card_hz==50000000&&
               m.platform.core.card.uhs_outcome==SUNXI_MMC_UHS_NO_WINDOW&&m.platform.core.card.reentered==start18&&u.cmd19&&pf_writes==1&&m.platform.signal==2);
    }else if(sdr104){
        /* SDR104 after the CMD19 sweep (150 MHz, or 100 MHz without the 300 MHz branch): the window of the card model is delays 20 to 43, midpoint 32. */
        assert(m.platform.core.card.signal_1v8&&m.platform.core.card.timing==3&&m.platform.core.card.card_hz==(variant==2?100000000u:150000000u)&&
               m.platform.core.card.uhs_outcome==SUNXI_MMC_UHS_SDR104&&m.platform.core.card.sample_delay==32&&
               m.platform.core.card.reentered==start18&&u.cmd11==(start18?0u:1u)&&pf_writes==1&&m.platform.signal==2&&u.cmd19&&u.cmd6_uhs==2);       /* CMD6 SDR104 check, then set */
    }else
    assert(m.platform.core.card.signal_1v8&&m.platform.core.card.timing==1&&m.platform.core.card.card_hz==50000000&&
           m.platform.core.card.uhs_outcome==SUNXI_MMC_UHS_LIMITED&&m.platform.core.card.reentered==start18&&
           u.cmd11==(start18?0u:1u)&&pf_writes==1&&m.platform.signal==2&&!u.cmd19&&!u.cmd6_uhs);
    assert(m.platform.core.card.switch_status[13]&8);                                  /* SDR104 support, visible at 1.8 V */
    if(variant!=5){      /* the descriptor-DMA variant has no IDMAC model: it identifies, restarts and closes, and never reads */
    assert(a733_mmc_platform_read(&m.platform,8,8,buffer,sizeof(buffer),9000000)==A733_MMC_PENDING);
    r=A733_MMC_PENDING;
    for(unsigned i=0;i<400&&r!=A733_MMC_COMPLETE;++i){TICK();r=uhs_step();assert(r!=A733_MMC_RETAINED);}
    assert(r==A733_MMC_COMPLETE&&a733_mmc_platform_take(&m.platform,&result,9000000)==A733_MMC_READY&&result.received==4096);
    }
    if(variant==4||variant==5){
        /* A restart of the identification (what the recovery policy does after a failed read), with every register write offered to the real adapter filter:
         * the card is re-entered at 1.8 V with no CMD11 and no new PF write, tuned again, and read again. */
        assert(a733_mmc_platform_restart(&m.platform,9000000,3,SUNXI_MMC_MODE_RETUNE)==A733_MMC_PENDING);
        r=A733_MMC_PENDING;
        for(unsigned i=0;i<40000&&r!=A733_MMC_READY;++i){TICK();r=uhs_step();assert(r==A733_MMC_PENDING||r==A733_MMC_READY);}
        assert(r==A733_MMC_READY&&m.platform.core.card.timing==3&&m.platform.core.card.reentered&&u.cmd11==(start18?0u:1u)&&pf_writes==1&&m.platform.signal==2&&
               u.cmd6_uhs==4&&m.platform.core.card.mode_steps==1&&m.platform.core.card.sample_delay==32);
        if(variant==4){
        assert(a733_mmc_platform_read(&m.platform,8,8,buffer,sizeof(buffer),9000000)==A733_MMC_PENDING);
        r=A733_MMC_PENDING;
        for(unsigned i=0;i<400&&r!=A733_MMC_COMPLETE;++i){TICK();r=uhs_step();assert(r!=A733_MMC_RETAINED);}
        assert(r==A733_MMC_COMPLETE&&a733_mmc_platform_take(&m.platform,&result,9000000)==A733_MMC_READY&&result.received==4096);
        }
    }
    if(variant==4){
        /* The ceilings SDR50 and then SDR25: the CMD6 for function 2 must pass the adapter's command rule (the first restart probe on the board stopped on it). */
        static const unsigned ceilings[2]={2,1};
        for(unsigned step=0;step<2;++step){
            assert(a733_mmc_platform_restart(&m.platform,9000000,ceilings[step],SUNXI_MMC_MODE_ERRORS)==A733_MMC_PENDING);
            r=A733_MMC_PENDING;
            for(unsigned i=0;i<40000&&r!=A733_MMC_READY;++i){TICK();r=uhs_step();assert(r==A733_MMC_PENDING||r==A733_MMC_READY);}
            assert(r==A733_MMC_READY&&m.platform.core.card.signal_1v8&&u.cmd11==(start18?0u:1u)&&pf_writes==1&&m.platform.signal==2);
            assert(m.platform.core.card.timing==(step?1u:2u)&&m.platform.core.card.card_hz==(step?50000000u:100000000u));
            assert(a733_mmc_platform_read(&m.platform,8,8,buffer,sizeof(buffer),9000000)==A733_MMC_PENDING);
            r=A733_MMC_PENDING;
            for(unsigned i=0;i<400&&r!=A733_MMC_COMPLETE;++i){TICK();r=uhs_step();assert(r!=A733_MMC_RETAINED);}
            assert(r==A733_MMC_COMPLETE&&a733_mmc_platform_take(&m.platform,&result,9000000)==A733_MMC_READY&&result.received==4096);
        }
    }
    if(variant==5){assert(!shadow_refusals&&!commands_at_mismatch);return (int)n;}   /* no read happened, so the platform does not close (close_ready) */
    closing=1;
    for(unsigned i=0;i<40&&r!=A733_MMC_CLOSED;++i){TICK();r=close_step();
        if(r!=A733_MMC_PENDING&&r!=A733_MMC_CLOSED&&getenv("PF_RESTORE"))fprintf(stderr,"close retained: stage %u fault line %u why %u signal %u\n",m.platform.stage,m.platform.fault_line,m.platform.fault_why,m.platform.signal);
        assert(r==A733_MMC_PENDING||r==A733_MMC_CLOSED);}
    assert(r==A733_MMC_CLOSED&&gate_order==4&&!m.platform.pin.id&&!m.platform.rail.id&&!m.platform.clock.id);
    assert(restore_writes==1&&m.platform.signal==0&&(*pmic_model_word(&m.power,0x02000070)&1)==1&&
           ((*pmic_model_word(&m.power,0x02000048)>>10)&3)==2&&u.phase==3&&u.bank==0&&pf_writes==1);
    assert(!*pmic_model_word(&m.power,m.platform.module)&&!*pmic_model_word(&m.power,m.platform.bus));
    assert(!commands_at_mismatch&&!shadow_refusals);
    return (int)n;
cut_point:
    /* What a reset here leaves: the selector, the card's level and the platform's record agree, and no command went out at a mismatch. */
    uhs_pf_value=*pmic_model_word(&m.power,0x02000070)&1;
    assert(!commands_at_mismatch&&pf_writes<=1&&restore_writes<=1);
    if(!uhs_pf_value)assert(u.phase>=2&&u.bank>=1&&u.bank<=2);                           /* bank at 1.8 V only after or during the card's switch */
    if(m.platform.signal==1||m.platform.signal==2)assert(!uhs_pf_value);
    if(m.platform.signal==3)assert(uhs_pf_value&&restore_writes==1);
    if(!m.platform.signal&&!restore_writes)assert(uhs_pf_value&&!pf_writes);
    return -1;
#undef TICK
}
int main(int argc,char **argv)
{
    assert(argc==1||(argc==2&&(!strcmp(argv[1],"--failures-only")||!strcmp(argv[1],"--admission")||!strcmp(argv[1],"--lifetimes"))));
    if(argc==2&&!strcmp(argv[1],"--lifetimes")){
        unsigned cuts=0;
        for(unsigned variant=0;variant<6;++variant)for(unsigned start18=0;start18<2;++start18){
            unsigned sdr104=variant!=0;
            int total=run_lifetime(start18,-1,variant);assert(total>0);
            /* every poll for SDR25; for SDR104 every poll except inside the 520-command sweep, where every 9th */
            for(int cut=0;cut<total;cut+=(sdr104&&cut>300&&cut<total-60)?9:1){assert(run_lifetime(start18,cut,variant)==-1);++cuts;}
        }
        /* A UHS-I lifetime that stayed at 3.3 V (manual PF mode, no switch) closes too. */
        selected_profile=SUNXI_MMC_SD_UHS_SDR104;cfg_sdr25=cfg_start18=cfg_dma=0;closing=stall_update=fail_gate=gate_order=0;
        begin(2,1,2);assert(settle()==A733_MMC_READY&&!m.platform.signal);
        {static uint8_t b[4096];read_ok(b);}
        m.platform.ops.write=close_write;closing=1;enum a733_mmc_platform_result r=A733_MMC_PENDING;
        for(unsigned i=0;i<40&&r!=A733_MMC_CLOSED;++i){r=close_step();assert(r!=A733_MMC_RETAINED);}
        assert(r==A733_MMC_CLOSED&&!restore_writes&&!pf_writes);
        printf("PASS UHS-I lifetimes: switch, SDR25, SDR104 at 150 and at 100 MHz (CMD19 sweep) and the no-window fallback to SDR25, reads and close with the bank back at 3.3 V; re-entry from a card at 1.8 V with no CMD11; %u cuts (every poll, every 9th inside a CMD19 sweep), "
               "no command with the bank at 1.8 V and the card at 3.3 V, the selector and the platform's record agree at every cut\n",cuts);
        return 0;
    }
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
        puts("PASS SD close admission: DMA enable alone, wrong owner, active/unreaped read");return 0;
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
    /* A pin release that fails: the pins' live fields no longer match the lease (the mux changed behind the owner). The close retains every
     * remaining resource: pins, rail and clock stay held, and the close does not go on to release the rail. */
    setup_read();assert(close_step()==A733_MMC_PENDING);assert(close_step()==A733_MMC_PENDING);
    *pmic_model_word(&m.power,0x02000300)|=1u;
    assert(close_step()==A733_MMC_RETAINED&&m.platform.pin.id&&m.platform.rail.id&&m.platform.clock.id);retained();
    puts(argc==2?"PASS SD close: idle, update-stall, gate and partial-release failures":
         "PASS SD close: collected read, restoration, deadline identity, duplicate and retained failures");
    return 0;
}
