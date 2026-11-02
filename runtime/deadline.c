#include "FreeRTOS.h"
#include "task.h"
#include "sunstone_deadline.h"
#include <stdatomic.h>
/* Fixed admission bound, including application and board device owners. Task
 * handles remain valid for this image's lifetime. Each wait uses its owner's
 * slot even when the scheduler migrates the task to another CPU. */
#define DEADLINE_SLOTS 16u
struct deadline_slot {
    _Atomic(TaskHandle_t) owner;
    atomic_uint_fast64_t deadline;
    uint64_t sequence,last_deadline,woken_us;
    unsigned failed;
    unsigned index;                  /* the notification index of the wait in progress */
};
/* The notification index that the calling task's deadline wait uses. The default is index 0, the task's own; the OS
 * returns SS_NOTIFY_DEADLINE for a task of an application, whose index 0 is the application's (include/sunstone_notify.h). */
__attribute__((weak)) unsigned sunstone_deadline_notify_index(void){return 0;}
static struct deadline_slot slots[DEADLINE_SLOTS];
static atomic_uint rearm_requested;
static struct deadline_slot *find(TaskHandle_t owner)
{
    for(unsigned i=0;i<DEADLINE_SLOTS;++i)
        if(atomic_load_explicit(&slots[i].owner,memory_order_acquire)==owner)return &slots[i];
    return NULL;
}
int sunstone_deadline_register(void)
{
    TaskHandle_t owner=xTaskGetCurrentTaskHandle();
    if(!owner)return 0;
    taskENTER_CRITICAL();
    struct deadline_slot *s=find(owner);
    if(!s)for(unsigned i=0;i<DEADLINE_SLOTS;++i)
        if(!atomic_load_explicit(&slots[i].owner,memory_order_relaxed)){
            s=&slots[i];atomic_store_explicit(&s->owner,owner,memory_order_release);break;
        }
    taskEXIT_CRITICAL();
    return s!=NULL;
}
static uint64_t earliest(void)
{
    uint64_t first=0;
    for(unsigned i=0;i<DEADLINE_SLOTS;++i){
        uint64_t d=atomic_load_explicit(&slots[i].deadline,memory_order_acquire);
        if(d&&(!first||d<first))first=d;
    }
    return first;
}
void sunstone_deadline_rearm(void)
{
    if(atomic_exchange_explicit(&rearm_requested,0,memory_order_acquire))
        sunstone_deadline_program(earliest());
}
int sunstone_deadline_wait_until(uint64_t until)
{
    struct deadline_slot *s=find(xTaskGetCurrentTaskHandle());
    if(!s)return SUNSTONE_DEADLINE_ERROR;
    if(!until||until>UINT64_MAX/24){
        taskENTER_CRITICAL();s->failed=1;taskEXIT_CRITICAL();
        return SUNSTONE_DEADLINE_ERROR;
    }
    taskENTER_CRITICAL();
    if(atomic_load_explicit(&s->deadline,memory_order_relaxed)){
        s->failed=1;taskEXIT_CRITICAL();return SUNSTONE_DEADLINE_ERROR;
    }
    if(s->sequence==UINT64_MAX){
        s->failed=1;taskEXIT_CRITICAL();return SUNSTONE_DEADLINE_ERROR;
    }
    ++s->sequence;s->last_deadline=until;s->woken_us=0;
    const unsigned index=sunstone_deadline_notify_index();
    s->index=index;
    uint64_t now=sunstone_deadline_now_us();
    if(now>=until){
        /* A matured hold still creates an observation certificate. There is
         * no hardware alarm, and real progress must follow the caller's check. */
        s->woken_us=now;taskEXIT_CRITICAL();return SUNSTONE_DEADLINE_EXPIRED;
    }
    atomic_store_explicit(&s->deadline,until,memory_order_release);
    atomic_store_explicit(&rearm_requested,1,memory_order_release);
    /* Publish before the SGI. A concurrent CPU0 scan may write an older
     * compare, but this pending SGI then observes the newly published slot. */
    sunstone_deadline_kick();
    taskEXIT_CRITICAL();
    (void)ulTaskNotifyTakeIndexed(index,pdTRUE,portMAX_DELAY);
    taskENTER_CRITICAL();
    int expired=sunstone_deadline_now_us()>=until;
    atomic_store_explicit(&s->deadline,0,memory_order_release);
    if(!s->woken_us)s->woken_us=sunstone_deadline_now_us();
    /* The ISR notifies while holding this same recursive kernel lock. Thus
     * cancellation cannot leave a late timer hint for the next operation.
     * Device hints consumed here are safe only under the public recheck rule. */
    (void)ulTaskNotifyTakeIndexed(index,pdTRUE,0);
    taskEXIT_CRITICAL();
    return expired?SUNSTONE_DEADLINE_EXPIRED:SUNSTONE_DEADLINE_NOTIFIED;
}
void sunstone_deadline_irq(void)
{
    uint64_t now=sunstone_deadline_now_us(),first=0;
    struct deadline_slot *due=NULL;
    for(unsigned i=0;i<DEADLINE_SLOTS;++i){
        uint64_t d=atomic_load_explicit(&slots[i].deadline,memory_order_acquire);
        if(d&&d<=now&&(!first||d<first)){first=d;due=&slots[i];}
    }
    BaseType_t woken=pdFALSE;
    if(due){
        UBaseType_t mask=taskENTER_CRITICAL_FROM_ISR();
        uint64_t d=atomic_load_explicit(&due->deadline,memory_order_relaxed);
        if(d&&d<=now){
            due->woken_us=now;
            atomic_store_explicit(&due->deadline,0,memory_order_release);
            vTaskNotifyGiveIndexedFromISR(atomic_load_explicit(&due->owner,memory_order_relaxed),due->index,&woken);
        }
        taskEXIT_CRITICAL_FROM_ISR(mask);
    }
    /* One notification per IRQ bounds the added kernel critical section.
     * Further due slots keep the level-sensitive compare pending. A remote
     * arm racing this scan also sends SGI0, which will correct a stale min. */
    sunstone_deadline_program(earliest());
    portYIELD_FROM_ISR(woken);
}
int sunstone_deadline_owner_record(void *owner,struct sunstone_deadline_record *out)
{
    if(!owner||!out)return 0;
    taskENTER_CRITICAL();
    struct deadline_slot *s=find(owner);
    if(s){
        out->sequence=s->sequence;out->deadline_us=s->last_deadline;
        out->woken_us=s->woken_us;out->failed=s->failed;
        out->armed=atomic_load_explicit(&s->deadline,memory_order_relaxed)!=0;
    }
    taskEXIT_CRITICAL();return s!=NULL;
}
void sunstone_deadline_owner_progress(void *owner)
{
    if(owner!=xTaskGetCurrentTaskHandle())return;
    taskENTER_CRITICAL();struct deadline_slot *s=find(owner);
    if(s)s->failed=0;
    taskEXIT_CRITICAL();
}
