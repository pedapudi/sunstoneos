/* SPDX-License-Identifier: Apache-2.0 */
/* Actual board waiter and xHCI core on the real FreeRTOS kernel under the host shim
 * (checks/host/); deferred hardware effects follow the fixture's device clock, which
 * advances with the shim's virtual time. */
#define SUNSTONE_BOARD_APPLICATION_PRIVATE_H
#include "a733_usb2_native.h"
#include "gicv3_spi.h"
#ifndef configMAX_API_CALL_INTERRUPT_PRIORITY
#define configMAX_API_CALL_INTERRUPT_PRIORITY 11
#endif
#ifndef portPRIORITY_SHIFT
#define portPRIORITY_SHIFT 4
#endif
#include "host_rtos.h"
#include "register_latency.h"
#include <sys/mman.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct {void *handles[8];struct a733_usb2_native host;} board;
#define SUNSTONE_XHCI_FIXTURE_CORE board.host.core
#define main detach_fixture_main
#include "a733-xhci-detach.c"
#undef main
static unsigned waits,ready_steps,ready_waits,baseline,busy,stuck,return_case;
static unsigned rearms,drain_afters,stopped_source;
static volatile uint32_t *gic;
static uint64_t reset_delay=3000,run_delay=2000;
static struct register_latency reset_effect,run_effect;
static int board_live(void *v){(void)v;return 1;}
/* Time passes for the kernel and for the device model together. */
static void elapse(uint64_t us){host_advance_us(us);now+=us;}
/* The owner's only blocking wait is one tick, taken outside a critical section. */
uint32_t __real_ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks);
uint32_t __wrap_ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t ticks)
{
    if(ticks&&xTaskGetCurrentTaskHandle()==board.handles[4]){assert(!host_critical_depth()&&clear==pdTRUE&&ticks==1);++waits;}
    return __real_ulTaskGenericNotifyTake(index,clear,ticks);
}
int gicv3_spi_mask(const struct gicv3_spi *s)
{assert((host_critical_depth()||host_in_isr())&&s->intid==187);
 if(gic)gic[0x114/4]&=~BIT(27);return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *s)
{assert((host_critical_depth()||host_in_isr())&&s->intid==187);
 assert(!stopped_source);if(gic)gic[0x114/4]|=BIT(27);++rearms;return 1;}
