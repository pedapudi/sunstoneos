#include "supervisor-wait.h"

/* The precise backend converts microseconds to the 24 MHz counter domain. */
#define LAST_US (UINT64_MAX/24u)
static int refuse(struct os_supervisor_wait *w)
{w->failed=1;return SUNSTONE_DEADLINE_ERROR;}

int os_supervisor_wait_init(struct os_supervisor_wait *w)
{
    if(!w||w->owner||w->failed)return 0;
    TaskHandle_t owner=xTaskGetCurrentTaskHandle();
    if(!owner)return 0;
    w->owner=owner;
    if(!sunstone_deadline_register()){(void)refuse(w);return 0;}
    uint64_t now=sunstone_deadline_now_us();
    if(now>LAST_US-OS_SUPERVISOR_OBSERVATION_US){(void)refuse(w);return 0;}
    w->last_us=now;w->next_us=now+OS_SUPERVISOR_OBSERVATION_US;
    return 1;
}
int os_supervisor_wait_next(struct os_supervisor_wait *w)
{
    if(!w||!w->owner||w->owner!=xTaskGetCurrentTaskHandle()||w->failed)
        return SUNSTONE_DEADLINE_ERROR;
    uint64_t now=sunstone_deadline_now_us();
    if(!w->next_us||now<w->last_us||now>LAST_US)return refuse(w);
    if(now>=w->next_us){
        uint64_t phases=(now-w->next_us)/OS_SUPERVISOR_OBSERVATION_US+1;
        if(phases>(LAST_US-w->next_us)/OS_SUPERVISOR_OBSERVATION_US)return refuse(w);
        w->next_us+=phases*OS_SUPERVISOR_OBSERVATION_US;
    }
    uint64_t until=w->next_us;
    int event=sunstone_deadline_wait_until(until);
    uint64_t observed=sunstone_deadline_now_us();
    if((event!=SUNSTONE_DEADLINE_NOTIFIED&&event!=SUNSTONE_DEADLINE_EXPIRED)||
       observed<now||observed>LAST_US)return refuse(w);
    w->last_us=observed;
    return event;
}
