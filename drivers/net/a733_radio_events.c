#include "a733_radio_events.h"
#include "sunstone_deadline.h"
#include "sunxi_smhc.h"
#include "a733_range.h"
#ifndef A733_RADIO_EVENTS_MMIO_DEFINED
#include "a733_mmio.h"
static uint32_t event_read(uintptr_t address){return a733_mmio_read32_dsb_sy_both(address);}
static void event_write(uintptr_t address,uint32_t value){a733_mmio_write32_dsb_sy_both(address,value);}
static uint64_t event_route(uintptr_t address)
{uint64_t v;__asm__ volatile("dsb sy":::"memory");v=*(volatile uint64_t *)address;__asm__ volatile("dsb sy":::"memory");return v;}
#endif
#define GIC UINT32_C(0x03400000)
#define BIT194 4u
#define SDC UINT32_C(0x04021000)
#if defined(portPRIORITY_SHIFT)
_Static_assert(((((configMAX_API_CALL_INTERRUPT_PRIORITY<<portPRIORITY_SHIFT)>>1)|0x80u))<=0xd8u,
    "SDIO route priority must permit FreeRTOS FromISR calls");
#endif

int a733_radio_events_init(struct a733_radio_events *e,struct a733_radio_platform *p,TaskHandle_t owner)
{
    if(!e||e->initialized||!p||!p->initialized||!owner||p->interrupt_retire||
       !a733_ranges_disjoint(e,sizeof(*e),p,sizeof(*p))||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED||!sunxi_sdio_bind_events(&p->host))return 0;
    e->platform=p;e->owner=owner;e->irq=(struct gicv3_spi){GIC,194};e->masked=1;
    atomic_init(&e->pending,0);atomic_init(&e->failed,0);
    p->interrupt_retire=a733_radio_events_retire;p->interrupt_context=e;
    e->initialized=1;return 1;
}
int a733_radio_events_begin(struct a733_radio_events *e)
{
    if(!e||!e->initialized||e->owner!=xTaskGetCurrentTaskHandle()||portGET_CORE_ID()!=0||
       e->platform->ops.current(e->platform->cookie)!=1||atomic_load(&e->failed))return 0;
    if(e->started)return 1;
    /* The EL3 image table installs secure priority d8. With the distributor's
     * security extension enabled its Nonsecure view reads b0. */
    /* IGROUPR/IGRPMODR are RAZ from Nonsecure EL1 with DS=0. The EL3
     * route installer proves those fields; the owner checks readable state. */
    if((event_read(GIC+0xc30)&(3u<<4))||
       ((event_read(GIC+0x4c0)>>16)&255u)!=0xb0u||event_route(GIC+0x6610)!=0||
       ((event_read(GIC+0x118)|event_read(GIC+0x218)|event_read(GIC+0x318))&BIT194)){
        atomic_store(&e->failed,1);return 0;
    }
    if(!sunstone_deadline_register()){atomic_store(&e->failed,1);return 0;}
    e->registered=1;e->started=1;return 1;
}
void a733_radio_events_irq(struct a733_radio_events *e)
{
    if(!e||!e->initialized)return;
    UBaseType_t saved=taskENTER_CRITICAL_FROM_ISR();
    if(!e->masked){
        e->masked=1;
        if(!gicv3_spi_mask(&e->irq))atomic_store_explicit(&e->failed,1,memory_order_release);
    }
    atomic_store_explicit(&e->pending,1,memory_order_release);
    ++e->interrupts;
    taskEXIT_CRITICAL_FROM_ISR(saved);
    BaseType_t woken=pdFALSE;
    vTaskNotifyGiveFromISR(e->owner,&woken);portYIELD_FROM_ISR(woken);
}
int a733_radio_events_prepare(struct a733_radio_events *e,uint64_t *until)
{
    if(!until||!a733_radio_events_begin(e))return -1;
    if(atomic_load_explicit(&e->failed,memory_order_acquire))return -1;
    /* Mask before inspecting device state; an earlier hint remains in the
     * notification count and the authoritative controller state. */
    taskENTER_CRITICAL();
    int masked=1;
    if(!e->masked){e->masked=1;masked=gicv3_spi_mask(&e->irq);}
    atomic_store_explicit(&e->pending,0,memory_order_release);
    taskEXIT_CRITICAL();
    if(!masked)return -1;
    uint64_t core_until=0;
    int ready=a733_radio_platform_event_prepare(e->platform,&core_until);
    if(ready<0)return -1;
    if(ready)return 1;
    if(core_until&&(!*until||core_until<*until))*until=core_until;
    /* No source is enabled before controller preparation or after shutdown. */
    unsigned running=!e->retired&&e->platform->host.event_live&&e->platform->host.state!=SUNXI_SDIO_QUIESCED;
    if(running){
        taskENTER_CRITICAL();
        int rearmed=0;
        if(!atomic_load_explicit(&e->pending,memory_order_acquire)){
            rearmed=gicv3_spi_rearm(&e->irq);
            if(rearmed==1)e->masked=0;
        }
        taskEXIT_CRITICAL();
        if(rearmed<0)return -1;
        if(!rearmed){
            uint64_t now=sunstone_deadline_now_us();
            if(now>UINT64_MAX-50)return -1;
            if(!*until||now+50<*until)*until=now+50;
        }
    }
    return 0;
}
int a733_radio_events_wait(struct a733_radio_events *e,uint64_t until)
{
    int ready=a733_radio_events_prepare(e,&until);
    if(ready<0)return 0;
    if(ready)return 1;
    if(!until)return 0; /* All native waits retain the immutable session end. */
    int result=sunstone_deadline_wait_until(until);
    if(result==SUNSTONE_DEADLINE_ERROR)return 0;
    ++e->wakes;
    if(result==SUNSTONE_DEADLINE_EXPIRED)++e->deadline_wakes;
    return 1;
}
int a733_radio_events_retire(void *cookie)
{
    struct a733_radio_events *e=cookie;
    if(!e||!e->initialized)return -1;
    if(e->retired)return 1;
    /* A rejected route may carry an inherited source. It grants no pending
     * clear or controller access and cannot authorize the clock-off path. */
    if(!e->started)return -1;
    taskENTER_CRITICAL();int mask=1;
    if(!e->masked){e->masked=1;mask=gicv3_spi_mask(&e->irq);}
    taskEXIT_CRITICAL();
    if(!mask)return -1;
    struct a733_radio_platform *p=e->platform;
    if(p->host.event_live){
        /* Direct retained platform callbacks remain usable after a core
         * fault. They authenticate the sole EL1 owner and accessible aperture. */
        uint32_t control,got;
        if(p->ops.current(p->cookie)!=1||!p->ops.write(p->cookie,SDC+SUNXI_SMHC_IMASK,0)||
           !p->ops.read(p->cookie,SDC+SUNXI_SMHC_GCTRL,&control)||
           !p->ops.write(p->cookie,SDC+SUNXI_SMHC_GCTRL,control&~SUNXI_SMHC_GCTRL_INT_ENABLE)||
           !p->ops.barrier(p->cookie)||!p->ops.read(p->cookie,SDC+SUNXI_SMHC_IMASK,&got)||got||
           !p->ops.read(p->cookie,SDC+SUNXI_SMHC_GCTRL,&got)||(got&SUNXI_SMHC_GCTRL_INT_ENABLE))return -1;
    }
    int disabled=gicv3_spi_masked(&e->irq);
    if(disabled<0)return -1;
    if(!disabled||(event_read(GIC+0x318)&BIT194))return 0;
    /* Controller delivery is now disabled and read back. A stale pending
     * level may be cleared only after the last active handler has returned. */
    event_write(GIC+0x298,BIT194);
    if((event_read(GIC+0x218)|event_read(GIC+0x318))&BIT194)return 0;
    e->retired=1;return 1;
}
