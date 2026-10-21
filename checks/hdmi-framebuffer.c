/* Actual application/DE/TV0/IOMMU/media/native-provider composition.
 * Queue scheduling, PMIC bus responses, analog state and initial physical
 * authority are modeled. HDMI PREPARED is an explicit checkpoint supported by
 * prior native HDMI evidence; this case does not replay PHY/DDC preparation. */
#include "../apps/hdmi/application.h"
#include "a733_memory_uncached.h"
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
struct hdmi_application app;
static struct pmic_model model;
static unsigned scheduler;
static TaskHandle_t holders[A733_PLATFORM_LOCK_COUNT];
uint64_t ullPortInterruptNestings[8];
volatile uint64_t ullCriticalNestings[8];
volatile uint64_t framebuffer_test_time=100;
volatile unsigned framebuffer_fault_case,framebuffer_test_result;
int hdmi_live(void *v)
{(void)v;return !atomic_load(&app.fatal)&&framebuffer_test_time<app.deadline;}
void hdmi_fail(void *v){(void)v;atomic_store(&app.fatal,1);}
BaseType_t xTaskGetSchedulerState(void)
{return scheduler?taskSCHEDULER_RUNNING:taskSCHEDULER_NOT_STARTED;}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{return (TaskHandle_t)(uintptr_t)(model.worker?2:1);}
QueueHandle_t xQueueCreateMutexStatic(const uint8_t type,StaticQueue_t *p)
{(void)type;return (QueueHandle_t)p;}
static unsigned queue(QueueHandle_t q)
{
    for(unsigned i=0;i<A733_PLATFORM_LOCK_COUNT;++i)
        if(q==app.locks.mutex[i])return i;
    assert(0);return 0;
}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q){return holders[queue(q)];}
BaseType_t xQueueSemaphoreTake(QueueHandle_t q,TickType_t wait)
{
    (void)wait;unsigned i=queue(q);assert(!holders[i]);holders[i]=xTaskGetCurrentTaskHandle();
    if(framebuffer_fault_case==2)atomic_store(&app.clocks.failed,1);
    return pdTRUE;
}
BaseType_t xQueueGenericSend(QueueHandle_t q,const void *item,TickType_t wait,BaseType_t position)
{(void)item;(void)wait;(void)position;unsigned i=queue(q);assert(holders[i]==xTaskGetCurrentTaskHandle());holders[i]=0;return pdTRUE;}
static int shared_now(void *v,uint64_t *out)
{(void)v;return a733_counter_clock_now(&app.counter,out);}
static int pck_read(void *v,uintptr_t a,uint32_t *out)
{(void)v;assert(a==0x07090160||a==0x070101ac);*out=*(volatile uint32_t *)a;return 1;}
static int pck_update(void *v,uintptr_t a,uint32_t required,uint32_t set,uint64_t d)
{(void)v;assert(a==0x070101ac&&a733_platform_lock_enter(&app.locks,A733_PLATFORM_REGISTER_LOCK,d)==1);
 assert((*(volatile uint32_t *)a&required)==required);*(volatile uint32_t *)a|=set;
 a733_platform_lock_leave(&app.locks,A733_PLATFORM_REGISTER_LOCK);return 1;}
