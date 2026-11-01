/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "sunstone_probe.h"
#include "sunstone_deadline.h"
#include "sunstone_board.h"
#include "a733_twi.h"
#include "a733_clock_reference.h"
#include "diagnostic.h"
#include <stdatomic.h>
#ifndef USB_VBUS_MODE
#define USB_VBUS_MODE 0
#endif
__attribute__((used,retain)) const uint64_t usb_vbus_probe_profile[]={UINT64_C(0x3153554256535355),1,USB_VBUS_MODE};
struct report {
    uint64_t magic,mode,failure,state,rose,fell,rise_us,fall_us,max_gap,cc_samples;
    uint64_t guard_gap,guard_fault,on_us,pl8_cfg,pl8_dat,twi_state,twi_status,rate_hz,divider,bus_retired;
};
#define OUT ((volatile struct report *)(uintptr_t)0x42000100)
SUNSTONE_PROBE_IMAGE(0x42000100,sizeof(struct report),0,0,20);
#define PIO UINT32_C(0x07025000)
#define TWI UINT32_C(0x07084000)
#define BGR UINT32_C(0x0701019c)
static struct a733_twi bus;
static struct vbus_diagnostic diagnostic;
static const struct sunstone_usb_port *port;
static StaticTask_t tcbs[2];
static StackType_t stacks[2][2048];
static TaskHandle_t owner;
static atomic_uint revoked,stop_guard,guard_parked,guard_fault;
static atomic_uint_fast64_t heartbeat,on_until;
static uint64_t work_deadline,guard_max_gap,enable_attempt_us,off_confirm_us;
static atomic_uint_fast64_t first_off_request_us;
static unsigned transaction,held,bus_fault,prepared,retired,active_reg,active_read;
static uint32_t divider,rate,idle_status,saved_mux,saved_drive,saved_pull;
static unsigned bus_retired;
static uint8_t saved_timer_priority;
static unsigned timer_retired;
static uint32_t rd(uintptr_t a){uint32_t v=*(volatile uint32_t *)a;__asm__ volatile("dsb sy":::"memory");return v;}
static void wr(uintptr_t a,uint32_t v){*(volatile uint32_t *)a=v;__asm__ volatile("dsb sy":::"memory");}
/* Fault entry runs at EL3 with translation disabled. No EL1 state is read:
 * dirty Nonsecure cache lines cannot authorize a Secure EL3 operation. The
 * runner owns the fixed cutoff pin before launching this dedicated image. */
