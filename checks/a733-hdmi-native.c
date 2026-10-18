/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual native HDMI, controller, registry, rail, PMIC, pin, PCK manager,
 * counter, translation and common-lock code. Scheduler/queue behavior, analog
 * admission and hardware responses are modeled. No physical authority claimed. */
#include "a733_hdmi_native.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "task.h"
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
static struct pmic_model model;
static struct a733_counter_clock counter;
static struct a733_platform_locks locks;
static struct a733_hdmi_native hdmi,copied;
static struct a733_hdmi_native_config config;
static unsigned scheduler;
static TaskHandle_t queue_owner[2];
uint64_t ullPortInterruptNesting;
volatile uint64_t ullCriticalNesting;
uint64_t hdmi_test_time=100;
volatile unsigned hdmi_fault_case,hdmi_mmio_count,hdmi_translation_count;
static uint32_t *reg(uintptr_t a){return (uint32_t *)a;}
static unsigned queue_index(QueueHandle_t q)
{if(q==locks.mutex[0])return 0;assert(q==locks.mutex[1]);return 1;}
BaseType_t xTaskGetSchedulerState(void)
{return scheduler?taskSCHEDULER_RUNNING:taskSCHEDULER_NOT_STARTED;}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{return (TaskHandle_t)(uintptr_t)(model.worker?2:1);}
QueueHandle_t xQueueCreateMutexStatic(const uint8_t type,StaticQueue_t *buffer)
{(void)type;return (QueueHandle_t)buffer;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q){return queue_owner[queue_index(q)];}
BaseType_t xQueueSemaphoreTake(QueueHandle_t q,TickType_t wait)
{
    (void)wait;unsigned i=queue_index(q);assert(!queue_owner[i]);
    queue_owner[i]=xTaskGetCurrentTaskHandle();
    if(hdmi_fault_case==1)atomic_store_explicit(&model.clocks.failed,1,memory_order_release);
    return pdTRUE;
}
BaseType_t xQueueGenericSend(QueueHandle_t q,const void *item,TickType_t wait,const BaseType_t position)
{
    (void)item;(void)wait;(void)position;unsigned i=queue_index(q);
    assert(queue_owner[i]==xTaskGetCurrentTaskHandle());queue_owner[i]=0;return pdTRUE;
}
static int shared_now(void *p,uint64_t *out)
{(void)p;return a733_counter_clock_now(&counter,out);}
static int pck_read(void *p,uintptr_t a,uint32_t *out)
{(void)p;assert(hdmi_translation_count>=34);assert(a==0x07090160||a==0x070101ac);*out=*reg(a);return 1;}
static int pck_update(void *p,uintptr_t a,uint32_t required,uint32_t set,uint64_t deadline)
{
    (void)p;assert(a==0x070101ac);
    assert(a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,deadline));
    assert((*reg(a)&required)==required);*reg(a)|=set;
    a733_platform_lock_leave(&locks,A733_PLATFORM_REGISTER_LOCK);return 1;
}
__attribute__((noinline)) void hdmi_checkpoint(unsigned id)
{(void)id;__asm__ volatile("":::"memory");}
static void setup(void)
{
    assert(a733_counter_clock_init(&counter,24000000));
    assert(a733_platform_locks_init(&locks,a733_counter_clock_now,&counter));
    model.now=hdmi_test_time;
    *pmic_model_word(&model,0x07090160)=0x10000002;
    *pmic_model_word(&model,0x0701019c)=0x10001;
    *pmic_model_word(&model,0x07025000)=0x22;
    *pmic_model_word(&model,0x07025014)=0x11;
    *pmic_model_word(&model,0x07025024)=5;
    *pmic_model_word(&model,0x0708300c)=0x40;
    *pmic_model_word(&model,0x07083010)=0xf8;
    *pmic_model_word(&model,0x07083014)=0x28;
    *pmic_model_word(&model,0x07083020)=0x30;
    *pmic_model_word(&model,0x07083200)=0x00f81000;
    *pmic_model_word(&model,0x07083210)=0x80c0;
    *pmic_model_word(&model,0x07083218)=0x00100010;
    model.pmic[0x0e]=3;model.pmic[0x10]=2;model.pmic[0x13]=30;
    model.pmic[0x21]=16;model.pmic[0x30]=13;
    assert(a733_clock_leases_init(&model.clocks,a733_counter_clock_now,&counter));
    const struct a733_gpu_resource_ops gpu={shared_now,pck_read,pck_update,pmic_model_gpu_rail};
    assert(a733_gpu_resources_init(&model.gpu,&gpu,&model));
    const struct a733_rail_binding binding={&model.clocks,&model.gpu,900,a733_counter_clock_now,&counter};
    assert(a733_rail_leases_init(&model.rails,&binding));
    const struct a733_pins_operations pins={pmic_model_pin_enter,pmic_model_pin_leave,
        shared_now,pmic_model_pin_read,pmic_model_pin_write};
    struct a733_pin_handoff handoff={0};
    for(unsigned b=1;b<A733_PIN_BANKS;++b)handoff.authorized[b]=a733_pin_bank_mask(b);
    for(unsigned i=0;i<2;++i)handoff.latch[i]=(struct a733_pin_latch_handoff){100+i,900,a733_pin_bank_mask(A733_PIN_L+i),0};
    assert(a733_pins_init_banks(&model.pins,&pins,&model,&handoff));
    const struct a733_pmic_config power={777,888,1000000,{{2,1,1},{2,1,1}}};
    const struct a733_pmic_ops ops={shared_now,pmic_model_current,pmic_model_enter,
        pmic_model_leave,pmic_model_retain,pmic_model_read,pmic_model_write,pmic_model_outside};
    assert(a733_pmic_init(&model.provider,&power,&ops,&model,&model.clocks,&model.rails,&model.pins));
    config=(struct a733_hdmi_native_config){.clocks=&model.clocks,.rails=&model.rails,
        .pmic=&model.provider,.locks=&locks,.pck=&model.gpu,.pck_client=0,.task=(void *)1,
        .owner=42,.epoch=200,.recovery=201,.request={.mode=A733_HDMI_720P60,
        .range=A733_HDMI_LIMITED,.initial={.id=7,.generation=8,
        .words={8,0xa0,0x00110001,0x10000000,0x76543210,0,0,0x001f0040},
        .resistor_control=0xa5a50000,.resistor_manual=0xabcdef12}}};
    struct a733_hdmi_native *alias=(void *)&model.trace[3000];
    _Static_assert(sizeof(*alias)<=sizeof(model.trace)-3000*sizeof(model.trace[0]),"alias fits backing");
    assert(!alias->self&&!a733_hdmi_native_init(alias,&config)&&!alias->self);
    assert(a733_hdmi_native_init(&hdmi,&config));
    assert(!a733_hdmi_native_init(&copied,&config));
    struct a733_clock_token unchanged={.id=123};
    assert(a733_clock_acquire(&model.clocks,55,A733_CLOCK_VO1,0,1000000,&unchanged)==A733_CLOCK_INVALID&&unchanged.id==123);
    struct a733_pmic_snapshot s;
    assert(a733_pmic_decode(A733_RAIL_CLDO2,16,0,&s)&&s.decoded&&s.programmed_uv==500000);
    assert(a733_pmic_decode(A733_RAIL_CLDO2,16,29,&s)&&s.decoded&&s.programmed_uv==3400000);
    assert(a733_pmic_decode(A733_RAIL_CLDO2,16,30,&s)&&!s.decoded);
    assert(a733_pmic_decode(A733_RAIL_CLDO2,16,31,&s)&&!s.decoded);
    scheduler=1;
}
static void provider_step(void)
{
    model.now=hdmi_test_time+=100;
    assert(pmic_model_step(&model)!=A733_PMIC_RETAINED);
}
unsigned hdmi_native_check(void)
{
    setup();assert(a733_hdmi_prepare(&hdmi.controller,1000000)==1);
    for(unsigned i=0;i<10000&&hdmi.controller.state!=A733_HDMI_PREPARED;++i){
        provider_step();
        /* Native stage checkpoints only select preserved execution positions;
         * expected register sequences are independently checked by the runner. */
        if(hdmi.stage==8)hdmi_checkpoint(1);
        if(hdmi.stage==13)hdmi_checkpoint(2);
        unsigned accesses=hdmi_mmio_count;
        if(hdmi_fault_case==2)hdmi_test_time=hdmi.power_deadline;
        if(hdmi_fault_case==3)++hdmi.config.epoch;
        int r=a733_hdmi_poll(&hdmi.controller);
        if(hdmi_fault_case){
            assert(r==-1&&hdmi.controller.state==A733_HDMI_FAULT_RETAINED&&hdmi.failed);
            assert(hdmi.clock.id&&hdmi.rail.id&&hdmi.pck_lease.token&&hdmi.requests[0].id&&hdmi.requests[1].id);
            assert(hdmi_mmio_count==accesses);
            if(hdmi_fault_case==1)assert(atomic_load(&locks.retained)&1);
            assert(a733_hdmi_poll(&hdmi.controller)==-1&&hdmi_mmio_count==accesses);
            return 0;
        }
        assert(r>=0);
    }
    assert(hdmi.controller.state==A733_HDMI_PREPARED);
    assert(hdmi.supplies[0].programmed_uv==800000&&hdmi.supplies[1].programmed_uv==1800000);
    assert(hdmi.requests[0].id&&hdmi.requests[1].id&&hdmi.pck_lease.token);
    assert(a733_rail_release(&model.rails,&hdmi.rail,1000000)!=A733_RAIL_READY);
    struct a733_clock_token other={0};
    assert(a733_clock_acquire(&model.clocks,43,0,A733_CLOCK_VO1,1000000,&other)==A733_CLOCK_BUSY);
    assert(a733_hdmi_start(&hdmi.controller,2000000)==1);
    for(unsigned i=0;i<1000&&hdmi.controller.state!=A733_HDMI_ACTIVE;++i){provider_step();assert(a733_hdmi_poll(&hdmi.controller)>=0);}
    assert(hdmi.controller.state==A733_HDMI_ACTIVE);
    assert(a733_hdmi_stop(&hdmi.controller,3000000)==1);
    for(unsigned i=0;i<100&&hdmi.controller.state!=A733_HDMI_STOPPED_RETAINED;++i){provider_step();assert(a733_hdmi_poll(&hdmi.controller)>=0);}
    assert(hdmi.controller.state==A733_HDMI_STOPPED_RETAINED);
    assert(hdmi.rail.id&&hdmi.clock.id&&a733_clock_token_status(&model.clocks,&hdmi.clock)==A733_CLOCK_OK);
    assert(!queue_owner[0]&&!queue_owner[1]);return 0;
}