__attribute__((noinline)) void framebuffer_checkpoint(unsigned which)
{(void)which;__asm__ volatile("":::"memory");}
static void setup(void)
{
    app.deadline=20000000;app.stop_at=18000000;app.handles[1]=(TaskHandle_t)1;
    app.session.owner=11;app.session.epoch=22;app.session.supervisor=33;app.session.pck_client=3;
    app.session.request=(struct a733_hdmi_request){.mode=A733_HDMI_720P60,.range=A733_HDMI_LIMITED,.phy_reference_hz=24000000,
        .source=A733_HDMI_FRAMEBUFFER,.initial={.id=44,.generation=55,
            .words={8,0xa0,0x00110001,0x10000000,0x76543210,0,0,0x001f0040},
            .resistor_control=0xa5a50000,.resistor_manual=0xabcdef12}};
    app.session.framebuffer=(struct hdmi_framebuffer_session){.isolation={1},.memory={2},.authority=66,
        .fabric={0,0x80000000,0x80000000},.fabric_hz={24000000,24000000,24000000},
        .iommu_gates={0x10007,0x10007}};
    assert(a733_counter_clock_init(&app.counter,24000000));
    assert(a733_platform_locks_init(&app.locks,a733_counter_clock_now,&app.counter));
    assert(a733_clock_leases_init(&app.clocks,a733_counter_clock_now,&app.counter));
    const struct a733_gpu_resource_ops gpu={shared_now,pck_read,pck_update,pmic_model_gpu_rail};
    assert(a733_gpu_resources_init(&app.pck,&gpu,&model));
    const struct a733_rail_binding rb={&app.clocks,&app.pck,900,a733_counter_clock_now,&app.counter};
    assert(a733_rail_leases_init(&app.rails,&rb));
    model.now=100;
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
    const struct a733_pins_operations pins={pmic_model_pin_enter,pmic_model_pin_leave,
        shared_now,pmic_model_pin_read,pmic_model_pin_write};
    struct a733_pin_handoff handoff={0};
    for(unsigned b=1;b<A733_PIN_BANKS;++b)handoff.authorized[b]=a733_pin_bank_mask(b);
    for(unsigned i=0;i<2;++i)handoff.latch[i]=(struct a733_pin_latch_handoff){100+i,900,a733_pin_bank_mask(A733_PIN_L+i),0};
    assert(a733_pins_init_banks(&app.pins,&pins,&model,&handoff));
    const struct a733_pmic_config power={777,888,20000000,{{2,1,1},{2,1,1}}};
    const struct a733_pmic_ops ops={shared_now,pmic_model_current,pmic_model_enter,
        pmic_model_leave,pmic_model_retain,pmic_model_read,pmic_model_write,pmic_model_outside};
    assert(a733_pmic_init(&app.pmic,&power,&ops,&model,&app.clocks,&app.rails,&app.pins));
    struct a733_hdmi_native_config config={.clocks=&app.clocks,.rails=&app.rails,.pmic=&app.pmic,
        .locks=&app.locks,.pck=&app.pck,.pck_client=3,.task=(void *)1,.owner=11,.epoch=22,
        .request=app.session.request,.recovery_mode=A733_HDMI_SUPERVISED_FRAMEBUFFER,.supervision=33,
        .pixel_source=a733_display_framebuffer_source,.pixel_context=&framebuffer};
    assert(a733_hdmi_native_init(&app.hdmi,&config));
    assert(hdmi_framebuffer_boot());scheduler=1;
    assert(a733_hdmi_prepare(&app.hdmi.controller,app.deadline)==1);
    unsigned polls=0;
    while(app.hdmi.controller.state==A733_HDMI_ACQUIRE){
        assert(++polls<2000);model.worker=1;enum a733_pmic_status pr=a733_pmic_poll(&app.pmic);model.worker=0;
        assert(pr==A733_PMIC_PENDING||pr==A733_PMIC_READY||pr==A733_PMIC_COMPLETE);
        assert(a733_hdmi_poll(&app.hdmi.controller)>=0);framebuffer_test_time+=100;model.now=framebuffer_test_time;
    }
    assert(app.hdmi.controller.state==A733_HDMI_PREFLIGHT);
    /* Reused prior native-controller evidence supplies this explicit prepared
     * PHY/controller checkpoint. New TV0 DE-source programming runs below. */
    app.hdmi.controller.state=A733_HDMI_PREPARED;
    while(!app.hdmi.controller.tv.prepared)assert(a733_tcon_tv_prepare(&app.hdmi.controller.tv)>=0);
    app.stage=1;atomic_store(&app.pmic_ready,1);
}
static void retained(void)
{
    struct device_memory_extent e;enum device_memory_state state;
    assert(device_memory_describe(framebuffer.pool,framebuffer.lease,&e,&state));
    assert(state==DEVICE_MEMORY_DEVICE||state==DEVICE_MEMORY_RETAINED);
    assert(e.cpu==0x41c00000&&e.bytes==2097152);
    assert(!device_memory_release(framebuffer.pool,framebuffer.lease));
}
int framebuffer_check_run(void)
{
    setup();assert(a733_display_framebuffer_source(&framebuffer,1)==-1);
    unsigned checkpoint=0,start_checkpoint=0;
    for(unsigned i=0;i<4000&&atomic_load(&app.state)!=HDMI_APP_ACTIVE;++i){
        if(framebuffer.stage==2&&checkpoint<1){checkpoint=1;framebuffer_checkpoint(1);
            if(framebuffer_fault_case==1)++app.session.framebuffer.authority;}
        if(framebuffer.stage==9&&!start_checkpoint){start_checkpoint=1;framebuffer_checkpoint(4);
            if(framebuffer_fault_case==4)atomic_store(&app.cancel,1);}
        if(framebuffer.stage==10&&checkpoint<2){checkpoint=2;framebuffer_checkpoint(2);}
        int r=hdmi_output_step();
        if(framebuffer_fault_case&&framebuffer_fault_case!=4){assert(r==-1);retained();goto passed;}
        if(framebuffer_fault_case==4&&r<0){
            assert(atomic_load(&app.state)==HDMI_APP_FAULT_RETAINED&&app.hdmi.controller.io.fault&&
                app.hdmi.controller.state==A733_HDMI_STOPPED_RETAINED);
            assert(!( *(volatile uint32_t *)0x05730090&0x80000000));
            assert(framebuffer.failed&&framebuffer.de.state==A733_DE_PENDING);
            retained();goto passed;
        }
        assert(r>=0);framebuffer_test_time+=100;model.now=framebuffer_test_time;
    }
    assert(atomic_load(&app.state)==HDMI_APP_ACTIVE&&framebuffer.de.state==A733_DE_ARMED);
    assert(*(volatile uint32_t *)0x05008010==0x40);
    assert(*(volatile uint32_t *)0x05221000==0xff000a03);
    assert(*(volatile uint32_t *)0x0522100c==2560);
    assert(*(volatile uint32_t *)0x05221010==0x10001000);
    assert((*(volatile uint32_t *)0x05730040&7)==0);
    assert((*(volatile uint32_t *)0x05730090&0x80000000)!=0);
    uint64_t pa;assert(a733_iommu_translate(&framebuffer.tables,0x10001000,A733_IOMMU_READ,&pa)==1&&pa==0x41c09000);
    assert(a733_iommu_translate(&framebuffer.tables,0x10001000,A733_IOMMU_WRITE,&pa)!=1);
    retained();assert(hdmi_output_step()>=0); /* One actual fault scan begins before stop. */
    framebuffer_checkpoint(3);
    atomic_store(&app.cancel,1);
    for(unsigned i=0;i<400&&atomic_load(&app.state)!=HDMI_APP_STOPPED_RETAINED;++i){
        int r=hdmi_output_step();
        if(framebuffer_fault_case==3&&r<0){retained();goto passed;}
        assert(r>=0);framebuffer_test_time+=100;model.now=framebuffer_test_time;
    }
    assert(!framebuffer_fault_case);
    assert(atomic_load(&app.state)==HDMI_APP_STOPPED_RETAINED&&framebuffer.de.state==A733_DE_DISABLED_RETAINED);
    assert(!(*(volatile uint32_t *)0x05730090&0x80000000)&&!*(volatile uint32_t *)0x05008140);
    retained();
passed:
    framebuffer_test_result=1;return 0;
}
