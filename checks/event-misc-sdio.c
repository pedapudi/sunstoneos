/* Real controller, event owner and deadline runtime. The register model uses
 * manual 11.3.8.13-17 FIFO watermark and W1C semantics. */
#include "host_rtos.h"
#include "sunstone_deadline.h"
#include "a733_radio_events.h"
#include "sunxi_smhc.h"
#include "a733_range.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct a733_radio_platform platform;
static struct a733_radio_events events;
static struct {
    uint32_t regs[256],raw;
    unsigned enabled,pending,active,clears,masks,accesses,bytes,queued,consumed,fill;
    unsigned writing,data_active,drop_command,hold_data,busy,late_error,fail_pending,race;
    uint64_t rwp_until,rwp_delay;
} model;
static void raise_if_ready(void);
static void device_irq(void *unused)
{
    (void)unused;
    if(!model.enabled)return;
    model.active=1;model.pending=0;a733_radio_events_irq(&events);model.active=0;
}
static void refresh(void)
{
    if(model.data_active){
        if(model.writing&&model.fill<= (model.regs[SUNXI_SMHC_FTRGL/4]&0x7ffu)&&
           (model.regs[SUNXI_SMHC_FTRGL/4]&0x7ffu))model.raw|=1u<<4;
        if(!model.writing&&(model.fill>((model.regs[SUNXI_SMHC_FTRGL/4]>>16)&0x7ffu)||
           (model.fill&&model.queued==model.bytes)))model.raw|=1u<<5;
    }
}
static void raise_if_ready(void)
{
    refresh();
    if((model.regs[0]&SUNXI_SMHC_GCTRL_INT_ENABLE)&&(model.raw&model.regs[SUNXI_SMHC_IMASK/4])){
        model.pending=1;
        if(model.enabled)host_irq_at(host_time_us(),device_irq,NULL);
    }
}
static uint32_t event_read(uintptr_t a)
{
    switch(a-UINT32_C(0x03400000)){
    case 0:return host_time_us()<model.rwp_until?1u<<31:0;
    case 0x98:case 0xd18:return 0; /* Nonsecure group registers are RAZ. */
    case 0xc30:return 0;
    case 0x4c0:return 0xb0u<<16;
    case 0x118:return model.enabled?4:0;
    case 0x218:return model.pending?4:0;
    case 0x318:return model.active?4:0;
    default:assert(0);return 0;
    }
}
static uint64_t event_route(uintptr_t a){assert(a==0x03406610);return 0;}
static void event_write(uintptr_t a,uint32_t value)
{
    assert(a==0x03400298&&value==4);
    assert(!model.active&&!model.regs[SUNXI_SMHC_IMASK/4]&&!(model.regs[0]&SUNXI_SMHC_GCTRL_INT_ENABLE));
    ++model.clears;if(!model.fail_pending)model.pending=0;
}
int gicv3_spi_mask(const struct gicv3_spi *irq)
{
    assert(irq->intid==194);++model.masks;model.enabled=0;
    model.rwp_until=host_time_us()+model.rwp_delay;return 1;
}
int gicv3_spi_masked(const struct gicv3_spi *irq)
{assert(irq->intid==194);return host_time_us()<model.rwp_until?0:model.enabled?-1:1;}
int gicv3_spi_rearm(const struct gicv3_spi *irq)
{
    assert(irq->intid==194);
    if(host_time_us()<model.rwp_until)return 0;
    model.enabled=1;
    if(model.race){model.race=0;model.raw|=1u<<16;}
    raise_if_ready();return 1;
}
#define A733_RADIO_EVENTS_MMIO_DEFINED
#include "radio-events.inc"
static int current(void *unused)
{(void)unused;return !host_in_isr()&&xTaskGetCurrentTaskHandle()==events.owner;}
static int now(void *unused,uint64_t *out){(void)unused;*out=host_time_us();return 1;}
static int barrier(void *unused){(void)unused;assert(!host_in_isr());return 1;}
static int timely(struct a733_radio_platform *p)
{p->last_now=host_time_us();return p->last_now<p->deadline;}
static int fail(struct a733_radio_platform *p)
{p->fault=1;sunxi_sdio_stop(&p->host,SUNXI_SDIO_IO_ERROR);return 0;}
static int finish(struct a733_radio_platform *p,int result)
{atomic_store(&p->busy,0);return result;}
#define SDC UINT32_C(0x04021000)
#include "radio-host-access.inc"
static void update_done(void *unused){(void)unused;model.regs[SUNXI_SMHC_CMD/4]&=~SUNXI_SMHC_CMD_START;}
static void fifo_clock(void *unused)
{
    (void)unused;
    if(!model.data_active)return;
    unsigned bytes=200;
    if(model.writing){
        if(bytes>model.fill*4)bytes=model.fill*4;
        model.fill-=bytes/4;model.consumed+=bytes;
        if(model.consumed==model.bytes){model.data_active=0;if(!model.hold_data)model.raw|=8;}
    }else{
        if(bytes>model.bytes-model.queued)bytes=model.bytes-model.queued;
        if(bytes/4>256-model.fill){model.raw|=1u<<11;model.data_active=0;}
        else {model.queued+=bytes;model.fill+=bytes/4;}
        if(model.queued==model.bytes&&!model.hold_data)model.raw|=8;
    }
    raise_if_ready();
    if(model.data_active&&(model.writing||model.queued<model.bytes))host_irq_at(host_time_us()+16,fifo_clock,NULL);
}
static void command_done(void *unused)
{(void)unused;if(!model.drop_command)model.raw|=4;raise_if_ready();}
static int read32(void *unused,uintptr_t a,uint32_t *out)
{
    (void)unused;assert(!host_in_isr());++model.accesses;
    unsigned off=(unsigned)(a-SDC);assert(off<=0x300&&!(off&3));
    if(off==0)model.regs[0]&=~7u;
    if(off==SUNXI_SMHC_RINT){refresh();*out=model.raw;return 1;}
    if(off==SUNXI_SMHC_STATUS){
        *out=(model.fill<<17)|(!model.fill?4u:0)|(model.fill==256?8u:0)|(model.busy?1u<<9:0);return 1;
    }
    if(off==SUNXI_SMHC_FIFO){
        assert(!model.writing&&model.fill);--model.fill;model.consumed+=4;*out=0x03020100;
        if(model.late_error&&model.consumed==model.bytes)model.raw|=1u<<7;
        return 1;
    }
    *out=model.regs[off/4];return 1;
}
static int write32(void *unused,uintptr_t a,uint32_t value)
{
    (void)unused;assert(!host_in_isr());++model.accesses;
    unsigned off=(unsigned)(a-SDC);assert(off<=0x300&&!(off&3));
    if(off==SUNXI_SMHC_RINT){model.raw&=~value;return 1;}
    if(off==SUNXI_SMHC_FIFO){assert(model.writing&&model.fill<256);++model.fill;model.queued+=4;return 1;}
    model.regs[off/4]=value;
    if(off==SUNXI_SMHC_GCTRL&&(value&2))model.fill=0;
    if(off==SUNXI_SMHC_CMD){
        if(value&SUNXI_SMHC_CMD_UPDATE_CLOCK){host_irq_at(host_time_us()+10,update_done,NULL);return 1;}
        model.regs[off/4]&=~SUNXI_SMHC_CMD_START;
        model.bytes=model.regs[SUNXI_SMHC_BCNT/4];
        model.queued=model.consumed=model.fill=0;model.writing=(value&SUNXI_SMHC_CMD_WRITE)!=0;
        model.data_active=(value&SUNXI_SMHC_CMD_DATA)!=0;
        host_irq_at(host_time_us()+20,command_done,NULL);
        if(model.data_active)host_irq_at(host_time_us()+32,fifo_clock,NULL);
    }
    return 1;
}
static void poll_core(void)
{platform.core_call=1;sunxi_sdio_poll(&platform.host);platform.core_call=0;}
static void complete(void)
{
    for(unsigned passes=0;platform.host.state!=SUNXI_SDIO_DONE&&platform.host.state!=SUNXI_SDIO_FAULT;++passes){
        assert(passes<300);poll_core();
        if(platform.host.state!=SUNXI_SDIO_DONE&&platform.host.state!=SUNXI_SDIO_FAULT)
            assert(a733_radio_events_wait(&events,platform.deadline));
    }
}
static void function_irq(void *unused){(void)unused;model.raw|=1u<<16;raise_if_ready();}
static void idle_observation(void *unused)
{
    (void)unused;assert(events.wakes==0&&events.interrupts==0&&eTaskGetState(events.owner)==eBlocked);
}
static void owner(void *unused)
{
    unsigned mode=(unsigned)(uintptr_t)unused;
    if(mode==1){
        model.pending=1;unsigned accesses=model.accesses;
        assert(!a733_radio_events_begin(&events)&&a733_radio_events_retire(&events)==-1);
        assert(model.pending&&!model.clears&&!model.masks&&model.accesses==accesses);return;
    }
    assert(a733_radio_events_begin(&events));
    assert(sunxi_sdio_prepare(&platform.host,platform.deadline));
    platform.stage=RSTAGE_PREPARING;
    while(platform.host.state!=SUNXI_SDIO_READY){poll_core();if(platform.host.state!=SUNXI_SDIO_READY)assert(a733_radio_events_wait(&events,platform.deadline));}
    assert(model.regs[SUNXI_SMHC_FTRGL/4]==0x000f00f0);
    platform.stage=RSTAGE_READY;platform.borrower.generation=1;
    if(mode==2||mode==3){
        static uint8_t data[4];
        struct sunxi_sdio_request r={.command=52};
        if(mode==2)model.drop_command=1;
        else {r=(struct sunxi_sdio_request){.command=53,.argument=(1u<<28)|4,.data=data,.bytes=4};model.late_error=1;}
        assert(sunxi_sdio_submit(&platform.host,&r,host_time_us()+100));complete();
        assert(platform.host.state==SUNXI_SDIO_FAULT&&platform.host.request.command==r.command);
        assert(platform.host.result.error==(mode==2?SUNXI_SDIO_TIMEOUT:SUNXI_SDIO_CONTROLLER_ERROR));
        unsigned accesses=model.accesses;poll_core();assert(model.accesses==accesses);
        assert(a733_radio_events_retire(&events)==1);return;
    }
    events.wakes=events.interrupts=events.deadline_wakes=0;
    host_run_at(host_time_us()+1000000,idle_observation,NULL);
    uint64_t wake=host_time_us()+2000000;host_irq_at(wake,function_irq,NULL);
    assert(a733_radio_events_wait(&events,platform.deadline)&&host_time_us()==wake);
    assert(events.wakes==1&&events.interrupts==1&&!events.deadline_wakes);poll_core();
    assert(platform.host.sdio_pending);
    /* Function16 stays latched across a command; only its own IMASK bit is suppressed. */
    struct sunxi_sdio_request request={.command=52};struct sunxi_sdio_result result;
    assert(sunxi_sdio_submit(&platform.host,&request,host_time_us()+10000));complete();
    assert(platform.host.state==SUNXI_SDIO_DONE&&!(model.regs[SUNXI_SMHC_IMASK/4]&(1u<<16)));
    assert(sunxi_sdio_take_result(&platform.host,&result));
    /* TX exceeds one FIFO and gets no repeated CMD_DONE. Watermark IRQs keep it moving. */
    static uint8_t bytes[2048];
    request=(struct sunxi_sdio_request){.command=53,.argument=(1u<<31)|(1u<<28)|(1u<<27)|4,.block_bytes=512,.data=bytes,.bytes=sizeof(bytes)};
    assert(sunxi_sdio_submit(&platform.host,&request,host_time_us()+10000));complete();
    assert(platform.host.state==SUNXI_SDIO_DONE&&model.queued==sizeof(bytes)&&model.consumed==sizeof(bytes));
    assert(!(model.regs[SUNXI_SMHC_IMASK/4]&((1u<<16)|(1u<<4)))&&platform.host.sdio_pending);
    assert(sunxi_sdio_take_result(&platform.host,&result)&&result.transferred==sizeof(bytes));
    model.raw&=~(1u<<16);assert(sunxi_sdio_ack_irq(&platform.host)&&!platform.host.sdio_pending);
    /* CMD_DONE arrives before this short RX packet. Packet end raises RX below15words. */
    request=(struct sunxi_sdio_request){.command=53,.argument=(1u<<28)|4,.data=bytes,.bytes=4};
    assert(sunxi_sdio_submit(&platform.host,&request,host_time_us()+1000));complete();
    assert(platform.host.state==SUNXI_SDIO_DONE&&result.error==0&&model.consumed==4);
    assert(sunxi_sdio_take_result(&platform.host,&result));
    /* A function event races source rearm, before the wait registers its timer. */
    model.race=1;uint64_t before=host_time_us();
    assert(a733_radio_events_wait(&events,platform.deadline)&&host_time_us()==before);poll_core();
    model.raw&=~(1u<<16);assert(sunxi_sdio_ack_irq(&platform.host));
    /* One disable is issued across a delayed RWP, even when observations retry. */
    model.rwp_delay=120;unsigned masks=model.masks;
    assert(a733_radio_events_wait(&events,host_time_us()+50));
    assert(a733_radio_events_wait(&events,host_time_us()+50));
    assert(a733_radio_events_wait(&events,host_time_us()+50));
    assert(model.masks==masks+1);model.rwp_delay=0;
    /* Startup and stopping use the real host_access core_call admission. */
    platform.stop_requested=1;platform.stage=RSTAGE_SHUTDOWN_CONTROLLER_WAIT;
    assert(sunxi_sdio_quiesce(&platform.host,host_time_us()+1000));
    while(platform.host.state!=SUNXI_SDIO_QUIESCED){poll_core();if(platform.host.state!=SUNXI_SDIO_QUIESCED)assert(a733_radio_events_wait(&events,platform.deadline));}
    model.active=1;model.pending=1;
    assert(a733_radio_events_retire(&events)==0&&!model.clears);
    model.active=0;model.fail_pending=1;assert(a733_radio_events_retire(&events)==0);
    model.fail_pending=0;assert(a733_radio_events_retire(&events)==1&&events.retired&&!model.pending);
    assert(!(model.regs[0]&SUNXI_SMHC_GCTRL_INT_ENABLE)&&!model.regs[SUNXI_SMHC_IMASK/4]);
}
int main(int argc,char **argv)
{
    platform.initialized=1;platform.deadline=5000000;platform.config.lifetime_deadline_us=5000000;
    platform.ops=(struct a733_radio_platform_ops){.now_us=now,.current=current,.read=read32,.write=write32,.barrier=barrier};
    struct sunxi_sdio_config config={{host_read,host_write,host_barrier,host_time,NULL},&platform,800000};
    assert(sunxi_sdio_init(&platform.host,&config));model.regs[SUNXI_SMHC_VERSION/4]=0x50300;
    unsigned mode=argc>1?(!strcmp(argv[1],"missing")?2:!strcmp(argv[1],"late-error")?3:1):0;
    TaskHandle_t task=host_rtos_prepare(owner,(void *)(uintptr_t)mode,4,6000000);
    assert(a733_radio_events_init(&events,&platform,task));host_timer_handler(sunstone_deadline_irq);host_rtos_start();
    puts("PASS SDIO event idle, command/FIFO, rearm race and retained retirement");
}