__attribute__((section(".text.probe_recovery"),noinline)) void sunstone_probe_abort_devices(void)
{
    volatile uint32_t *dat=(void *)(uintptr_t)(PIO+0x10);
    *dat=*dat&~UINT32_C(0x100);
    __asm__ volatile("dsb sy":::"memory");
    uint32_t cfg=*(volatile uint32_t *)(uintptr_t)(PIO+4),value=*dat;
    __asm__ volatile("dsb sy"::"r"(cfg),"r"(value):"memory");
}
static uint64_t now(void *unused){(void)unused;return counter()/24;}
static int pin_owned(void)
{
    return (rd(PIO+4)&0x00ff00ff)==0x00330001 &&
        (rd(BGR)&0x20002)==0x20002 && (rd(0x07010010)&0x0700001f)==0;
}
static int output(void *unused,unsigned on)
{
    (void)unused;unsigned pin=port?port->vbus_pin:8;
    taskENTER_CRITICAL();
    int ok=(rd(PIO+4)&15)==1;
    if(on&&(atomic_load_explicit(&revoked,memory_order_acquire)||!pin_owned()))ok=0;
    if(ok){uint32_t value=rd(PIO+0x10);
        if(on&&!enable_attempt_us)enable_attempt_us=now(NULL);
        if(!on&&(value&(1u<<pin))&&!atomic_load(&first_off_request_us))
            atomic_store(&first_off_request_us,now(NULL));
        wr(PIO+0x10,on?value|(1u<<pin):value&~(1u<<pin));
        ok=!!(rd(PIO+0x10)&(1u<<pin))==!!on;
        if(!on&&ok&&(value&(1u<<pin)))off_confirm_us=now(NULL);
    }
    taskEXIT_CRITICAL();return ok;
}
static int bus_now(void *unused,uint64_t *n){*n=now(unused);return 1;}
static int current(void *unused){(void)unused;return xTaskGetCurrentTaskHandle()==owner;}
static int authenticate(void *unused){(void)unused;return current(NULL)&&prepared&&!bus_fault&&pin_owned()?1:-1;}
static int enter(void *unused,uint64_t deadline){(void)unused;(void)deadline;if(held)return -1;held=1;return 1;}
static void leave(void *unused){(void)unused;held=0;}
static void retain(void *unused){(void)unused;bus_fault=1;atomic_store(&revoked,1);(void)output(NULL,0);}
static int read_bus(void *unused,uintptr_t a,uint32_t *v)
{
    (void)unused;if(!held||authenticate(NULL)!=1||a<TWI)return 0;
    switch(a-TWI){case 8:case 0xc:case 0x10:case 0x14:case 0x18:case 0x1c:case 0x20:
    case 0x200:case 0x210:case 0x214:case 0x218:case 0x21c:*v=rd(a);return 1;default:return 0;}
}
static int write_bus(void *unused,uintptr_t a,uint32_t v)
{(void)unused;if(!held||authenticate(NULL)!=1||v>255||(a!=TWI+8&&a!=TWI+0xc))return 0;wr(a,v);return 1;}
static int io(void *unused,unsigned reg,unsigned reading,uint8_t *value)
{
    (void)unused;if(bus_fault)return -1;
    if(!transaction){
        if(a733_twi_request(&bus,port->typec_address,(uint16_t)reg,reading,*value,now(NULL)+5000))return -1;
        active_reg=reg;active_read=reading;transaction=1;return 0;
    }
    int r=a733_twi_poll(&bus);if(r!=1)return r;
    uint8_t received=0;
    r=a733_twi_take(&bus,active_read?&received:NULL);
    if(r==1){transaction=0;if(active_reg!=reg||active_read!=reading)return 0;
        if(reading)*value=received;}return r;
}
static int update(uintptr_t a,uint32_t mask,uint32_t v)
{uint32_t desired=(rd(a)&~mask)|(v&mask);wr(a,desired);return rd(a)==desired;}
__attribute__((noinline)) static int prepare(void)
{
    const struct sunstone_board_description *board=sunstone_board_orangepi_zero3w();
    if(board->usb_port_count!=1)return 0;port=&board->usb_ports[0];
    if(port->vbus_pin!=8||port->irq_pin!=9||port->scl_pin!=12||port->sda_pin!=13||
       port->typec_i2c_controller!=A733_TWI_R1||port->i2c_function!=3||
       port->vbus_bank!=A733_PIN_L||port->irq_bank!=A733_PIN_L||
       port->scl_bank!=A733_PIN_L||port->sda_bank!=A733_PIN_L||
       !port->vbus_active_high||port->typec_address!=0x22||port->controller!=2)return 0;
    saved_mux=rd(PIO+4);saved_drive=rd(PIO+0x18);saved_pull=rd(PIO+0x24);
    uint32_t pins[]={8,9,12,13};
    for(unsigned i=0;i<4;++i){unsigned f=(rd(PIO+4)>>(4*(pins[i]-8)))&15;
        if(f!=0&&f!=15&&!(i==0&&f==1&&!(rd(PIO+0x10)&0x100)))return 0;}
    /* Data low precedes mux-to-output; unrelated bank-L fields are retained. */
    wr(PIO+0x10,rd(PIO+0x10)&~0x100u);
    if(!update(PIO+0x18,0x00ff00ff,0)||!update(PIO+0x24,0x0f0f0000,0)||
       !update(PIO+4,0x00ff00ff,0x00330001))return 0;
    uint32_t xo=rd(0x07090160);
    if((rd(0x07010010)&0x0700001f)||(xo&0x10000002)!=0x10000002||(rd(BGR)&0x20002))return 0;
    unsigned hz=a733_clock_dcxo_hz(xo),best=0;
    /* Vendor DTS requests400kHz. Select the fastest representable rate at or
     * below that clock, rather than the common standard-mode100kHz helper. */
    for(unsigned n=0;n<8;++n)for(unsigned m=0;m<16;++m){unsigned d=(1u<<n)*(m+1)*10;
        if((uint64_t)hz<=UINT64_C(400000)*d&&(!best||d<best)){best=d;divider=(m<<3)|n;}}
    if(!best)return 0;rate=hz/best;
    if(!update(BGR,0x20000,0x20000)||!update(BGR,2,2))return 0;
    if((rd(TWI+0xc)&0xfc)||rd(TWI+0x10)!=0xf8||(rd(TWI+0x200)&0x80000003)||
       (rd(TWI+0x210)&3)||(rd(TWI+0x214)&0xffff0000)||(rd(TWI+0x218)&0x01000100)||
       (rd(TWI+0x21c)&0x007f007f)||(rd(TWI+0x18)&1)||(rd(TWI+0x20)&0x35)!=0x30)return 0;
    if(!update(TWI+0x14,255,divider)||!update(TWI+0x1c,3,0)||!update(TWI+0xc,0xfd,0x40))return 0;
    static const struct a733_twi_ops ops={bus_now,authenticate,enter,leave,retain,read_bus,write_bus,current};
    prepared=1;return a733_twi_init_controller(&bus,&ops,&prepared,divider,A733_TWI_R1,1);
}
static void guard(void *unused)
{
    (void)unused;if(!sunstone_deadline_register())sunstone_probe_fail(0xa7,0);
    uint64_t last=now(NULL);
    for(;;){uint64_t t=now(NULL),gap=t-last;last=t;if(gap>guard_max_gap)guard_max_gap=gap;
        if(atomic_load(&stop_guard)){atomic_store(&guard_parked,1);vTaskSuspend(NULL);}
        uint64_t progress=atomic_load(&heartbeat),until=atomic_load(&on_until);
        if(gap>2000||t>=work_deadline||(progress&&t-progress>2000)||(until&&t>=until)){
            atomic_store(&revoked,1);if(!output(NULL,0))atomic_store(&guard_fault,1);
        }
        uint64_t wake=t+500;if(until&&until<wake&&until>t)wake=until;
        if(sunstone_deadline_wait_until(wake)<0)sunstone_probe_fail(0xa8,0);
    }
}
__attribute__((noinline)) static int retire_bus(void)
{
    if(bus_fault||held||transaction||bus.state!=A733_TWI_READY)return 0;
    idle_status=rd(TWI+0x10);
    if((rd(TWI+0xc)&0xfd)!=0x40||idle_status!=0xf8||(rd(TWI+0x20)&0x35)!=0x30||
       (rd(TWI+0x200)&0x80000003)||(rd(TWI+0x210)&3)||
       (rd(TWI+0x214)&0xffff0000)||(rd(TWI+0x218)&0x01000100)||(rd(TWI+0x21c)&0x007f007f))return 0;
    /* STOP completion and high bus lines precede gate removal. No TWI
     * register may be read after this point. Shared S_TWI0 bits remain intact. */
    if(!update(BGR,2,0)||!update(BGR,0x20000,0))return 0;
    prepared=0;
    /* PL8 retains its known-low GPIO authority for the runner's readback.
     * The other three pins and all original drive/pull fields are restored. */
    uint32_t restored=(saved_mux&0x00ff00f0)|1;
    if(!update(PIO+4,0x00ff00ff,restored)||
       !update(PIO+0x18,0x00ff00ff,saved_drive)||
       !update(PIO+0x24,0x0f0f0000,saved_pull))return 0;
    bus_retired=1;return 1;
}
uint64_t sunstone_probe_quiesce(void)
{
    if(!timer_retired||!bus_retired||!retired||!atomic_load(&guard_parked)||
       atomic_load(&guard_fault)||!diagnostic.fell||!output(NULL,0)||bus_fault||held||transaction||
       bus.state!=A733_TWI_READY||prepared)return 0;
    if((rd(BGR)&0x20002)||(rd(PIO+4)&0x00ff00ff)!=((saved_mux&0x00ff00f0)|1)||
       (rd(PIO+0x18)&0x00ff00ff)!=(saved_drive&0x00ff00ff)||
       (rd(PIO+0x24)&0x0f0f0000)!=(saved_pull&0x0f0f0000))return 0;
    return SUNSTONE_PROBE_READY;
}
__attribute__((noinline)) static void retain_off_origin(void)
{
    uint64_t first=atomic_load(&first_off_request_us);
    if(first&&(!diagnostic.off_at||first<diagnostic.off_at))diagnostic.off_at=first;
}
static void client(void *unused)
{
    (void)unused;if(!sunstone_deadline_register())sunstone_probe_fail(0xa7,1);
    if(!prepare())sunstone_probe_fail(0xa0,rd(PIO+4));
    static const struct vbus_ops ops={now,io,output};
    vbus_begin(&diagnostic,&ops,NULL,USB_VBUS_MODE,work_deadline);
    for(;;){uint64_t t=now(NULL);atomic_store(&heartbeat,t);
        /* Fixed upper enable deadline is published before any possible high. */
        if(USB_VBUS_MODE&&diagnostic.state==8&&!atomic_load(&on_until))atomic_store(&on_until,t+(uint64_t)USB_VBUS_MODE*1000000-2000);
        if(atomic_load(&revoked)&&diagnostic.state!=7){
            uint64_t until=atomic_load(&on_until);
            vbus_stop(&diagnostic,diagnostic.enabled&&until&&t>=until?0:0x21);
        }
        retain_off_origin();
        int r=vbus_step(&diagnostic);
        if(r)break;
        if(transaction){
            if(sunstone_deadline_wait_until(now(NULL)+50)<0)sunstone_probe_fail(0xa8,2);
        }else {
            uint64_t wake=now(NULL)+500;
            if(diagnostic.state==5&&diagnostic.until<wake&&diagnostic.until>now(NULL))wake=diagnostic.until;
            if(sunstone_deadline_wait_until(wake)<0)sunstone_probe_fail(0xa8,1);
        }
        sunstone_probe_progress();
    }
    atomic_store(&revoked,1);if(!output(NULL,0))sunstone_probe_fail(0xa1,0);
    if(diagnostic.done!=1||bus_fault||transaction)sunstone_probe_fail(0xa2,diagnostic.failure);
    /* Disable controller interrupt generation, PD transmit and all FUSB IRQ
     * sources before retiring the sole I2C actor. No interrupt was unmasked. */
    const uint8_t regs[]={2,3,9,0xa,0xe,0xf};const uint8_t values[]={0,0xb0,0,255,255,1};
    for(unsigned i=0;i<sizeof(regs);++i){uint8_t value=values[i];int r;
        do{atomic_store(&heartbeat,now(NULL));r=io(NULL,regs[i],0,&value);if(!r&&sunstone_deadline_wait_until(now(NULL)+50)<0)sunstone_probe_fail(0xa8,3);}while(!r);
        if(r<0)sunstone_probe_fail(0xa3,i);}
    atomic_store(&stop_guard,1);uint64_t park_deadline=now(NULL)+2000;
    while(!atomic_load(&guard_parked)){
        if(now(NULL)>=park_deadline)sunstone_probe_fail(0xac,0);
        if(sunstone_deadline_wait_until(now(NULL)+100)<0)sunstone_probe_fail(0xac,1);
    }
    retired=1;
    if(USB_VBUS_MODE&&enable_attempt_us&&(off_confirm_us<enable_attempt_us||
       off_confirm_us-enable_attempt_us>(uint64_t)USB_VBUS_MODE*1000000))diagnostic.failure=0x22;
    if(!retire_bus())sunstone_probe_fail(0xab,0);
    sunstone_deadline_program(0);wr(GICR_SGI+0x180,1u<<27);
    uint64_t irq_deadline=now(NULL)+100000;
    while(rd(GICR_BASE)&8)if(now(NULL)>=irq_deadline)sunstone_probe_fail(0xa9,0);
    wr(GICR_SGI+0x280,1u<<27);
    if((rd(GICR_SGI+0x100)|rd(GICR_SGI+0x200)|rd(GICR_SGI+0x300))&(1u<<27))sunstone_probe_fail(0xa9,1);
    *(volatile uint8_t *)(GICR_SGI+0x400+27)=saved_timer_priority;
    __asm__ volatile("dsb sy":::"memory");
    if(*(volatile uint8_t *)(GICR_SGI+0x400+27)!=saved_timer_priority)sunstone_probe_fail(0xa9,2);
    uint64_t timer_control;__asm__ volatile("mrs %0,cntv_ctl_el0":"=r"(timer_control));
    if(timer_control&1)sunstone_probe_fail(0xa9,3);timer_retired=1;
    *OUT=(struct report){UINT64_C(0x3154505256425353),USB_VBUS_MODE,diagnostic.failure,diagnostic.state,
        diagnostic.rose,diagnostic.fell,diagnostic.rise_us,diagnostic.fall_us,diagnostic.max_gap,
        diagnostic.cc_samples,guard_max_gap,atomic_load(&guard_fault),
        enable_attempt_us?off_confirm_us-enable_attempt_us:0,rd(PIO+4),rd(PIO+0x10),
        bus.state,idle_status,rate,divider,bus_retired};
    sunstone_probe_complete(diagnostic.failure!=0||guard_max_gap>2000,sizeof(*OUT));
}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name){(void)task;(void)name;sunstone_probe_fail(0xa4,0);}
void kernel_main(void)
{
    platform_init();work_deadline=now(NULL)+14000000;
    saved_timer_priority=*(volatile uint8_t *)(GICR_SGI+0x400+27);
    if(rd(GICR_SGI+0x100)&(1u<<27))sunstone_probe_fail(0xaa,0);
    *(volatile uint8_t *)(GICR_SGI+0x400+27)=portLOWEST_USABLE_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT;
    wr(GICR_SGI+0x100,1u<<27);
    uint64_t irq_deadline=now(NULL)+100000;
    while(rd(GICR_BASE)&8)if(now(NULL)>=irq_deadline)sunstone_probe_fail(0xaa,1);
    owner=xTaskCreateStatic(client,"vbus",2048,NULL,2,stacks[0],&tcbs[0]);
    if(!owner||!xTaskCreateStatic(guard,"vbus-cutoff",2048,NULL,3,stacks[1],&tcbs[1]))sunstone_probe_fail(0xa5,0);
    vTaskStartScheduler();sunstone_probe_fail(0xa6,0);
}

extern void vbus_platform_irq(uint64_t);
void vApplicationFPUSafeIRQHandler(uint64_t interrupt)
{if(interrupt==27)sunstone_deadline_irq();else vbus_platform_irq(interrupt);}
