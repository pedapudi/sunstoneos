/* SPDX-License-Identifier: Apache-2.0 */
/* Native instruction composition. Scheduler ownership and hardware responses
 * are modeled; provider/ledger/pins/consumer/native-adapter/startup code is real.
 * The runner supplies only counter/EL system-register values and mapped MMIO.
 */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "a733_npu_startup.h"
#include "a733_gpu_platform.h"
static struct pmic_model model;
static struct a733_counter_clock source;
static struct a733_platform_locks locks;
static struct a733_npu_startup_resources shared;
static struct a733_npu_startup npu;
static struct a733_gpu_platform gpu;
static unsigned scheduler, held, late_gpu, late_npu, token;
uint64_t consumer_test_time;
unsigned consumer_test_case;
static uint32_t *word(uintptr_t a){return (uint32_t *)a;}
static void advance(void){model.now+=100;consumer_test_time=model.now;}
BaseType_t xTaskGetSchedulerState(void){return scheduler ? taskSCHEDULER_RUNNING : taskSCHEDULER_NOT_STARTED;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)(uintptr_t)(model.worker ? 2 : 1);}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q)
{
    if(q==locks.mutex[0])return held ? (TaskHandle_t)1 : 0;
    if(q==locks.mutex[1])return model.i2c_held ? (TaskHandle_t)2 : 0;
    assert(q==locks.mutex[A733_PLATFORM_I2C1_LOCK]);return 0;
}
int a733_platform_lock_enter(struct a733_platform_locks *l,enum a733_platform_lock k,uint64_t d)
{
    assert(l==&locks && !model.worker && k==A733_PLATFORM_REGISTER_LOCK);
    if(held || model.now>=d || atomic_load(&locks.retained))return 0;
    held=1;model.register_held=1;return 1;
}
void a733_platform_lock_leave(struct a733_platform_locks *l,enum a733_platform_lock k)
{
    assert(l==&locks && k==A733_PLATFORM_REGISTER_LOCK && held);
    held=0;model.register_held=0;
    if(late_npu && a733_npu_platform_ready(&npu.platform))model.provider.failed=1;
}
int a733_platform_lock_retain(struct a733_platform_locks *l,enum a733_platform_lock k)
{assert(l==&locks && k==A733_PLATFORM_REGISTER_LOCK && held);atomic_store(&l->retained,1);return 1;}
static int reg_enter(void *v,uint64_t d){return a733_platform_lock_enter(v,A733_PLATFORM_REGISTER_LOCK,d);}
static void reg_leave(void *v){a733_platform_lock_leave(v,A733_PLATFORM_REGISTER_LOCK);}
static int bus_enter(void *v,uint64_t d){(void)v;(void)d;assert(0);return 0;}
static void bus_leave(void *v){(void)v;assert(0);}
enum i2c_result a733_i2c_open(struct a733_i2c *b){(void)b;assert(0);return I2C_STATUS;}
enum i2c_result a733_i2c_read8(struct a733_i2c *b,uint8_t a,uint8_t r,uint8_t *o)
{(void)b;(void)a;(void)r;(void)o;assert(0);return I2C_STATUS;}
void a733_i2c_close(struct a733_i2c *b){(void)b;assert(0);}
static int clock_now(void *v,uint64_t *out)
{(void)v;return a733_counter_clock_now(&source,out);}
static int gpu_now(void *v,uint64_t *out)
{
    (void)v;
    if(late_gpu && gpu.power.phase==A733_GPU_POWER_ON && gpu.phase==A733_GPU_PLATFORM_STARTING)
        model.provider.failed=1;
    return clock_now(0,out);
}
static uintptr_t address(enum a733_gpu_power_bank b,uint32_t off)
{return (b==A733_GPU_CCU ? 0x02002000u : b==A733_GPU_R_CCU ? 0x07010000u : 0x07060000u)+off;}
static int gpu_read(void *v,enum a733_gpu_power_bank b,uint32_t off,uint32_t *out)
{(void)v;uintptr_t a=address(b,off);*out=a==0x07065008 ? *word(0x07065000)&15 : *word(a);return 1;}
static int gpu_update(void *v,enum a733_gpu_power_bank b,uint32_t off,uint32_t clear,uint32_t set,uint64_t d)
{(void)v;assert(d>model.now);*word(address(b,off))=(*word(address(b,off))&~clear)|set;return 1;}
int a733_npu_platform_native_read(void *v,uintptr_t a,uint32_t *out)
{(void)v;assert(held);*out=a==0x07064008 ? *word(0x07064000)&15 : *word(a);return 1;}
int a733_npu_platform_native_write(void *v,uintptr_t a,uint32_t value)
{(void)v;assert(held);*word(a)=value;return 1;}
static int backend_read(void *v,unsigned a,uint32_t *o){(void)v;(void)a;(void)o;assert(0);return 0;}
static int backend_write(void *v,unsigned a,uint32_t x){(void)v;(void)a;(void)x;assert(0);return 0;}
static uint64_t backend_time(void *v){(void)v;assert(0);return 0;}
static void backend_delay(void *v,unsigned n){(void)v;(void)n;assert(0);}
static void backend_barrier(void *v){(void)v;assert(0);}
static int backend_publish(void *v,const struct a733_npu_mmu_image *i,const struct a733_npu_command *c,int b)
{(void)v;(void)i;(void)c;(void)b;assert(0);return 0;}
static int backend_finish(void *v,const struct a733_npu_command *c,int b)
{(void)v;(void)c;(void)b;assert(0);return 0;}
static void setup(unsigned client)
{
    /* Independent fresh instruction-machine instance per case. */
    (void)pmic_model_setup;model.now=consumer_test_time=100;
    assert(a733_counter_clock_init(&source,24000000));
    locks.initialized=1;locks.now=a733_counter_clock_now;locks.context=&source;
    for(unsigned i=0;i<A733_PLATFORM_LOCK_COUNT;++i)
        locks.mutex[i]=(SemaphoreHandle_t)(uintptr_t)(i+1);
    locks.callbacks=(struct a733_gpu_resource_locks){reg_enter,reg_leave,bus_enter,bus_leave,&locks};
    assert(a733_npu_startup_resources_init(&shared,&source,&locks));
    assert(a733_clock_leases_init(&model.clocks,a733_counter_clock_now,&source));
    const struct a733_rail_binding binding={&model.clocks,&shared.manager,900,a733_counter_clock_now,&source};
    assert(a733_rail_leases_init(&model.rails,&binding));
    const struct a733_pins_operations pins={pmic_model_pin_enter,pmic_model_pin_leave,
        clock_now,pmic_model_pin_read,pmic_model_pin_write};
    struct a733_pin_handoff handoff={0};
    for(unsigned b=1;b<A733_PIN_BANKS;++b)handoff.authorized[b]=a733_pin_bank_mask(b);
    for(unsigned i=0;i<2;++i)handoff.latch[i]=(struct a733_pin_latch_handoff){100+i,900,a733_pin_bank_mask(A733_PIN_L+i),0};
    assert(a733_pins_init_banks(&model.pins,&pins,&model,&handoff));
    const struct a733_pmic_config config={777,888,1000000,{{2,1,1},{2,1,1}}};
    const struct a733_pmic_ops ops={clock_now,pmic_model_current,pmic_model_enter,
        pmic_model_leave,pmic_model_retain,pmic_model_read,pmic_model_write,pmic_model_outside};
    assert(a733_pmic_init(&model.provider,&config,&ops,&model,&model.clocks,&model.rails,&model.pins));
    assert(a733_pmic_identity(&model.provider,&model.lifetime)==A733_PMIC_READY);
    const uintptr_t reg[]={0x07090160,0x0701019c,0x07025000,0x07025014,0x07025024,
        0x0708300c,0x07083010,0x07083014,0x07083020,0x07083200,0x07083210,0x07083218};
    const uint32_t value[]={0x10000002,0x10001,0x22,0x11,5,0x40,0xf8,0x28,0x30,0x00f81000,0x80c0,0x00100010};
    for(unsigned i=0;i<sizeof(reg)/sizeof(*reg);++i)*pmic_model_word(&model,reg[i])=value[i];
    model.pmic[0x0e]=3;model.pmic[0x10]=0x0a;model.pmic[0x13]=model.pmic[0x15]=30;
    struct a733_npu_handoff h={
        .allowed={.fabric={0x03000001,0x82000001,0x84000000},.rate_hz={300000000,400000000,400000000},
            .owner=7,.handoff=123,.deadline_us=1000000},
        .backend={backend_read,backend_write,backend_time,backend_delay,backend_barrier,backend_publish,backend_finish},
        .backend_cookie=&token,.image={0x44000000,0x5000,0x44000000,1},
        .commands={{0x100000,A733_NPU_FETCH_BYTES,&token}},.controller=0x03600000,.evidence=123,
        .command_admission=234,.established=A733_NPU_HANDOFF_REQUIRED,.command_count=1,.pck_client=1};
    assert(a733_npu_startup_init(&npu,&h,&shared,&model.clocks));
    assert(a733_npu_startup_bind_pmic(&npu,&model.provider,&model.lifetime));
    /* Shared-unbound is a real refusal, with no legacy bus callback. */
    struct a733_gpu_resource_rail untouched={9,9,9,9,9,9};
    assert(!shared.native.operations.rail(&shared.native,&untouched,1000000));
    assert(untouched.chip==9 && !model.reads && !model.writes);
    const struct a733_gpu_platform_ops gops={gpu_now,gpu_read,gpu_update};
    assert(a733_gpu_platform_init(&gpu,&shared.manager,client,&gops,0));
    assert(a733_gpu_platform_bind_pmic(&gpu,&shared.native,&model.provider,&model.lifetime,8));
    assert(a733_npu_startup_bind_task(&npu,(TaskHandle_t)1));
    *word(0x07090160)=0;
    *word(0x02002000)=0xf8636300;*word(0x020020a0)=0xfe126310;
    *word(0x020022a0)=0xf8105100;
    *word(0x02003908)=0x0fff0fff;*word(0x02003988)=0x0fff0000;
    *word(0x02003920)=0x10001;*word(0x020039a0)=0x10000;
    *word(0x02002500)=h.allowed.fabric[0];*word(0x02002580)=h.allowed.fabric[1];*word(0x02002588)=h.allowed.fabric[2];
    *word(0x020025c0)=0x010000ff;*word(0x020025e0)=0x41055800;*word(0x02002b04)=0x80000;
    *word(0x070101ac)=1;scheduler=1;
    for(unsigned i=0;i<16 && a733_pmic_live(&model.provider,&model.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&model)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&model.provider,&model.lifetime)==A733_PMIC_READY);
}
static void provider_step(void)
{advance();assert(pmic_model_step(&model)!=A733_PMIC_RETAINED);}
static void npu_ready(void)
{
    enum a733_npu_platform_result r=A733_NPU_PLATFORM_PENDING;uint64_t first=0,second=0,first_time=0;
    for(unsigned i=0;i<1000 && r!=A733_NPU_PLATFORM_READY;++i) {
        r=a733_npu_startup_poll(&npu);assert(r!=A733_NPU_PLATFORM_RETAINED);
        if(npu.settle_until && !first){first=npu.pmic.request.id;first_time=model.now;}
        if(first && npu.pmic.request.id && npu.pmic.request.id!=first) {
            second=npu.pmic.request.id;assert(model.now>=first_time && model.now-first_time>=UINT64_C(5160));
        }
        provider_step();
    }
    assert(r==A733_NPU_PLATFORM_READY && first && second>first && !npu.rail_token.id);
    assert(npu.platform.resources.rail==npu.pmic.rail.id && npu.pmic.snapshot.programmed_uv==800000);
}
unsigned consumer_check(unsigned which)
{
    consumer_test_case=which;setup(which==1 ? 1 : 0);
    if(which==5) {
        int result=0;
        for(unsigned i=0;i<400 && !result;++i) {
            result=a733_pmic_consumer_sample(&gpu.pmic,1000000);assert(result>=0);
            if(!result)provider_step();
        }
        assert(result==1 && a733_pmic_consumer_arm(&gpu.pmic,1000000)==1);
        uint64_t foreign=0;struct a733_gpu_power_resources output={0};
        assert(a733_gpu_resources_acquire(&shared.manager,4,A733_GPU_RESOURCE_RAIL,1000000,
            &foreign,&output)==A733_GPU_RESOURCE_FAULT);
        assert(foreign && !gpu.token && !shared.manager.rail.chip && gpu.pmic.armed);
        assert(!shared.native.i2c_held && !model.i2c_held);return 0;
    }
    if(which==4)late_npu=1;
    if(which==3 || which==4) {
        enum a733_npu_platform_result r=A733_NPU_PLATFORM_PENDING;
        for(unsigned i=0;i<1000 && r!=A733_NPU_PLATFORM_RETAINED;++i) {
            r=a733_npu_startup_poll(&npu);
            if(which==3 && npu.platform.resources.supply_uv) {
                atomic_store(&model.rails.busy,1);model.now=consumer_test_time=1000000;
                assert(a733_npu_startup_poll(&npu)==A733_NPU_PLATFORM_RETAINED);
                assert(npu.pmic.request.id && npu.pmic.rail.id);return 0;
            }
            if(r!=A733_NPU_PLATFORM_RETAINED)provider_step();
        }
        assert(which==4 && r==A733_NPU_PLATFORM_RETAINED && a733_npu_platform_ready(&npu.platform));
        assert(npu.pmic.request.id && npu.pmic.rail.id);return 0;
    }
    npu_ready();
    if(which==2)late_gpu=1;
    assert(a733_gpu_platform_begin(&gpu,1000000));
    int r=0;
    for(unsigned i=0;i<1000 && r==0;++i) {
        r=a733_gpu_platform_poll(&gpu);
        if(which==1 && gpu.pmic_observed) {
            assert(r==0 && !gpu.pmic.armed && !gpu.token);
            assert(!shared.native.operations.rail(&shared.native,&shared.manager.rail,1000000));
            assert(gpu.pmic.request.id && a733_pmic_consumer_health(&gpu.pmic)==1);return 0;
        }
        if(r==0)provider_step();
    }
    if(which==2) {
        assert(r==-1 && gpu.phase==A733_GPU_PLATFORM_FAILED && gpu.power.phase==A733_GPU_POWER_ON);
        assert(gpu.token && gpu.pmic.request.id && gpu.pmic.rail.id);return 0;
    }
    assert(r==1 && gpu.phase==A733_GPU_PLATFORM_ON && !shared.native.i2c_held && !npu.i2c_held);
    assert(!atomic_load(&shared.manager.busy) && a733_pmic_consumer_health(&gpu.pmic)==1);
    assert(a733_rail_release(&model.rails,&gpu.pmic.rail,1000000)!=A733_RAIL_READY);
    assert(a733_rail_release(&model.rails,&npu.pmic.rail,1000000)!=A733_RAIL_READY);
    assert(a733_pmic_consumer_arm(&gpu.pmic,1000000)==1);
    struct a733_pmic_consumer before=gpu.pmic;
    assert(!a733_pmic_consumer_take(&gpu.pmic,&shared.native,1000000,(void *)&gpu.pmic));
    assert(!memcmp(&before,&gpu.pmic,sizeof(before)));gpu.pmic.armed=0;
    /* Completed startup authority survives its former operation deadline. */
    model.now=consumer_test_time=1000100;
    assert(a733_gpu_platform_poll(&gpu)==1);
    assert(a733_npu_startup_prepared(&npu,&npu.handoff.backend,npu.handoff.backend_cookie,
        &npu.handoff.image,npu.handoff.commands,npu.handoff.command_count)==1);
    model.provider.failed=1;
    assert(a733_gpu_platform_poll(&gpu)==-1);
    assert(a733_npu_startup_prepared(&npu,&npu.handoff.backend,npu.handoff.backend_cookie,
        &npu.handoff.image,npu.handoff.commands,npu.handoff.command_count)==-1);
    assert(gpu.pmic.request.id && npu.pmic.request.id);return 0;
}
