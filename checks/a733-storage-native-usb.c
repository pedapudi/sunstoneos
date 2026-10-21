/* Execute the target USB framing and provider guards. USB hardware, scheduler,
 * counter and watchdog observations are modeled; provider metadata is real.
 * This does not execute the target bootstrap or qualify a maintenance lease. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
#include "../platform/a733-storage-update/bootstrap.c"
#include "../platform/a733-storage-update/usb.c"
#include "../platform/a733/native_usb.c"
#include <setjmp.h>
#include <stdlib.h>
static struct pmic_model power;
static struct a733_storage_native state;
static struct a733_native_usb *const u=&state.providers;
static jmp_buf stopped;
static uint64_t clock_now=100,clock_next;
static unsigned delays,revoked,io_calls,dcd_calls;
static int lease_alive=1;
static uint8_t wire[128];static unsigned wire_at,wire_size;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void *)3;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q){(void)q;return (void *)3;}
void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
    if(atomic_load(&state.failed))longjmp(stopped,1);
    ++delays;atomic_store(&u->clocks.busy,0);
}
int a733_counter_clock_now(void *c,uint64_t *out)
{(void)c;*out=clock_now;if(clock_next){clock_now=clock_next;clock_next=0;}return 1;}
int a733_maintenance_alive(struct a733_maintenance_lease *lease){(void)lease;return lease_alive;}
void a733_maintenance_revoke(struct a733_maintenance_lease *lease){(void)lease;++revoked;}
int a733_usb_platform_native_read(void *p,uintptr_t a,uint32_t *out)
{(void)p;(void)a;++io_calls;*out=0;return 1;}
int a733_usb_platform_native_write(void *p,uintptr_t a,uint32_t v)
{(void)p;(void)a;(void)v;++io_calls;return 1;}
void dcd_int_handler(uint8_t p){assert(!p);++dcd_calls;}
bool tud_mounted(void){return true;}
uint32_t tud_vendor_n_available(uint8_t i){assert(!i);return wire_size-wire_at;}
uint32_t tud_vendor_n_read(uint8_t i,void *p,uint32_t bytes)
{assert(!i&&bytes<=wire_size-wire_at);memcpy(p,wire+wire_at,bytes);wire_at+=bytes;return bytes;}
/* Sanitizer global metadata retains the target task table. These functions
 * must never execute in this guard-only fixture. */
