#include "a733_radio_platform_native.h"
#include "a733_platform_locks.h"
#include "task.h"
#include "a733_mmio.h"
#include "a733_task_context.h"
#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "Radio native binding requires scheduler-state, task and mutex-owner APIs"
#endif
static int current(void *cookie)
{
    struct a733_radio_platform_native *n=cookie;
    /* Never admit ISR or critical code, or any task but the owner. */
    return n && n->initialized && a733_task_context_owner_el1((TaskHandle_t)n->owner_task);
}

static int now(void *cookie,uint64_t *v)
{
    struct a733_radio_platform_native *n=cookie;
    return n->locks->now(n->locks->context,v);
}
static int admit(void *cookie,const struct a733_radio_platform_config *c,const struct a733_clock_token *t)
{
    struct a733_radio_platform_native *n=cookie;
    if(!current(n))return -1;
    int r=n->admit(n->admission_cookie,c,t);
    return r==1&&n->extension?n->extension(n->extension_cookie,c,t):r;
}
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_radio_platform_native *n=cookie;
    return current(n)&&a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void leave(void *cookie)
{
    struct a733_radio_platform_native *n=cookie;
    a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);
}
static void retain(void *cookie)
{
    struct a733_radio_platform_native *n=cookie;
    configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);
}
static int allowed(struct a733_radio_platform_native *n,uintptr_t a,unsigned writing)
{
    if(!current(n)||!a733_radio_platform_native_address(a,writing))return 0;
    if(a>=UINT32_C(0x04021000)&&a<=UINT32_C(0x04021300))return 1;
    return !(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1u)&&
        xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_REGISTER_LOCK])==(TaskHandle_t)n->owner_task;
}
static int read32(void *cookie,uintptr_t a,uint32_t *v)
{
    struct a733_radio_platform_native *n=cookie;
    if(!v||!allowed(n,a,0))return 0;
    *v=a733_mmio_read32_dsb_sy_both(a);return 1;
}
static int write32(void *cookie,uintptr_t a,uint32_t v)
{
    struct a733_radio_platform_native *n=cookie;
    if(!allowed(n,a,1))return 0;
    if(a==UINT32_C(0x03400198)) {
        if(v!=4)return 0;
    } else if(a==UINT32_C(0x02002d10)||a==UINT32_C(0x02002d1c)) {
        uint32_t mask=a==UINT32_C(0x02002d10)?UINT32_C(0x87001f1f):UINT32_C(0x10001);
        if(a==UINT32_C(0x02002d10)&&(v&(mask&~UINT32_C(0x80000000)))!=29)return 0;
        a733_mmio_dsb_sy();
        uint32_t old=a733_mmio_read32(a);
        if((old^v)&~mask)return 0;
    }
    a733_mmio_write32_dsb_sy_both(a,v);return 1;
}
static int barrier(void *cookie)
{
    if(!current(cookie))return 0;
    a733_mmio_dsb_sy();return 1;
}
int a733_radio_platform_native_init(struct a733_radio_platform_native *n,
    struct a733_platform_locks *locks,void *task,
    int (*admission)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *),void *cookie)
{
    if(!n||n->initialized||!locks||!locks->initialized||!task||!admission)return 0;
    n->locks=locks;n->owner_task=task;n->admit=admission;n->admission_cookie=cookie;
    n->operations=(struct a733_radio_platform_ops){now,current,admit,enter,leave,retain,read32,write32,barrier};
    n->initialized=1;return 1;
}
int a733_radio_platform_native_extend(struct a733_radio_platform_native *n,
    int (*admission)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *),void *cookie)
{
    if(!n||!n->initialized||n->extension||!admission||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    n->extension=admission;n->extension_cookie=cookie;return 1;
}
