#include "a733_spi_platform_native.h"
#include "a733_platform_locks.h"
#include "task.h"
#include "a733_mmio.h"
#include "a733_task_context.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "SPI native binding requires scheduler-state, task and mutex-owner APIs"
#endif
static int current(void *cookie)
{
    struct a733_spi_platform_native *n=cookie;
    /* Never admit ISR or critical code, nor any task other than the owner. */
    return n && n->initialized && a733_task_context_owner_el1((TaskHandle_t)n->owner_task);
}
static int now(void *cookie,uint64_t *v)
{
    struct a733_spi_platform_native *n=cookie;
    return n->locks->now(n->locks->context,v);
}
static int authorize(void *cookie,const struct a733_spi_platform_config *c,
    const struct a733_clock_token *t,enum a733_spi_platform_admission phase)
{
    struct a733_spi_platform_native *n=cookie;
    if (!current(n) || c->format.port!=n->port) return -1;
    return n->admit(n->admission_cookie,c,t,phase);
}
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_spi_platform_native *n=cookie;
    return current(n) && a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void leave(void *cookie)
{
    struct a733_spi_platform_native *n=cookie;
    a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);
}
static void retain(void *cookie)
{
    struct a733_spi_platform_native *n=cookie;
    configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);
}
int a733_spi_platform_native_address(unsigned port,uintptr_t a,unsigned writing,unsigned width)
{
    if(port>5)return 0;
    uintptr_t base=port==5?0x07092000u:0x02540000u+port*0x1000u;
    if(width==1)return a==base+(writing?0x200u:0x300u);
    if(width!=4||(a&3))return 0;
    if(a>=base&&a<=base+0x400) {
        switch(a-base) {
        case 4:case 8:case 0x10:case 0x14:case 0x18:case 0x20:
        case 0x30:case 0x34:case 0x38:case 0x40:return 1;
        case 0x1c:case 0x88:case 0x400:return !writing;
        default:return 0;
        }
    }
    static const uintptr_t clocks[]={0x02002f00,0x02002f08,0x02002f10,0x02002f20,0x02002f28,0x07010150};
    if(a==clocks[port]||a==clocks[port]+(port==5?12u:4u))return 1;
    if(a==0x07090160u||(port<5&&a==0x02002000u))return !writing;
    unsigned intid=port==5?242:56+port;uintptr_t word=4*(intid/32);
    if(a==0x03400000u||a==0x03400100u+word||a==0x03400200u+word||a==0x03400300u+word)return !writing;
    return a==0x03400180u+word||a==0x03400280u+word;
}
static int allowed(struct a733_spi_platform_native *n,uintptr_t a,unsigned writing,unsigned width)
{
    return current(n)&&a733_spi_platform_native_address(n->port,a,writing,width)&&
        !(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1u)&&
        xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_REGISTER_LOCK])==(TaskHandle_t)n->owner_task;
}
static int read32(void *p,uintptr_t a,uint32_t *v)
{
    struct a733_spi_platform_native *n=p;if(!v||!allowed(n,a,0,4))return 0;
    *v=a733_mmio_read32_dsb_sy_both(a);return 1;
}
static int write32(void *p,uintptr_t a,uint32_t v)
{
    struct a733_spi_platform_native *n=p;if(!allowed(n,a,1,4))return 0;
    struct a733_spi_plan plan;if(a733_spi_plan(n->port,24000000,&plan))return 0;
    if(a==plan.clock||a==plan.reset) {
        uint32_t mask=a==plan.clock?0x87001f1fu:0x10001u;
        a733_mmio_dsb_sy();uint32_t old=a733_mmio_read32(a);
        if((old^v)&~mask)return 0;
    } else if(a>=0x03400000&&a<0x03410000&&v!=(UINT32_C(1)<<(plan.intid%32)))return 0;
    a733_mmio_write32_dsb_sy_both(a,v);return 1;
}
static int read8(void *p,uintptr_t a,uint8_t *v)
{
    struct a733_spi_platform_native *n=p;if(!v||!allowed(n,a,0,1))return 0;
    *v=a733_mmio_read8_dsb_sy_both(a);return 1;
}
static int write8(void *p,uintptr_t a,uint8_t v)
{
    struct a733_spi_platform_native *n=p;if(!allowed(n,a,1,1))return 0;
    a733_mmio_write8_dsb_sy_both(a,v);return 1;
}
int a733_spi_platform_native_init(struct a733_spi_platform_native *n,
    struct a733_platform_locks *locks,void *owner_task,unsigned port,
    int (*admit)(void *,const struct a733_spi_platform_config *,
        const struct a733_clock_token *,enum a733_spi_platform_admission),void *admission_cookie)
{
    if(!n||n->initialized||!locks||!locks->initialized||!owner_task||port>5||!admit)return 0;
    n->locks=locks;n->owner_task=owner_task;n->port=port;n->admit=admit;n->admission_cookie=admission_cookie;
    n->operations=(struct a733_spi_platform_ops){now,current,authorize,enter,leave,retain,read32,write32,read8,write8};
    n->initialized=1;return 1;
}