bool tusb_rhport_init(uint8_t p,const tusb_rhport_init_t *i){(void)p;(void)i;abort();}
void tud_task_ext(uint32_t timeout,bool isr){(void)timeout;(void)isr;abort();}
uint32_t tud_vendor_n_write(uint8_t i,const void *p,uint32_t bytes){(void)i;(void)p;(void)bytes;abort();}
uint32_t tud_vendor_n_write_flush(uint8_t i){(void)i;abort();}
int a733_maintenance_poll(struct a733_maintenance_lease *p){(void)p;abort();}
enum a733_usb_platform_result a733_usb_platform_poll(struct a733_usb_platform *p){(void)p;abort();}
int a733_platform_lock_enter(struct a733_platform_locks *p,enum a733_platform_lock lock,uint64_t deadline)
{(void)p;(void)lock;(void)deadline;abort();}
void a733_platform_lock_leave(struct a733_platform_locks *p,enum a733_platform_lock lock){(void)p;(void)lock;abort();}
int a733_platform_lock_retain(struct a733_platform_locks *p,enum a733_platform_lock lock){(void)p;(void)lock;abort();}
void a733_storage_native_pmic_task(void *p){(void)p;abort();}
void a733_storage_native_worker_task(void *p){(void)p;abort();}
static void setup(void)
{
    pmic_model_setup(&power);
    assert(a733_clock_leases_init(&u->clocks,pmic_model_now,&power));
    const struct a733_gpu_resource_ops go={pmic_model_now,pmic_model_gpu_read,pmic_model_gpu_write,pmic_model_gpu_rail};
    assert(a733_gpu_resources_init(&u->gpu,&go,&power));
    const struct a733_rail_binding rb={&u->clocks,&u->gpu,900,pmic_model_now,&power};
    assert(a733_rail_leases_init(&u->rails,&rb));
    const struct a733_pins_operations po={pmic_model_pin_enter,pmic_model_pin_leave,pmic_model_now,pmic_model_pin_read,pmic_model_pin_write};
    struct a733_pin_handoff handoff={0};
    for(unsigned b=1;b<A733_PIN_BANKS;++b)handoff.authorized[b]=a733_pin_bank_mask(b);
    for(unsigned i=0;i<2;++i)handoff.latch[i]=(struct a733_pin_latch_handoff){100+i,900,a733_pin_bank_mask(A733_PIN_L+i),0};
    assert(a733_pins_init_banks(&u->pins,&po,&power,&handoff));
    assert(a733_pmic_init(&u->pmic,&power.provider.config,&power.provider.ops,&power,&u->clocks,&u->rails,&u->pins));
    assert(a733_pmic_identity(&u->pmic,&u->usb_power)==A733_PMIC_READY);
    for(unsigned i=0;i<16&&a733_pmic_live(&u->pmic,&u->usb_power)!=A733_PMIC_READY;++i){power.worker=1;assert(a733_pmic_poll(&u->pmic)!=A733_PMIC_RETAINED);power.worker=0;}
    assert(a733_pmic_live(&u->pmic,&u->usb_power)==A733_PMIC_READY);
    a733_rail_mask_t rails=A733_RAIL_BIT(A733_RAIL_DCDC1)|A733_RAIL_BIT(A733_RAIL_DC1SW2)|A733_RAIL_BIT(A733_RAIL_DCDC9)|A733_RAIL_BIT(A733_RAIL_ELDO6);
    assert(a733_rail_acquire(&u->rails,1001,rails,0,1000000,&u->usb_rail)==A733_RAIL_PENDING);
    enum a733_rail_result r=A733_RAIL_PENDING;for(unsigned i=0;i<16&&r!=A733_RAIL_READY;++i)r=a733_rail_poll(&u->rails,&u->usb_rail);assert(r==A733_RAIL_READY);
    assert(a733_clock_acquire(&u->clocks,1001,A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_CLOCK_USB01_REF,A733_CLOCK_USB0,1000000,&u->usb.token)==A733_CLOCK_OK);
    u->usb_rail_index=4;state.boot.usb.owner=1001;u->boot.usb=state.boot.usb;u->usb.config=u->boot.usb;
    u->live=providers_live;u->fail=providers_fail;u->context=&state;u->usb_task=(void *)3;
    /* Guard-only fixture: model the platform's final publication; do not claim
     * that this fixture executes USB reset/PHY preparation or boot admission. */
    u->usb.self=(uintptr_t)&u->usb;u->usb.initialized=1;
    atomic_init(&u->usb_prepared,1);atomic_init(&state.active,1);atomic_init(&state.failed,0);
    atomic_init(&u->locks.retained,0);state.task_handle[2]=(void *)3;
    state.boot.grant.deadline_us=1000000;memset(state.boot.grant.nonce,1,32);
    a733_storage_channel_init(&state.channel);runtime=&state;polling=1;
}
int main(void)
{
    setup();uint32_t value;
    state.boot.mmc.owner=1001;state.mmc.config=state.boot.mmc;
    assert(mmc_admit(&state,&state.mmc.config,&u->usb.token)==1);
    atomic_store(&u->clocks.busy,1);assert(mmc_admit(&state,&state.mmc.config,&u->usb.token)==0);atomic_store(&u->clocks.busy,0);
    assert(usb_authorize(u,&u->usb.config,&u->usb.token)==1);
    assert(usb_read(u,0x02000000,&value)&&usb_write(u,0x02000000,0)&&io_calls==2);
    atomic_store(&u->clocks.busy,1);assert(usb_authorize(u,&u->usb.config,&u->usb.token)==0);atomic_store(&u->clocks.busy,0);
    atomic_store(&u->rails.busy,1);assert(usb_authorize(u,&u->usb.config,&u->usb.token)==0);atomic_store(&u->rails.busy,0);
    atomic_store(&u->pmic.busy,1);assert(usb_authorize(u,&u->usb.config,&u->usb.token)==0);atomic_store(&u->pmic.busy,0);
    atomic_store(&u->clocks.busy,1);a733_usb_poll();assert(delays==1&&dcd_calls==1);
    /* Fault published before the callback's post-mutex check. */
    assert(a733_clock_retain(&u->clocks,&u->usb.token)==A733_CLOCK_FAULT);
    assert(!usb_read(u,0x02000000,&value)&&!usb_write(u,0x02000000,0)&&io_calls==2);
    assert(u->usb_io_denial==A733_NATIVE_USB_DENIAL_PROVIDER&&u->usb_provider_denial==A733_NATIVE_USB_PROVIDER_CLOCK);
    if(!setjmp(stopped)){a733_usb_poll();assert(0);}assert(dcd_calls==1&&revoked==1);
    /* Only guard fixture state is reset; actual provider lifetime stays failed.
     * The remaining cases exercise callbacks that do not access providers. */
    atomic_store(&state.failed,0);atomic_store(&state.channel.lost,0);atomic_store(&state.transport.lost,0);
    clock_now=state.boot.grant.deadline_us-1;clock_next=state.boot.grant.deadline_us+1;
    unsigned before=delays;
    if(!setjmp(stopped)){tusb_time_delay_ms_api(0);assert(0);}assert(delays==before&&revoked==2);
    atomic_store(&state.failed,0);atomic_store(&state.channel.lost,0);atomic_store(&state.transport.lost,0);clock_now=100;
    put32(wire,MESSAGE_MAGIC);put32(wire+4,1);put32(wire+8,UPDATE_COMMAND);put32(wire+12,88);
    memcpy(wire+16,state.boot.grant.nonce,32);put32(wire+48,1);put32(wire+52,A733_STORAGE_BACKUP);wire_size=104;
    receive();assert(!atomic_load(&state.transport.accepted_sequence));receive();
    assert(sequence==1&&atomic_load(&state.channel.state)==1&&atomic_load(&state.transport.accepted_sequence)==1);
    struct storage_digest digest;uint8_t expected[32];storage_digest_init(&digest);assert(storage_digest_add(&digest,wire,104));storage_digest_end(&digest,expected);
    for(unsigned i=0;i<4;++i){uint64_t word=0;for(unsigned j=0;j<8;++j)word|=(uint64_t)expected[i*8+j]<<(8*j);assert(atomic_load(&state.transport.accepted_hash[i])==word);}
    response_bytes=sent=72;transferred=0;
    tud_vendor_tx_cb(0,32);assert(!atomic_load(&state.transport.completed_sequence));
    tud_vendor_tx_cb(0,40);assert(atomic_load(&state.transport.completed_sequence)==1);
    tud_umount_cb();assert(atomic_load(&state.channel.lost)&&atomic_load(&state.transport.lost));
    puts("PASS: target USB provider guards, bounded contention/cutoff, full-frame publication, partial/final IN completion and disconnect loss");
    return 0;
}
