#include "a733_uart_platform_native.h"
#include "a733_platform_locks.h"
#include "task.h"
#include "a733_mmio.h"
#include "a733_task_context.h"

#if INCLUDE_xTaskGetSchedulerState != 1 || INCLUDE_xTaskGetCurrentTaskHandle != 1 || INCLUDE_xSemaphoreGetMutexHolder != 1
#error "UART native binding requires scheduler-state, task and mutex-owner APIs"
#endif
static int current(void *cookie)
{
    struct a733_uart_platform_native *n=cookie;
    /* Never admit ISR or critical code, nor any task other than the owner. */
    return n && n->initialized && a733_task_context_owner_el1((TaskHandle_t)n->owner_task);
}
static int now(void *cookie,uint64_t *v)
{
    struct a733_uart_platform_native *n=cookie;
    return n->locks->now(n->locks->context,v);
}
static int authorize(void *cookie,const struct a733_uart_platform_config *c,
    const struct a733_clock_token *t,enum a733_uart_admission phase)
{
    struct a733_uart_platform_native *n=cookie;
    if (!current(n) || c->format.port!=n->port) return -1;
    return n->admit(n->admission_cookie,c,t,phase);
}
static int enter(void *cookie,uint64_t deadline)
{
    struct a733_uart_platform_native *n=cookie;
    return current(n) && a733_platform_lock_enter(n->locks,A733_PLATFORM_REGISTER_LOCK,deadline);
}
static void leave(void *cookie)
{
    struct a733_uart_platform_native *n=cookie;
    a733_platform_lock_leave(n->locks,A733_PLATFORM_REGISTER_LOCK);
}
static void retain(void *cookie)
{
    struct a733_uart_platform_native *n=cookie;
    configASSERT(a733_platform_lock_retain(n->locks,A733_PLATFORM_REGISTER_LOCK)==1);
}
static int access_allowed(struct a733_uart_platform_native *n,uintptr_t a,unsigned writing)
{
    if (!current(n) || !a733_uart_platform_native_address(n->port,a,writing)) return 0;
    uintptr_t base=n->port<7 ? UINT32_C(0x02500000)+n->port*0x1000u : UINT32_C(0x07080000)+(n->port-7)*0x1000u;
    if (a>=base && a<=base+0xc0) return 1; /* Exclusive controller, owner task. */
    return !(atomic_load_explicit(&n->locks->retained,memory_order_acquire)&1u) &&
        xSemaphoreGetMutexHolder(n->locks->mutex[A733_PLATFORM_REGISTER_LOCK])==(TaskHandle_t)n->owner_task;
}
static int read32(void *cookie,uintptr_t a,uint32_t *v)
{
    struct a733_uart_platform_native *n=cookie;
    if (!v || !access_allowed(n,a,0)) return 0;
    *v=a733_mmio_read32_dsb_sy_both(a); return 1;
}
static int write32(void *cookie,uintptr_t a,uint32_t v)
{
    struct a733_uart_platform_native *n=cookie;
    if (!access_allowed(n,a,1)) return 0;
    uintptr_t base=n->port<7 ? UINT32_C(0x02500000)+n->port*0x1000u : UINT32_C(0x07080000)+(n->port-7)*0x1000u;
    if (a>=base && a<=base+0xc0) {
        /* FCR accepts only the port's planned trigger, with or without the
         * FIFO reset bits, and the DMA mode value. MCR accepts the planned
         * flow control alone or with loopback, IrDA SIR and RS-485 9-bit.
         * Offset 0 takes a ninth (RS-485 address) bit only as THR: after the
         * last MCR write selected RS-485 and while the last LCR write left
         * DLAB clear, so DLL never takes it. */
        uint32_t fcr=a733_uart_fcr(n->port);
        uint32_t mcr=v&~0x10u;
        unsigned ninth=a==base && n->mcr==0xa2 && !(n->lcr&0x80);
        if (v>(ninth ? 0x1ffu : 255u) || (a==base+0xa4 && (v&~7u) && v!=0xc2) ||
            (a==base+8 && v!=fcr && v!=(fcr|6u) && v!=9) ||
            (a==base+0x10 && mcr!=0 && mcr!=0x22 && v!=0x40 && v!=0xa2) ||
            (a==base+0x88 && v!=0xe5) || (a==base+0x8c && v!=0 && v!=3) ||
            (a==base+0xc0 && v!=0 && v!=0x18)) return 0;
        if (a==base+0x10) n->mcr=v;
        else if (a==base+0xc) n->lcr=v;
    } else if (a>=UINT32_C(0x03400000) && a<UINT32_C(0x03410000)) {
        unsigned intid=n->port<7 ? 34+n->port : 233+n->port-7;
        if (v!=(UINT32_C(1)<<(intid%32))) return 0;
    } else {
        uint32_t gate=n->port<7 ? 1u : UINT32_C(1)<<(n->port-7);
        uint32_t mask=gate|(gate<<16);
        a733_mmio_dsb_sy();
        uint32_t old=a733_mmio_read32(a);
        if ((old^v)&~mask) return 0;
    }
    a733_mmio_write32_dsb_sy_both(a,v); return 1;
}
int a733_uart_platform_native_init(struct a733_uart_platform_native *n,
    struct a733_platform_locks *locks,void *owner_task,unsigned port,
    int (*admit)(void *,const struct a733_uart_platform_config *,
        const struct a733_clock_token *,enum a733_uart_admission),void *admission_cookie)
{
    if (!n || n->initialized || !locks || !locks->initialized || !owner_task || port>8 || !admit) return 0;
    n->locks=locks;n->owner_task=owner_task;n->port=port;n->admit=admit;n->admission_cookie=admission_cookie;
    n->mcr=n->lcr=0;
    n->operations=(struct a733_uart_platform_ops){now,current,authorize,enter,leave,retain,read32,write32};
    n->initialized=1;return 1;
}
