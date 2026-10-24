/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual native provider, TWI, PMIC, pin/rail/clock managers and common locks.
 * The runner models MMIO and FreeRTOS scheduling, never electrical authority. */
#include "a733_typec_native.h"
#include "a733_pmic_native.h"
#include "a733_pins_native.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "task.h"
#include <assert.h>
static struct a733_counter_clock counter;
static struct a733_platform_locks locks;
static struct a733_clock_leases clocks;
static struct a733_rail_leases rails;
static struct a733_gpu_resource_manager gpu;
static struct a733_pins pins;
static struct a733_pins_native pin_native;
static struct a733_pmic pmic;
static struct a733_pmic_native pmic_native;
static struct a733_typec_native port,copied;
static struct a733_typec_native_config config;
static struct a733_typec_token token;
static unsigned scheduler,current_task=1;
static TaskHandle_t queue_owner[A733_PLATFORM_LOCK_COUNT];
uint64_t ullPortInterruptNesting;
volatile uint64_t ullCriticalNesting;
uint64_t typec_test_time=100;
volatile unsigned typec_block_only;
volatile unsigned typec_fault_case,typec_mmio_count,typec_translation_count,typec_checkpoint_id;
volatile unsigned typec_wire_fault,typec_partner_cc=1,typec_external_vbus,typec_delay_stop;
volatile uint64_t typec_monitor_request;
volatile unsigned typec_steady_polls,typec_steady_reads,typec_steady_mmio,typec_steady_old_loop;
BaseType_t xTaskGetSchedulerState(void)
{return scheduler?taskSCHEDULER_RUNNING:taskSCHEDULER_NOT_STARTED;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)(uintptr_t)current_task;}
QueueHandle_t xQueueCreateMutexStatic(const uint8_t type,StaticQueue_t *buffer)
{(void)type;return (QueueHandle_t)buffer;}
static unsigned queue_index(QueueHandle_t q)
{for(unsigned i=0;i<A733_PLATFORM_LOCK_COUNT;i++)if(q==locks.mutex[i])return i;assert(0);return 0;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q){return queue_owner[queue_index(q)];}
BaseType_t xQueueSemaphoreTake(QueueHandle_t q,TickType_t wait)
{
    unsigned i=queue_index(q);
    if(queue_owner[i]){typec_test_time+=wait?1000:0;return pdFALSE;}
    queue_owner[i]=xTaskGetCurrentTaskHandle();return pdTRUE;
}
BaseType_t xQueueGenericSend(QueueHandle_t q,const void *item,TickType_t wait,const BaseType_t position)
{
    (void)item;(void)wait;(void)position;unsigned i=queue_index(q);
    assert(queue_owner[i]==xTaskGetCurrentTaskHandle());queue_owner[i]=0;return pdTRUE;
}
static int forbidden_read(void *p,uintptr_t a,uint32_t *out)
{(void)p;(void)a;(void)out;assert(0);return 0;}
static int forbidden_update(void *p,uintptr_t a,uint32_t r,uint32_t s,uint64_t d)
{(void)p;(void)a;(void)r;(void)s;(void)d;assert(0);return 0;}
static int forbidden_rail(void *p,struct a733_gpu_resource_rail *out,uint64_t d)
{(void)p;(void)out;(void)d;assert(0);return 0;}
__attribute__((noinline)) void typec_checkpoint(unsigned id)
{typec_checkpoint_id=id;}
static void setup(void)
{
    assert(a733_counter_clock_init(&counter,24000000));
    assert(a733_platform_locks_init(&locks,a733_counter_clock_now,&counter));
    assert(a733_clock_leases_init(&clocks,a733_counter_clock_now,&counter));
    const struct a733_gpu_resource_ops gpu_ops={a733_counter_clock_now,forbidden_read,forbidden_update,forbidden_rail};
    assert(a733_gpu_resources_init(&gpu,&gpu_ops,&counter));
    const struct a733_rail_binding rail_binding={&clocks,&gpu,900,a733_counter_clock_now,&counter};
    assert(a733_rail_leases_init(&rails,&rail_binding));
}
static int pin_now(void *p,uint64_t *out)
{struct a733_platform_locks *l=p;return l->now(l->context,out);}
static void finish_setup(void)
{
    const struct a733_pins_locks pin_locks={locks.callbacks.register_enter,locks.callbacks.register_leave,pin_now,&locks};
    assert(a733_pins_native_init(&pin_native,&pin_locks));
    struct a733_pin_handoff handoff={0};
    for(unsigned b=1;b<A733_PIN_BANKS;b++)handoff.authorized[b]=a733_pin_bank_mask(b);
    for(unsigned b=0;b<2;b++)handoff.latch[b]=(struct a733_pin_latch_handoff){100+b,900,a733_pin_bank_mask(A733_PIN_L+b),0};
    assert(a733_pins_init_banks(&pins,&pin_native.operations,&pin_native,&handoff));
    assert(a733_pmic_native_init(&pmic_native,&locks,(void *)2));
    const struct a733_pmic_config pmic_config={777,888,1000000,{{2,1,1},{2,1,1}}};
    assert(a733_pmic_native_bind(&pmic,&pmic_config,&pmic_native,&clocks,&rails,&pins));
    config=(struct a733_typec_native_config){.clocks=&clocks,.pins=&pins,.rails=&rails,.pmic=&pmic,.locks=&locks,
        .provider_task=(void *)1,.power_task=(void *)3,
        .electrical={.owner=42,.generation=51,.qualification=52,.recovery=53,.source_min_mv=4750,
        .source_max_mv=5250,.source_ma=500,.zero_threshold_max_mv=600,.device_id=0x80}};
    assert(a733_typec_native_init(&port,&config));
    assert(!a733_typec_native_init(&copied,&config));
    scheduler=1;assert(a733_typec_begin(&port.provider,1000000)==0);
}
static int service(unsigned run_provider)
{
    typec_test_time+=25;
    current_task=2;
    if(!port.provider.prepared)assert(a733_pmic_poll(&pmic)!=A733_PMIC_RETAINED);
    current_task=3;int power=a733_typec_native_power_poll(&port);
    current_task=1;int result=run_provider?a733_typec_poll(&port.provider):0;
    if(!atomic_load(&port.provider.revoked))assert(power>=0);
    return result;
}
static void retained(void)
{
    assert(port.clock.id&&port.rail.id&&port.pin.id);
    assert(port.requests[0].id&&port.requests[1].id);
    assert(atomic_load(&port.output_state)==1);
    assert(a733_clock_token_status(&clocks,&port.clock)==A733_CLOCK_OK);
    assert(a733_rail_release(&rails,&port.rail,typec_test_time+1000)!=A733_RAIL_READY);
    struct a733_typec_token untouched={.owner=123};
    assert(a733_typec_snapshot(&port.provider,&untouched,typec_test_time+1000)==-1&&untouched.owner==123);
}
unsigned typec_native_check(void)
{
    setup();finish_setup();unsigned look_saved=0,safe_saved=0;
    for(unsigned i=0;i<40000&&!token.provider;i++){
        if(port.stage==7){
            typec_checkpoint(4);
            if(typec_fault_case==7){
                unsigned accesses=typec_mmio_count;pmic.failed=1;
                assert(a733_typec_poll(&port.provider)==-1&&port.setup_fault&&!port.pin.id);
                assert(typec_mmio_count==accesses&&port.clock.id&&port.rail.id);return 0;
            }
        }
        if(port.provider.state==A733_TYPEC_LOOK&&!look_saved){
            look_saved=1;typec_checkpoint(1);
            if(typec_fault_case==1)typec_partner_cc=2;
        }
        if(port.provider.state==A733_TYPEC_SAFE0&&!safe_saved){
            safe_saved=1;typec_checkpoint(2);
            if(typec_fault_case==2)typec_external_vbus=1;
        }
        if(port.provider.state==A733_TYPEC_MONITOR&&port.provider.step==0&&!port.provider.transaction){
            typec_monitor_request=typec_test_time+25;typec_delay_stop=1;
        }
        int r=service(1);
        if(typec_fault_case==2&&atomic_load(&port.provider.revoked)){
            assert(atomic_load(&port.output_state)!=2);
            for(unsigned k=0;k<1000&&port.provider.state!=A733_TYPEC_OFF_RETAINED&&port.provider.state!=A733_TYPEC_UNCERTAIN;k++)service(1);
            assert(atomic_load(&port.output_state)==1);return 0;
        }
        assert(r>=0);
        if(r==1){assert(a733_typec_snapshot(&port.provider,&token,typec_test_time+1000)==1);}
    }
    assert(token.provider==(uintptr_t)&port.provider&&token.polarity==(typec_fault_case==1?2u:1u));
    assert(token.observed_us==typec_monitor_request&&typec_test_time-token.observed_us>=250);
    assert(a733_typec_validate(&port.provider,&token,typec_test_time+1000)==1);
    assert(typec_translation_count>=8&&port.supplies[0].programmed_uv==3300000&&port.supplies[1].programmed_uv==3300000);
    typec_checkpoint(3);
    if(typec_block_only){
        uint8_t bytes[3]={0};
        assert(a733_twi_request_block(&port.bus,0x22,1,1,NULL,3,typec_test_time+5000)==0);
        int result=0;
        for(unsigned i=0;i<200&&!result;i++){
            typec_test_time+=25;current_task=3;
            assert(a733_typec_native_power_poll(&port)>=0);current_task=1;
            result=a733_twi_poll(&port.bus);
        }
        assert(result==1&&a733_twi_take_block(&port.bus,bytes,3)==1);
        assert(bytes[0]==0x80&&bytes[1]==0x80&&bytes[2]==0x80);
        assert(!port.bus.held&&!queue_owner[A733_PLATFORM_I2C1_LOCK]&&!port.bus_fault);
        return 0;
    }
    if(typec_fault_case==3){
        typec_wire_fault=1;
        for(unsigned k=0;k<1000&&!port.bus_fault;k++)service(1);
        assert(port.bus_fault&&atomic_load(&locks.retained)==(1u<<A733_PLATFORM_I2C1_LOCK)&&queue_owner[A733_PLATFORM_I2C1_LOCK]==(void *)1);
        for(unsigned k=0;k<10&&atomic_load(&port.output_state)!=1;k++)service(1);
        retained();return 0;
    }
    if(typec_fault_case==4){
        for(unsigned k=0;k<500&&!atomic_load(&port.provider.revoked);k++)service(0);
        assert(atomic_load(&port.provider.revoked));retained();return 0;
    }
    if(typec_fault_case==5){
        ++token.generation;assert(a733_typec_validate(&port.provider,&token,typec_test_time+1000)==-1);
        service(0);retained();return 0;
    }
    if(typec_fault_case==8){
        /* Board cadence for 100 ms of attached monitoring: yield while an I2C
         * transfer is in flight, otherwise sleep one 1 ms tick. Every
         * published token stays fresh and the service gap holds. The old
         * loop also yielded after each changed monitoring step. */
        uint64_t end=typec_test_time+100000;unsigned mmio=typec_mmio_count,progress=0;
        while(typec_test_time<end){
            if((port.bus.state<A733_TWI_BEGIN||port.bus.state>A733_TWI_COMPLETE)&&!progress)typec_test_time+=975;
            unsigned idle=!port.provider.transaction,step=port.provider.step;
            assert(service(1)>=0&&!atomic_load(&port.provider.revoked));++typec_steady_polls;
            progress=typec_steady_old_loop&&!port.provider.pd_turn&&port.provider.step&&port.provider.step!=step;
            if(idle&&port.provider.transaction&&port.bus.reg==0x40)++typec_steady_reads;
            struct a733_typec_token t;
            assert(a733_typec_snapshot(&port.provider,&t,typec_test_time+1000)==1);
            assert(typec_test_time-t.observed_us<A733_TYPEC_FRESH_US);
        }
        typec_steady_mmio=typec_mmio_count-mmio;
        assert(port.provider.state==A733_TYPEC_MONITOR&&typec_steady_reads);return 0;
    }
    if(typec_fault_case==6){
        atomic_store(&clocks.busy,1);current_task=3;
        assert(a733_typec_native_power_poll(&port)==-1&&!port.power_fault);
        assert(atomic_load(&port.provider.revoked)&&atomic_load(&port.output_state)==2);
        atomic_store(&clocks.busy,0);typec_test_time+=25;
        assert(a733_typec_native_power_poll(&port)==-1&&!port.power_fault);
        retained();return 0;
    }
    /* Nominal detach: the separate GPIO task revokes before the I2C task runs. */
    typec_partner_cc=0;
    current_task=3;assert(a733_typec_native_power_poll(&port)==-1);
    assert(atomic_load(&port.output_state)==1&&atomic_load(&port.provider.revoked));
    current_task=1;
    for(unsigned k=0;k<2000&&port.provider.state!=A733_TYPEC_OFF_RETAINED;k++)service(1);
    assert(port.provider.state==A733_TYPEC_OFF_RETAINED&&!queue_owner[0]&&!queue_owner[1]&&!queue_owner[2]);
    retained();return 0;
}