#ifndef SUNSTONE_HOST_IRQ_SOURCE
#define SUNSTONE_HOST_IRQ_SOURCE "../apps/board/host_irq.c"
#endif
#include SUNSTONE_HOST_IRQ_SOURCE
static int delayed_read(void *c,uint32_t off,uint32_t *v)
{
    reg[0x40/4]=reg_latency_apply(&reset_effect,now,reg[0x40/4]);
    reg[0x44/4]=reg_latency_apply(&run_effect,now,reg[0x44/4]);
    return rd(c,off,v);
}
static int delayed_write(void *c,uint32_t off,uint32_t v)
{
    uint32_t prior=reg[0x40/4];
    int result=wr(c,off,v);
    if(off==0x40&&(v&2)){
        reg[off/4]|=2;reg_latency_schedule(&reset_effect,now,reset_delay,2,0);
    }else if(off==0x40&&(v&1)&&!(prior&1)){
        reg[0x44/4]|=1;reg_latency_schedule(&run_effect,now,run_delay,1,0);
    }else if(off==0x40&&!(v&1)&&(prior&1)){
        /* xHCI 1.2 section 5.4.1.1: events and slots drain before R/S clears;
         * section 5.4.1 permits up to 16 ms before HCH becomes visible. */
        assert(!slots&&!x.command_kind&&!a733_xhci_stop_draining(&x));
        reg_latency_schedule(&run_effect,now,2000,1,1);
    }
    return result;
}
static int delayed_live(void *c,uint64_t generation,uint64_t deadline)
{return busy?0:live(c,generation,deadline);}
static void owner_wait(void)
{
    unsigned ready=a733_xhci_work_pending(&x),before=waits;
    uint64_t waited=host_time_us();
    board_host_irq_wait();now+=host_time_us()-waited;elapse(1);
    if(ready){++ready_steps;ready_waits+=waits-before;}
    if(!baseline&&ready)assert(waits==before&&"ready software step slept for a tick");
}
static void initialize(void)
{
    struct device_memory_extent extent;mode="ready-work";
    assert(device_memory_pool_init(&pool,(uintptr_t)dma,PHYS,sizeof(dma))&&
        device_memory_acquire(&pool,99,sizeof(dma),&lease,&extent));
    reg[0]=0x01100040;reg[1]=(1u<<24)|(1u<<8)|3;
    reg[4]=(0x20u<<16)|1|4|8;reg[5]=0x2000;reg[6]=0x1000;
    reg[0x80/4]=0x02000002;reg[0x84/4]=0x20425355;
    reg[0x88/4]=0x101;reg[0x44/4]=1;reg[0x48/4]=1;
    const struct a733_xhci_ops ops={cn,current,delayed_live,delayed_read,delayed_write,wr64,barrier,outside,notice};
    assert(a733_xhci_init(&x,&ops,&pool,&pool,lease,7,now+100000,now+100000000));
    board.host.core_started=1;
}
static void return_isr(void *unused){(void)unused;board_host_irq();}
static void test_return(void)
{
    /* Exercise the native caller's real before -> stop -> after sequence.
     * board_host_irq_after calls interrupt_start on every drain pass. */
    if(return_case==1){
        irq_model=1;
        assert(board_host_irq_after()==1&&x.irq_mode&&irq.armed&&!irq.masked);
        assert(a733_xhci_stop_draining(&x));
    }else assert(!x.irq_mode&&!a733_xhci_stop_draining(&x));
    uint8_t data[512];memset(data,0x5a,sizeof(data));hold=UINT_MAX;
    assert(a733_xhci_transfer_now(&x,1,0x81,data,sizeof(data),now+1000000));
    uint64_t deadline=now+1000000;unsigned before=notice_count;
    int result=0;
    for(unsigned i=0;result==0&&i<1000;++i){
        elapse(100);command_step();
        if(return_case==1&&(gic[0x114/4]&BIT(27))&&(reg[0x1020/4]&1))
            host_isr(return_isr,NULL);
        if(a733_xhci_stop_draining(&x))assert(board_host_irq_before()==1);
        result=a733_xhci_stop(&x,deadline);assert(result>=0);
        if(a733_xhci_stop_draining(&x)){
            assert(board_host_irq_after()==1);++drain_afters;
        }else{
            stopped_source=1;unsigned calls=rearms;
            assert(a733_xhci_interrupt_start(&x)==-1);
            /* Even an erroneous unconditional wrapper call cannot rearm. */
            assert(board_host_irq_after()==-1&&rearms==calls);
        }
        if(result==0)assert(!x.failed);
    }
    assert(result==1&&!slots&&memory()==DEVICE_MEMORY_DEVICE&&notice_count==before);
    assert(stopped_source&&!a733_xhci_stop_draining(&x)&&!(reg[0x40/4]&1)&&(reg[0x44/4]&1));
    for(unsigned i=0;i<sizeof(data);++i)assert(data[i]==0x5a);
    if(return_case==1)assert(drain_afters>1&&rearms>1&&host_gives_total()>0);
    else assert(!drain_afters&&!rearms&&!x.irq_mode);
    printf("PASS composed board IRQ return: existing_binding=%u drain_afters=%u rearms=%u\n",
           return_case==1,drain_afters,rearms);
}
static void test(void *unused)
{
    (void)unused;
    assert(board.handles[4]==xTaskGetCurrentTaskHandle());
    uint64_t start=now;
    assert(a733_xhci_next_deadline(&x)==x.deadline);
    for(unsigned i=0;!portsc_reads&&!x.failed&&i<200;++i){
        enum a733_xhci_result r=a733_xhci_poll(&x);assert(r!=A733_XHCI_INVALID);
        if(stuck&&reset_effect.pending)reset_effect.stuck=1;
        owner_wait();
    }
    if(stuck){assert(x.failed&&memory()==DEVICE_MEMORY_RETAINED);puts("PASS stuck reset retains at deadline");return;}
    assert(portsc_reads&&(portsc&BIT(9))&&now-start>=reset_delay+run_delay);
    unsigned startup_waits=waits;uint64_t startup_us=now-start;
    /* Existing ring model enumerates the real core before scheduling checks. */
    attach(1);enumerate(1);open_bulk(1);
    if(return_case){test_return();return;}
    assert(!a733_xhci_work_pending(&x));owner_wait();
    uint8_t data[512];unsigned base=notice_count;
    assert(a733_xhci_transfer(&x,1,0x81,data,sizeof(data),now+1000000));
    assert(a733_xhci_next_deadline(&x)==now+1000000);
    assert(a733_xhci_work_pending(&x));owner_wait();
    assert(a733_xhci_poll(&x)==A733_XHCI_READY);
    assert(x.devices[0].endpoints[1].pending==2&&!a733_xhci_work_pending(&x));
    uint64_t complete_at=now+3500;
    while(now<complete_at){assert(a733_xhci_poll(&x)==A733_XHCI_READY);owner_wait();}
    assert(notice_count==base);transfer_step();
    assert(a733_xhci_poll(&x)==A733_XHCI_READY&&notice_count==base+1);
    for(unsigned i=0;i<sizeof(data);++i)assert(data[i]==(uint8_t)(pattern+i));
    /* New endpoint configuration queues a command; its hardware completion
     * stays absent for several ticks, independently of task iterations. */
    assert(a733_xhci_open_interrupt(&x,1,0x83,16,4,now+1000000));
    assert(a733_xhci_work_pending(&x));owner_wait();
    assert(a733_xhci_poll(&x)==A733_XHCI_READY&&x.command_kind);
    assert(!a733_xhci_work_pending(&x));complete_at=now+4500;
    while(now<complete_at){assert(a733_xhci_poll(&x)==A733_XHCI_READY);owner_wait();}
    assert(x.command_kind);command_step();
    assert(a733_xhci_poll(&x)==A733_XHCI_READY&&!x.command_kind);
    assert(a733_xhci_next_deadline(&x)==x.lifetime_deadline);
    printf("startup_us=%llu startup_waits=%u ready_steps=%u ready_waits=%u\n",
        (unsigned long long)startup_us,startup_waits,ready_steps,ready_waits);
    /* A truthful ready hint with a contended guard cannot spin forever. */
    assert(a733_xhci_transfer(&x,1,0x81,data,sizeof(data),now+100000));
    busy=1;irq.burst=0;unsigned before=waits;
    for(unsigned i=0;i<32;++i){assert(a733_xhci_poll(&x)==A733_XHCI_BUSY);board_host_irq_wait();}
    assert(waits==before+(baseline?32:1));
    elapse(100000);assert(a733_xhci_poll(&x)==A733_XHCI_RETAINED&&memory()==DEVICE_MEMORY_RETAINED);
    assert(!a733_xhci_work_pending(&x));
    puts("PASS delayed reset/run/command/TD, idle enqueue, bounded BUSY and deadline retention");
}
int main(int argc,char **argv)
{
    baseline=argc>1&&!strcmp(argv[1],"baseline");
    stuck=argc>1&&!strcmp(argv[1],"stuck");
    if(argc>1&&!strcmp(argv[1],"long-delay")){reset_delay=7000;run_delay=5000;}
    if(argc>1&&!strcmp(argv[1],"return-irq"))return_case=1;
    if(argc>1&&!strcmp(argv[1],"return-no-irq"))return_case=2;
    initialize();
    host_set_time_us(now);
    board.handles[4]=host_rtos_prepare(test,NULL,2,1000000);
    if(return_case){
        gic=mmap((void *)0x03400000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        assert(gic==(void *)0x03400000);gic[0x4b8/4]=0xb0000000;
        assert(board_host_irq_bind());
    }
    host_rtos_start();
    return 0;
}
