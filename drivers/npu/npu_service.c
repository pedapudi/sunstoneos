#include "npu_arch.h"
#include "npu_service.h"
#include "sunstone_crumb.h"
#include "a733_range.h"
#ifdef SUNSTONE_A733_OS
#include "sunstone_deadline.h"
#endif
#include <string.h>

static struct npu_service *instance;
static unsigned creating_worker;
enum { SERVICE_WAITING, SERVICE_CHECKING, SERVICE_ACTIVE, SERVICE_RETAINED,
       SERVICE_PARKED, SERVICE_PREPARING, SERVICE_RESUMING };
static void worker(void *);
extern BaseType_t npu_runtime_queue_task_privileged(TaskFunction_t, void *) __attribute__((weak));

BaseType_t npu_service_task_privileged(TaskFunction_t entry, void *argument)
{
    if (creating_worker && entry == worker && argument == instance) return pdTRUE;
    return npu_runtime_queue_task_privileged ? npu_runtime_queue_task_privileged(entry, argument) : pdFALSE;
}
static int duration(uint64_t us) { return us && us <= INT64_MAX; }
static int active(enum a733_npu_state state)
{
    return state == A733_NPU_BOOTING || state == A733_NPU_RUNNING ||
           state == A733_NPU_CANCEL_ASSERTED || state == A733_NPU_CANCEL_WAIT;
}
enum { IRQ_EVENT=1u, IRQ_FAILED=2u, SERVICE_DEFERRED=5, SERVICE_HELD=6 };
/* last is NPU_SERVICE_WAIT outside runtime mode, which folds away the LOAD
 * comparison there. */
static inline int valid_request(const struct npu_service_request *r, enum npu_service_operation last)
{
    return (unsigned)r->operation <= (unsigned)last &&
        (r->operation == NPU_SERVICE_SUBMIT || r->operation == NPU_SERVICE_LOAD || !r->command) &&
        ((r->operation == NPU_SERVICE_OPEN || r->operation == NPU_SERVICE_SUBMIT ||
          r->operation == NPU_SERVICE_CANCEL) ? duration(r->timeout_us) : r->timeout_us == 0) &&
        (r->operation != NPU_SERVICE_CANCEL || r->timeout_us > 100);
}
static void irq_fault(struct npu_service *s)
{
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    if(s->controller.state!=A733_NPU_FAULT) {
        s->controller.state=A733_NPU_FAULT;s->controller.fault=A733_NPU_IRQ;
    }
#else
    s->controller.state=A733_NPU_FAULT;s->controller.fault=A733_NPU_IRQ;
#endif
    atomic_fetch_or_explicit(&s->signals->irq_pending,IRQ_FAILED,memory_order_release);
}
enum { IRQ_MASK_WAIT, IRQ_REARM_WAIT, IRQ_STOP_WAIT };
#ifdef SUNSTONE_A733_OS
/* GICD_CTLR.RWP and active-bit propagation have no completion interrupt
 * (Arm IHI 0069G, GICD_CTLR). Keep the existing 10 ms availability budget;
 * retry on a precise compare, independently of the scheduler tick. */
#define IRQ_WAIT_US UINT64_C(10000)
#define REGISTER_RETRY_US UINT64_C(50)
struct npu_service_wake_record npu_service_wakes;
static int service_wait(struct npu_service *s)
{
    const uint64_t now=sunstone_deadline_now_us();
    uint64_t until=0;
    const struct a733_npu_controller *c=&s->controller;
    const int busy=active(c->state);
    if(busy){
        const int cancel=c->state==A733_NPU_CANCEL_ASSERTED||c->state==A733_NPU_CANCEL_WAIT;
        const uint64_t elapsed=s->ops.microseconds(s->cookie)-(cancel?c->cancel_start:c->start);
        const uint64_t budget=cancel?c->cancel_timeout:c->timeout;
        until=now+(elapsed<budget?budget-elapsed:0);
        /* After an event, full-idle/drain and cancellation acknowledgments
         * have no further IRQ. A cancellation pulse lasts at least 101 us. */
        if(c->irq_seen||cancel){
            uint64_t retry=now+REGISTER_RETRY_US;
            if(c->state==A733_NPU_CANCEL_ASSERTED){
                const uint64_t held=s->ops.microseconds(s->cookie)-c->cancel_asserted_us;
                retry=now+(held<101?101-held:0);
            }
            if(retry<until)until=retry;
        }
    }
    if(!s->irq_retained&&(s->irq_waiting||s->irq_rearming||s->has_deferred||(!s->irq_masked&&
        (c->state==A733_NPU_FAULT||atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire))))){
        uint64_t retry=now+REGISTER_RETRY_US;
        for(unsigned i=0;i<3;++i)if((s->irq_waiting&(1u<<i))&&s->irq_wait_start[i]<retry)
            retry=s->irq_wait_start[i];
        if(!until||retry<until)until=retry;
    }
    if(!s->irq_bound&&busy&&
       (!until||now+REGISTER_RETRY_US<until))until=now+REGISTER_RETRY_US;
    ++npu_service_wakes.waits;
    const int result=until?sunstone_deadline_wait_until(until):
        ((void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY),SUNSTONE_DEADLINE_NOTIFIED);
    if(result==SUNSTONE_DEADLINE_ERROR){irq_fault(s);return 1;}
    if(result==SUNSTONE_DEADLINE_EXPIRED)++npu_service_wakes.deadlines;
    else ++npu_service_wakes.events;
    if(!busy&&!s->irq_waiting&&!s->has_deferred){
        npu_service_wakes.idle_us+=sunstone_deadline_now_us()-now;
        if(result==SUNSTONE_DEADLINE_EXPIRED)++npu_service_wakes.idle_deadlines;
        else ++npu_service_wakes.idle_events;
    }
    return result==SUNSTONE_DEADLINE_EXPIRED;
}
#endif
/* Availability policy for scheduler-driven callbacks, not proof of hardware
 * completion. The extra tick covers the first sample's unknown tick phase. */
#ifndef SUNSTONE_A733_OS
#define IRQ_WAIT_TICKS (pdMS_TO_TICKS(10) + 1)
#endif
static int irq_call(struct npu_service *s, unsigned which, int (*call)(void *))
{
    unsigned bit = 1u << which;
    if (s->irq_retained) return -1;
    if ((s->irq_waiting & bit) &&
#ifdef SUNSTONE_A733_OS
        sunstone_deadline_now_us() >= s->irq_wait_start[which]) {
#else
        (TickType_t)(xTaskGetTickCount() - s->irq_wait_start[which]) >= IRQ_WAIT_TICKS) {
#endif
        s->irq_retained = 1;
        irq_fault(s);
        return -1;
    }
    int result = call(s->irq_cookie);
    if (!result) {
        if (!(s->irq_waiting & bit)) {
#ifdef SUNSTONE_A733_OS
            s->irq_wait_start[which] = sunstone_deadline_now_us()+IRQ_WAIT_US;
#else
            s->irq_wait_start[which] = xTaskGetTickCount();
#endif
            s->irq_waiting |= bit;
        }
    } else s->irq_waiting &= ~bit;
    return result;
}
/* The permission is one bit of the client's bitmap, tested against the commands the service holds. */
static int command_allowed(const struct npu_service *s, unsigned client, unsigned command)
{
    return client < s->client_count && command < s->command_count &&
           (s->clients[client].commands[command / 32u] >> (command % 32u) & 1u);
}
static int irq_mask(struct npu_service *s)
{
    if(!s->irq_bound || s->irq_masked)return 1;
    if(s->irq_retained)return -1;
    int r=irq_call(s, IRQ_MASK_WAIT, s->irq_ops.mask);
    if(r==1){s->irq_masked=1;s->irq_rearming=0;s->irq_waiting&=~(1u<<IRQ_REARM_WAIT);}
    else if(r<0){s->irq_retained=1;irq_fault(s);}
    return r;
}
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
static int diagnostic_fault_at(struct npu_service *s,enum a733_npu_fault why,
    uint64_t operand,unsigned sample,uint32_t site,const void *where)
{
    if(s->controller.state!=A733_NPU_FAULT) {
        struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
        uint64_t ticket=d->job_ticket ? d->job_ticket : s->job_ticket;
        site|=SUNSTONE_CRUMB_NPU_CANCEL|((sample&3)<<23)|((uint32_t)(ticket&127)<<16);
        uint32_t value=(uint32_t)operand;
        if(operand>UINT32_MAX){site|=SUNSTONE_CRUMB_NPU_CANCEL_HIGH_WORD;value=(uint32_t)(operand>>32);}
        if(!d->fault_recorded) {
            d->fault_operand=operand;d->fault_ticket=ticket;d->fault_sample=sample;
            d->fault_site=site;d->fault_caller=(uintptr_t)where;d->fault_recorded=1;
            sunstone_crumb_fail(site,value,where);
        }
        s->controller.state=A733_NPU_FAULT;s->controller.fault=why;
    }
    (void)site;(void)where;
    return 0;
}
#define diagnostic_fault_value(s,why,value,sample) diagnostic_fault_at((s),(why),(value),(sample),SUNSTONE_CRUMB_FAIL_SITE(NPU),SUNSTONE_CRUMB_HERE())
#define diagnostic_fault(s,why) diagnostic_fault_value((s),(why),(uint64_t)(why),3)
static int diagnostic_live(struct npu_service *s)
{
    struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
    if(!d)return 1;
    int result=d->check(d->cookie,2);
    return result==1 || diagnostic_fault_value(s,A733_NPU_MEMORY,(uint32_t)result,3);
}
static int diagnostic_begin_irq(struct npu_service *s)
{
    struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
    if(!d)return 1;
    unsigned receipts=atomic_load_explicit(&d->irq_receipts,memory_order_acquire);
    if(receipts!=d->accounted_receipts)return diagnostic_fault_value(s,A733_NPU_IRQ,receipts,3);
    unsigned pending=atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire);
    if(pending)return diagnostic_fault_value(s,A733_NPU_IRQ,pending,3);
    d->receipt_base=receipts;
    atomic_store_explicit(&d->irq_window,1,memory_order_release);
    return 1;
}
static int diagnostic_active(struct npu_service *s)
{
    struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
    struct a733_npu_controller *c=&s->controller;
    const struct a733_npu_command *command=&s->commands[d->command];
    if(!d->started)return diagnostic_fault_value(s,A733_NPU_REGISTER,d->started,3);
    if(d->proven)return diagnostic_fault_value(s,A733_NPU_REGISTER,d->proven,3);
    if(c->state!=A733_NPU_RUNNING)return diagnostic_fault_value(s,A733_NPU_IRQ,c->state,3);
    if(s->job_ticket!=d->job_ticket)return diagnostic_fault_value(s,A733_NPU_IRQ,s->job_ticket,3);
    if(c->job.token!=command->token)return diagnostic_fault_value(s,A733_NPU_MEMORY,(uintptr_t)c->job.token,3);
    if(c->job.device!=command->device)return diagnostic_fault_value(s,A733_NPU_REGISTER,c->job.device,3);
    unsigned pending=atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire);
    if(pending)return diagnostic_fault_value(s,A733_NPU_IRQ,pending,3);
    unsigned receipts=atomic_load_explicit(&d->irq_receipts,memory_order_acquire);
    if(receipts!=d->accounted_receipts)return diagnostic_fault_value(s,A733_NPU_IRQ,receipts,3);
    for(unsigned i=0;i<3;++i) {
        struct npu_service_activity_sample *sample=&d->samples[i];
        if(!diagnostic_live(s))return 0;
        sample->time_us=s->ops.microseconds(s->cookie);
        sample->job_ticket=s->job_ticket;sample->state=c->state;
        if(sample->time_us-d->trigger_us>2000)return diagnostic_fault_value(s,A733_NPU_TIMEOUT,sample->time_us-d->trigger_us,i);
        if(!s->ops.read(s->cookie,4,&sample->idle))return diagnostic_fault_value(s,A733_NPU_IO,4,i);
        if(!s->ops.read(s->cookie,0x660,&sample->frontend[0]))return diagnostic_fault_value(s,A733_NPU_IO,0x660,i);
        if(!s->ops.read(s->cookie,0x660,&sample->frontend[1]))return diagnostic_fault_value(s,A733_NPU_IO,0x660,i);
        if(!s->ops.read(s->cookie,0x664,&sample->fetch[0]))return diagnostic_fault_value(s,A733_NPU_IO,0x664,i);
        if(!s->ops.read(s->cookie,0x664,&sample->fetch[1]))return diagnostic_fault_value(s,A733_NPU_IO,0x664,i);
        if(sample->state!=A733_NPU_RUNNING)return diagnostic_fault_value(s,A733_NPU_REGISTER,sample->state,i);
        if(sample->job_ticket!=d->job_ticket)return diagnostic_fault_value(s,A733_NPU_IRQ,sample->job_ticket,i);
        if(sample->idle&1)return diagnostic_fault_value(s,A733_NPU_NOT_IDLE,sample->idle,i);
        for(unsigned j=0;j<2;++j)if((sample->fetch[j]&7) || sample->fetch[j]<command->device ||
            (uint64_t)sample->fetch[j]>=command->device+command->readable_bytes)
                return diagnostic_fault_value(s,A733_NPU_REGISTER,sample->fetch[j],i);
        if(i!=2)s->ops.delay(s->cookie,101);
    }
    uint64_t now=s->ops.microseconds(s->cookie);
    if(now-d->trigger_us>2000)return diagnostic_fault_value(s,A733_NPU_TIMEOUT,now-d->trigger_us,2);
    /* A pre-existing cancel acknowledgment cannot authorize this request. */
    uint32_t cancel;
    if(!s->ops.read(s->cookie,0x3a8,&cancel))return diagnostic_fault_value(s,A733_NPU_IO,0x3a8,2);
    if(cancel&16)return diagnostic_fault_value(s,A733_NPU_REGISTER,cancel,2);
    pending=atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire);
    if(pending)return diagnostic_fault_value(s,A733_NPU_IRQ,pending,2);
    receipts=atomic_load_explicit(&d->irq_receipts,memory_order_acquire);
    if(receipts!=d->accounted_receipts)return diagnostic_fault_value(s,A733_NPU_IRQ,receipts,2);
    now=s->ops.microseconds(s->cookie);
    if(now-d->trigger_us>5000)return diagnostic_fault_value(s,A733_NPU_TIMEOUT,now-d->trigger_us,2);
    d->proven=1;sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_PROVEN,(uint32_t)d->job_ticket);return 1;
}
static enum npu_service_result diagnostic_recover(struct npu_service *s,
    const struct npu_service_message *m)
{
    struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
    struct a733_npu_controller *c=&s->controller;
    if(!d || s->owner!=m->client)return NPU_SERVICE_DENIED;
    if(c->state==A733_NPU_FAULT)return NPU_SERVICE_FAULT;
    if(!d->proven || !d->canceled || !d->reaped || d->recovered || s->job_ticket ||
       s->has_held || (d->recovery_ticket && d->recovery_ticket!=m->ticket))return NPU_SERVICE_DENIED;
    if(!diagnostic_live(s))return NPU_SERVICE_FAULT;
    d->recovery_ticket=m->ticket;
    if(!d->recovery) {
        if(c->state!=A733_NPU_DRAINED || d->check(d->cookie,0)!=1) {
            diagnostic_fault(s,A733_NPU_MEMORY);return NPU_SERVICE_FAULT;
        }
        if(irq_mask(s)!=1)return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
        if(atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire) || !a733_npu_close(c)) {
            diagnostic_fault(s,A733_NPU_IRQ);return NPU_SERVICE_FAULT;
        }
        d->recovery=1;
        sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_CLOSED,(uint32_t)d->job_ticket);
    }
    if(d->recovery==1) {
        if(c->state!=A733_NPU_CLOSED || c->fault || !s->irq_masked) {
            diagnostic_fault(s,A733_NPU_IRQ);return NPU_SERVICE_FAULT;
        }
        int stopped=irq_call(s,IRQ_STOP_WAIT,s->irq_ops.stop);
        if(stopped<0){diagnostic_fault(s,A733_NPU_IRQ);return NPU_SERVICE_FAULT;}
        if(!stopped)return SERVICE_DEFERRED;
        atomic_store_explicit(&s->signals->irq_stopped,1,memory_order_release);
        sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_IRQ_RETIRED,(uint32_t)d->job_ticket);
        taskENTER_CRITICAL();
        int ready=!atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire) &&
            !s->irq_retained && diagnostic_live(s) && d->irq(s->irq_cookie)==1 && diagnostic_live(s);
        if(ready)atomic_store_explicit(&s->signals->irq_stopped,0,memory_order_release);
        taskEXIT_CRITICAL();
        if(!ready){diagnostic_fault(s,A733_NPU_IRQ);return NPU_SERVICE_FAULT;}
        sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_IRQ_REARM,(uint32_t)d->job_ticket);
        d->recovery=2;
    }
    if(d->recovery!=2 || !diagnostic_begin_irq(s))return NPU_SERVICE_FAULT;
    s->job_ticket=m->ticket;
    c->diagnostic_ticket=m->ticket;
    if(!a733_npu_open(c,&s->ops,s->cookie,&s->image,&s->commands[0],m->request.timeout_us)) {
        diagnostic_fault(s,A733_NPU_MEMORY);return NPU_SERVICE_FAULT;
    }
    d->recovery=3;return NPU_SERVICE_OK;
}
#endif
/* The worker and the runtime loop each call irq_progress and execute_shared
 * once. Forced inlining keeps them inline in the worker, as when the worker
 * was their only caller; the runtime loop, linked only into images that call
 * npu_service_runtime, carries its own copies. */
#define SERVICE_INLINE static inline __attribute__((always_inline))
SERVICE_INLINE int irq_progress(struct npu_service *s)
{
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    struct npu_service_cancel_diagnostic *diagnostic=s->cancel_diagnostic;
    if(diagnostic && s->controller.state!=A733_NPU_FAULT) {
        unsigned received=atomic_load_explicit(&diagnostic->irq_receipts,memory_order_acquire);
        unsigned window=atomic_load_explicit(&diagnostic->irq_window,memory_order_acquire);
        if(!diagnostic_live(s) || (window ? received-diagnostic->receipt_base>1 :
            received!=diagnostic->accounted_receipts))diagnostic_fault(s,A733_NPU_IRQ);
    }
#endif
    a733_npu_check_timeout(&s->controller);
    unsigned pending=atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire);
    if(pending&IRQ_FAILED)irq_fault(s);
    if(pending || s->controller.state==A733_NPU_FAULT) {
        int masked=irq_mask(s);
        if(masked!=1)return masked<0 || s->controller.state==A733_NPU_FAULT ? 1 : 0;
        if(s->controller.state==A733_NPU_FAULT)return 1;
        pending=atomic_exchange_explicit(&s->signals->irq_pending,0,memory_order_acq_rel);
        if(pending&IRQ_FAILED){irq_fault(s);return 1;}
    }
    enum a733_npu_state before=s->controller.state;
    a733_npu_advance(&s->controller,!!(pending&IRQ_EVENT));
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    if(diagnostic && before==A733_NPU_CANCEL_ASSERTED && s->controller.state==A733_NPU_CANCEL_WAIT)
        diagnostic->cancel_released_us=s->ops.microseconds(s->cookie);
    if(diagnostic && s->controller.state==A733_NPU_CANCELED) {
        if(!diagnostic->proven || s->job_ticket!=diagnostic->job_ticket ||
           !diagnostic->cancel_released_us)diagnostic_fault(s,A733_NPU_IRQ);
        else if(!diagnostic->canceled) {
            if(!s->ops.read(s->cookie,0x3a8,&diagnostic->cancel_status))diagnostic_fault(s,A733_NPU_IO);
            diagnostic->cancel_idle=s->controller.last_idle;
            diagnostic->cancel_done_us=s->ops.microseconds(s->cookie);
            if((diagnostic->cancel_status&24)!=16)
                diagnostic_fault_value(s,A733_NPU_REGISTER,diagnostic->cancel_status,3);
            else if(diagnostic->cancel_idle!=UINT32_C(0x7fffffff))
                diagnostic_fault_value(s,A733_NPU_NOT_IDLE,diagnostic->cancel_idle,3);
            else if(diagnostic->cancel_done_us-s->controller.cancel_start>=s->controller.cancel_timeout)
                diagnostic_fault_value(s,A733_NPU_TIMEOUT,diagnostic->cancel_done_us-s->controller.cancel_start,3);
            else {
                diagnostic->canceled=1;
                sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_IDLE,(uint32_t)diagnostic->job_ticket);
            }
        }
    }
#endif
    if(before==A733_NPU_BOOTING && s->controller.state==A733_NPU_READY && s->runtime && s->runtime->recoveries &&
       (s->runtime->marks&NPU_MARK_REINIT))s->runtime->marks|=NPU_MARK_READY;
    if((before==A733_NPU_BOOTING && s->controller.state==A733_NPU_READY) ||
       (before==A733_NPU_RUNNING && s->controller.state==A733_NPU_DONE)) {
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        if(diagnostic) {
            unsigned received=atomic_load_explicit(&diagnostic->irq_receipts,memory_order_acquire);
            if(!atomic_load_explicit(&diagnostic->irq_window,memory_order_acquire) ||
               received-diagnostic->receipt_base!=1)diagnostic_fault(s,A733_NPU_IRQ);
            else {
                diagnostic->accounted_receipts=received;
                atomic_store_explicit(&diagnostic->irq_window,0,memory_order_release);
            }
            if(diagnostic->recovery==3 && before==A733_NPU_BOOTING && s->controller.state==A733_NPU_READY) {
                if(diagnostic->check(diagnostic->cookie,1)!=1)diagnostic_fault(s,A733_NPU_MEMORY);
                else {
                    diagnostic->recovery_receipts=received;diagnostic->recovered=1;diagnostic->recovery=4;
                    sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_BOOTSTRAP,received);
                }
            }
        }
        if(s->controller.state==A733_NPU_FAULT)return 1;
#endif
        unsigned count=atomic_load_explicit(&s->irq_completions,memory_order_relaxed);
        if(count==UINT32_MAX)irq_fault(s);
        else atomic_store_explicit(&s->irq_completions,count+1,memory_order_release);
    }
    if(s->controller.state==A733_NPU_FAULT) {
        (void)irq_mask(s);return 1;
    }
    /* A latched completion keeps the source masked through full-idle/finish.
     * Cancellation also stays masked; it has its own bounded progress path. */
    if((s->controller.state==A733_NPU_BOOTING || s->controller.state==A733_NPU_RUNNING) &&
       !s->controller.irq_seen && (s->irq_masked || s->irq_rearming)) {
        taskENTER_CRITICAL();
        if(!atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire)) {
            int r=irq_call(s, IRQ_REARM_WAIT, s->irq_ops.rearm);
            if(r>=0){s->irq_masked=0;s->irq_rearming=r==0;}
            else {s->irq_retained=1;irq_fault(s);}
        }
        taskEXIT_CRITICAL();
    }
    return 1;
}
static enum npu_service_result execute(struct npu_service *s,
                                        const struct npu_service_message *m,
                                        unsigned *canceled)
{
    const struct npu_service_request *r = &m->request;
    struct a733_npu_controller *c = &s->controller;
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    if(r->operation==NPU_SERVICE_RECOVER) {
        if(r->command || !duration(r->timeout_us) || r->timeout_us>100000)return NPU_SERVICE_INVALID;
        return diagnostic_recover(s,m);
    }
#endif
    if (!valid_request(r, NPU_SERVICE_WAIT)) return NPU_SERVICE_INVALID;
    if (r->operation == NPU_SERVICE_STATUS)
        return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_OK;
    if (r->operation == NPU_SERVICE_OPEN) {
        int reopening=s->reopening && c->state==A733_NPU_CLOSED;
        if(s->irq_bound && ((c->state==A733_NPU_CLOSED && !reopening) || c->state==A733_NPU_FAULT))return NPU_SERVICE_FAULT;
        if(s->irq_bound && irq_mask(s)!=1)
            return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
        if (s->owner != NPU_SERVICE_NONE) return NPU_SERVICE_BUSY;
        if (c->state != A733_NPU_COLD && c->state != A733_NPU_CLOSED) return NPU_SERVICE_BUSY;
        if(reopening) {
            taskENTER_CRITICAL();
            int ready=!atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire) &&
                atomic_load_explicit(&s->activation_state,memory_order_acquire)==SERVICE_ACTIVE &&
                !s->irq_retained && s->restart_ready(s->activation.context)==1;
            s->reopening=0;
            if(ready)atomic_store_explicit(&s->signals->irq_stopped,0,memory_order_release);
            taskEXIT_CRITICAL();
            if(!ready){irq_fault(s);return NPU_SERVICE_FAULT;}
        }
        s->owner = m->client; s->job_ticket = m->ticket;
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        c->diagnostic_ticket=m->ticket;
        if(!diagnostic_begin_irq(s))return NPU_SERVICE_FAULT;
#endif
        if (s->runtime) s->runtime->open_timeout_us = r->timeout_us;
        if (a733_npu_open(c, &s->ops, s->cookie, &s->image, &s->commands[0], r->timeout_us))
            return NPU_SERVICE_OK;
        if (c->state == A733_NPU_FAULT) return NPU_SERVICE_FAULT;
        if(reopening){irq_fault(s);return NPU_SERVICE_FAULT;}
        /* Controller validation refused before borrowing or hardware access. */
        s->owner = NPU_SERVICE_NONE; s->job_ticket = 0;
        return NPU_SERVICE_INVALID;
    }
    if (s->owner != m->client) return NPU_SERVICE_DENIED;
    if (c->state == A733_NPU_FAULT) return NPU_SERVICE_FAULT;
    switch (r->operation) {
    case NPU_SERVICE_WAIT:
        return active(c->state) ? SERVICE_HELD : NPU_SERVICE_OK;
    case NPU_SERVICE_SUBMIT:
        if (!r->command || r->command >= s->command_count) return NPU_SERVICE_INVALID;
        if (!command_allowed(s, m->client, r->command)) return NPU_SERVICE_DENIED;
        if (c->state != A733_NPU_READY) return NPU_SERVICE_BUSY;
        if(s->irq_bound && irq_mask(s)!=1)
            return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        if(s->cancel_diagnostic) {
            struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
            if(r->command==d->command) {
                if(d->started || r->timeout_us>100000)return NPU_SERVICE_DENIED;
            } else if(!d->recovered)return NPU_SERVICE_DENIED;
            if(!diagnostic_live(s))return NPU_SERVICE_FAULT;
            if(r->command!=d->command && !diagnostic_begin_irq(s))return NPU_SERVICE_FAULT;
        }
#endif
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        c->diagnostic_ticket=m->ticket;
#endif
        if (a733_npu_submit(c, &s->commands[r->command], r->timeout_us)) {
            s->job_ticket = m->ticket;
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
            if(s->cancel_diagnostic && r->command==s->cancel_diagnostic->command) {
                struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
                d->job_ticket=m->ticket;d->started=1;d->trigger_us=d->trigger(d->cookie);
                uint64_t elapsed=s->ops.microseconds(s->cookie)-d->trigger_us;
                if(!d->trigger_us || elapsed>2000) {
                    diagnostic_fault_value(s,A733_NPU_TIMEOUT,elapsed,3);return NPU_SERVICE_FAULT;
                }
            }
#endif
            return NPU_SERVICE_OK;
        }
        if (c->state == A733_NPU_FAULT) { s->job_ticket = m->ticket; return NPU_SERVICE_FAULT; }
        return NPU_SERVICE_INVALID;
    case NPU_SERVICE_CANCEL:
        if (c->state != A733_NPU_BOOTING && c->state != A733_NPU_RUNNING) return NPU_SERVICE_BUSY;
        if(s->irq_bound && irq_mask(s)!=1)
            return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        if(s->cancel_diagnostic && r->timeout_us>20000)return NPU_SERVICE_INVALID;
        if(s->cancel_diagnostic && !diagnostic_active(s))return NPU_SERVICE_FAULT;
#endif
        if (a733_npu_cancel(c, r->timeout_us)) {
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
            if(s->cancel_diagnostic) {
                struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
                d->cancel_asserted_us=c->cancel_asserted_us;
                if(c->cancel_asserted_us-d->trigger_us>5000) {
                    diagnostic_fault_value(s,A733_NPU_TIMEOUT,c->cancel_asserted_us-d->trigger_us,3);return NPU_SERVICE_FAULT;
                }
            }
#endif
            return NPU_SERVICE_OK;
        }
        break;
    case NPU_SERVICE_REAP: {
        void *token; int stopped;
        if (!a733_npu_reap(c, &token, &stopped)) return NPU_SERVICE_BUSY;
        (void)token; *canceled = (unsigned)stopped;
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        if(s->cancel_diagnostic && stopped) {
            struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
            if(!d->proven || !d->canceled || s->job_ticket!=d->job_ticket) {
                diagnostic_fault(s,A733_NPU_IRQ);return NPU_SERVICE_FAULT;
            }
            d->reaped=1;
            sunstone_crumb(SUNSTONE_CRUMB_NPU_CANCEL_REAPED,(uint32_t)d->job_ticket);
        }
#endif
        if(s->restart_irq)s->restart_reaped=!stopped;
        return NPU_SERVICE_OK;
    }
    case NPU_SERVICE_CLOSE:
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        if(s->cancel_diagnostic && (!s->cancel_diagnostic->recovered ||
           atomic_load_explicit(&s->irq_completions,memory_order_acquire)!=3))return NPU_SERVICE_DENIED;
#endif
        if (c->state != A733_NPU_READY && c->state != A733_NPU_DRAINED &&
            !(s->irq_bound && c->state==A733_NPU_CLOSED)) return NPU_SERVICE_BUSY;
        if(s->irq_bound && irq_mask(s)!=1)
            return c->state==A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
        if(s->restart_irq && c->state==A733_NPU_DRAINED)s->restart_reaped=0;
        if (c->state==A733_NPU_CLOSED || a733_npu_close(c)) {
            if(s->irq_bound) {
                int r=irq_call(s, IRQ_STOP_WAIT, s->irq_ops.stop);
                if(r<0){s->irq_retained=1;irq_fault(s);return NPU_SERVICE_FAULT;}
                if(!r)return SERVICE_DEFERRED;
                atomic_store_explicit(&s->signals->irq_stopped,1,memory_order_release);
            }
            s->owner = NPU_SERVICE_NONE; s->job_ticket = 0;
            if(s->restart_irq) {
                unsigned expected=SERVICE_ACTIVE;
                if(!atomic_compare_exchange_strong_explicit(&s->activation_state,&expected,SERVICE_PARKED,
                    memory_order_acq_rel,memory_order_acquire)){irq_fault(s);return NPU_SERVICE_FAULT;}
            }
            return NPU_SERVICE_OK;
        }
        break;
    default: return NPU_SERVICE_INVALID;
    }
    return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_INVALID;
}
static void respond(struct npu_service *s, const struct npu_service_message *m,
                    enum npu_service_result result, unsigned canceled)
{
    struct npu_service_reply reply = {.ticket = m->ticket, .result = result, .canceled = canceled};
    reply.state = s->controller.state; reply.fault = s->controller.fault;
    reply.mmu_status = s->controller.mmu_status; reply.mmu_address = s->controller.mmu_address;
    reply.owns_session = s->owner == m->client;
    reply.job_ticket = s->job_ticket;
    if (m->request.operation == NPU_SERVICE_REAP && result == NPU_SERVICE_OK)
        s->job_ticket = 0;
    configASSERT(xQueueSend(s->clients[m->client].replies, &reply, 0) == pdTRUE);
#ifdef SUNSTONE_A733_OS
    xTaskNotifyGive(s->clients[m->client].task);
#endif
}
/* The owner's held WAIT is answered once the controller leaves an active state.
 * Every active state has a controller deadline, so the hold is bounded. */
static void release_held(struct npu_service *s)
{
    if (!s->has_held || active(s->controller.state)) return;
    s->has_held = 0;
    respond(s, &s->held, s->controller.state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_OK, 0);
}
/* Shared mode: member records, round-robin dispatch and per-member WAITs. */
enum { MEMBER_NONE, MEMBER_IDLE, MEMBER_QUEUED, MEMBER_RUNNING, MEMBER_DONE, MEMBER_CANCELED };
static enum a733_npu_state view(const struct npu_service *s, unsigned client)
{
    const struct npu_service_member *m = &s->member[client];
    if (s->controller.state == A733_NPU_FAULT) return A733_NPU_FAULT;
    switch (m->state) {
    case MEMBER_IDLE:
        return s->controller.state == A733_NPU_BOOTING ? A733_NPU_BOOTING : A733_NPU_READY;
    case MEMBER_QUEUED: case MEMBER_RUNNING: return A733_NPU_RUNNING;
    case MEMBER_DONE: return A733_NPU_DONE;
    case MEMBER_CANCELED: return A733_NPU_CANCELED;
    default: return s->controller.state;
    }
}
#ifndef SUNSTONE_A733_OS
static int queued(const struct npu_service *s)
{
    for (unsigned i = 0; i < s->client_count; ++i)
        if (s->member[i].state == MEMBER_QUEUED) return 1;
    return 0;
}
#endif
SERVICE_INLINE enum npu_service_result execute_shared(struct npu_service *s,
    const struct npu_service_message *m, unsigned *canceled, unsigned *closed)
{
    const struct npu_service_request *r = &m->request;
    struct a733_npu_controller *c = &s->controller;
    struct npu_service_member *member = &s->member[m->client];
    if (r->operation == NPU_SERVICE_STATUS)
        return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_OK;
    if (r->operation == NPU_SERVICE_OPEN) {
        if (member->state != MEMBER_NONE) return NPU_SERVICE_BUSY;
        if (c->state == A733_NPU_FAULT || (s->irq_bound && c->state == A733_NPU_CLOSED))
            return NPU_SERVICE_FAULT;
        if (c->state == A733_NPU_COLD || c->state == A733_NPU_CLOSED) {
            if (s->irq_bound && irq_mask(s) != 1)
                return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
            /* The opener owns the bootstrap, including a retained failure. */
            member->state = MEMBER_IDLE; ++s->members;
            if (s->runtime) s->runtime->open_timeout_us = r->timeout_us;
            if (!a733_npu_open(c, &s->ops, s->cookie, &s->image, &s->commands[0], r->timeout_us)) {
                if (c->state == A733_NPU_FAULT) return NPU_SERVICE_FAULT;
                member->state = MEMBER_NONE; --s->members;
                return NPU_SERVICE_INVALID;
            }
            return NPU_SERVICE_OK;
        }
        member->state = MEMBER_IDLE; ++s->members;
        return NPU_SERVICE_OK;
    }
    if (member->state == MEMBER_NONE) return NPU_SERVICE_DENIED;
    if (c->state == A733_NPU_FAULT) return NPU_SERVICE_FAULT;
    switch (r->operation) {
    case NPU_SERVICE_WAIT:
        if (member->refused) { member->refused = 0; return NPU_SERVICE_INVALID; }
        return view(s, m->client) == A733_NPU_BOOTING || view(s, m->client) == A733_NPU_RUNNING ?
            SERVICE_HELD : NPU_SERVICE_OK;
    case NPU_SERVICE_SUBMIT:
        if (!r->command || r->command >= s->command_count) return NPU_SERVICE_INVALID;
        if (!command_allowed(s, m->client, r->command)) return NPU_SERVICE_DENIED;
        if (member->state != MEMBER_IDLE || c->state == A733_NPU_BOOTING) return NPU_SERVICE_BUSY;
        member->state = MEMBER_QUEUED; member->command = r->command;
        member->timeout_us = r->timeout_us; member->job_ticket = m->ticket; member->refused = 0;
        return NPU_SERVICE_OK;
    case NPU_SERVICE_CANCEL:
        /* A started job shares the controller with other members; only its
         * deadline ends it. A queued job never reached the device. */
        if (member->state != MEMBER_QUEUED) return NPU_SERVICE_BUSY;
        member->state = MEMBER_CANCELED;
        return NPU_SERVICE_OK;
    case NPU_SERVICE_REAP:
        if (member->state != MEMBER_DONE && member->state != MEMBER_CANCELED) return NPU_SERVICE_BUSY;
        *canceled = member->state == MEMBER_CANCELED;
        member->state = MEMBER_IDLE;
        return NPU_SERVICE_OK;
    case NPU_SERVICE_CLOSE:
        if (member->state != MEMBER_IDLE) return NPU_SERVICE_BUSY;
        if (s->members > 1) {
            member->state = MEMBER_NONE; --s->members; *closed = 1;
            return NPU_SERVICE_OK;
        }
        if (c->state != A733_NPU_READY && !(s->irq_bound && c->state == A733_NPU_CLOSED))
            return NPU_SERVICE_BUSY;
        if (s->irq_bound && irq_mask(s) != 1)
            return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : SERVICE_DEFERRED;
        if (c->state == A733_NPU_CLOSED || a733_npu_close(c)) {
            if (s->irq_bound) {
                int stopped = irq_call(s, IRQ_STOP_WAIT, s->irq_ops.stop);
                if (stopped < 0) { s->irq_retained = 1; irq_fault(s); return NPU_SERVICE_FAULT; }
                if (!stopped) return SERVICE_DEFERRED;
                atomic_store_explicit(&s->signals->irq_stopped, 1, memory_order_release);
            }
            member->state = MEMBER_NONE; --s->members; *closed = 1;
            return NPU_SERVICE_OK;
        }
        break;
    default: return NPU_SERVICE_INVALID;
    }
    return c->state == A733_NPU_FAULT ? NPU_SERVICE_FAULT : NPU_SERVICE_INVALID;
}
static void respond_shared(struct npu_service *s, const struct npu_service_message *m,
                           enum npu_service_result result, unsigned canceled, unsigned closed)
{
    struct npu_service_member *member = &s->member[m->client];
    struct npu_service_reply reply = {.ticket = m->ticket, .result = result, .canceled = canceled};
    reply.state = closed ? A733_NPU_CLOSED : view(s, m->client);
    reply.fault = s->controller.fault;
    reply.mmu_status = s->controller.mmu_status; reply.mmu_address = s->controller.mmu_address;
    reply.owns_session = member->state != MEMBER_NONE;
    reply.job_ticket = member->job_ticket;
    if (m->request.operation == NPU_SERVICE_REAP && result == NPU_SERVICE_OK)
        member->job_ticket = 0;
    configASSERT(xQueueSend(s->clients[m->client].replies, &reply, 0) == pdTRUE);
#ifdef SUNSTONE_A733_OS
    xTaskNotifyGive(s->clients[m->client].task);
#endif
}
/* Complete the finished job, then start the next queued member after the one
 * served last. Completion already ran the finish callback for that job. */
static void dispatch(struct npu_service *s)
{
    struct a733_npu_controller *c = &s->controller;
    if (c->state == A733_NPU_DONE && s->running != NPU_SERVICE_NONE) {
        void *token; int stopped;
        if (a733_npu_reap(c, &token, &stopped)) {
            s->member[s->running].state = stopped ? MEMBER_CANCELED : MEMBER_DONE;
            s->running = NPU_SERVICE_NONE;
        }
    }
    if (c->state != A733_NPU_READY || s->running != NPU_SERVICE_NONE) return;
    for (unsigned k = 0; k < s->client_count; ++k) {
        unsigned i = (s->next + k) % s->client_count;
        struct npu_service_member *member = &s->member[i];
        if (member->state != MEMBER_QUEUED) continue;
        if (s->irq_bound && irq_mask(s) != 1) return; /* Retried on the next pass. */
        s->next = (i + 1) % s->client_count;
        if (a733_npu_submit(c, &s->commands[member->command], member->timeout_us) ||
            c->state == A733_NPU_FAULT) {
            /* A faulted start stays attributed to its member and is retained. */
            member->state = MEMBER_RUNNING; s->running = i;
            return;
        }
        /* Refused before any device access: report it on the member's WAIT. */
        member->state = MEMBER_IDLE; member->job_ticket = 0; member->refused = 1;
    }
}
static void release_waits(struct npu_service *s)
{
    for (unsigned i = 0; i < s->client_count; ++i) {
        struct npu_service_member *member = &s->member[i];
        enum a733_npu_state state = view(s, i);
        if (!member->has_wait || state == A733_NPU_BOOTING || state == A733_NPU_RUNNING) continue;
        member->has_wait = 0;
        enum npu_service_result result = state == A733_NPU_FAULT ? NPU_SERVICE_FAULT :
            member->refused ? NPU_SERVICE_INVALID : NPU_SERVICE_OK;
        member->refused = 0;
        respond_shared(s, &member->wait, result, 0, 0);
    }
}
/* Runtime mode: members, held LOAD and SUBMIT replies and runtime steps. */
static void runtime_reply(struct npu_service *s, unsigned client, enum npu_service_result result,
                          enum a733_npu_state state)
{
    struct npu_service_member *member = &s->member[client];
    const int job_fault = result == NPU_SERVICE_JOB_FAULT && s->runtime;
    struct npu_service_reply reply = {.ticket = member->wait.ticket, .result = result,
        .state = state, .fault = job_fault ? A733_NPU_MMU : s->controller.fault, .owns_session = 1,
        .mmu_status = job_fault ? s->runtime->fault_status : s->controller.mmu_status,
        .mmu_address = job_fault ? s->runtime->fault_address : s->controller.mmu_address,
        .job_ticket = member->wait.request.operation == NPU_SERVICE_SUBMIT ? member->wait.ticket : 0};
    member->has_wait = 0; member->state = MEMBER_IDLE; member->job_ticket = 0;
    configASSERT(xQueueSend(s->clients[client].replies, &reply, 0) == pdTRUE);
#ifdef SUNSTONE_A733_OS
    xTaskNotifyGive(s->clients[client].task);
#endif
    if (s->runtime) s->runtime->marks |= NPU_MARK_REPLY_POSTED;
}
static enum npu_service_result execute_runtime(struct npu_service *s,
    const struct npu_service_message *m, unsigned *closed)
{
    const struct npu_service_request *r = &m->request;
    struct npu_service_member *member = &s->member[m->client];
    struct npu_service_runtime_state *rt = s->runtime;
    switch (r->operation) {
    case NPU_SERVICE_STATUS: case NPU_SERVICE_OPEN: case NPU_SERVICE_CLOSE: {
        unsigned canceled = 0;
        enum npu_service_result result = execute_shared(s, m, &canceled, closed);
        if (r->operation == NPU_SERVICE_CLOSE && result == NPU_SERVICE_OK)
            rt->hooks.closed(rt->context, m->client);
        return result;
    }
    case NPU_SERVICE_LOAD: case NPU_SERVICE_SUBMIT: break;
    default: return NPU_SERVICE_INVALID;
    }
    if (member->state == MEMBER_NONE) return NPU_SERVICE_DENIED;
    if (s->controller.state == A733_NPU_FAULT || rt->failed) return NPU_SERVICE_FAULT;
    if (member->state != MEMBER_IDLE) return NPU_SERVICE_BUSY;
    if (r->operation == NPU_SERVICE_SUBMIT &&
        (!r->command || r->command >= s->command_count ||
         !command_allowed(s, m->client, r->command))) return NPU_SERVICE_DENIED;
    enum npu_service_result result = NPU_SERVICE_INVALID;
    if (!rt->hooks.request(rt->context, m->client, r, &result)) return result;
    member->state = MEMBER_QUEUED; member->wait = *m; member->has_wait = 1;
    member->job_ticket = r->operation == NPU_SERVICE_SUBMIT ? m->ticket : 0;
    return SERVICE_HELD;
}
/* Hand a completed job back to the runtime; on a controller fault release
 * every held reply. */
int npu_service_runtime_parked(const struct npu_service *s)
{return s&&s->runtime&&atomic_load_explicit(&s->signals->runtime_parked,memory_order_acquire);}
int npu_service_observe(struct npu_service *s,
    void (*observe)(void *,const struct npu_service *,unsigned),void *cookie)
{
    /* Set once, before the scheduler starts: the sites below branch on a pointer that no later code writes. */
    if(!s||!s->initialized||!s->runtime||!observe||s->observe||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    s->observe=observe;s->observe_cookie=cookie;return 1;
}
static void runtime_settle(struct npu_service *s)
{
    struct a733_npu_controller *c = &s->controller;
    struct npu_service_runtime_state *rt = s->runtime;
    if (c->state == A733_NPU_DONE && s->running != NPU_SERVICE_NONE) {
        void *token; int stopped;
        unsigned client = s->running;
        if(__builtin_expect(s->observe!=NULL,0))s->observe(s->observe_cookie,s,2);
        if (a733_npu_reap(c, &token, &stopped) && !stopped) {
            s->running = NPU_SERVICE_NONE; s->member[client].state = MEMBER_QUEUED;
            rt->hooks.finished(rt->context, client, 0);
        }
    }
    if (c->state == A733_NPU_FAULT && c->fault == A733_NPU_MMU && !rt->failed && !c->bootstrap &&
        s->running != NPU_SERVICE_NONE && s->member[s->running].has_wait) {
        /* An NPU MMU exception during a job: fail that job only, then recover as the vendor driver does
         * (vip2/task/vip_drv_task_common.c:463-535). The slot is released first, so that its read-only region is
         * checked like that of a finished job; the reply carries the status and the address; then the NPU is
         * cancelled and re-initialized and the session goes on. */
        const unsigned client = s->running;
        rt->marks |= NPU_MARK_CAPTURED | NPU_MARK_SETTLE;
        rt->hooks.finished(rt->context, client, 2);
        rt->marks |= NPU_MARK_FINISHED;
        s->running = NPU_SERVICE_NONE; s->member[client].state = MEMBER_QUEUED;
        /* The SUBMIT reply stays held, as for a finished job, until the runtime has checked the slot's read-only
         * region; it carries what the controller captured, which the recovery below clears. */
        rt->fault_status = c->mmu_status; rt->fault_address = c->mmu_address;
        int cancel_seen = 0;
        if (a733_npu_recover(c, &s->commands[0], rt->open_timeout_us, &cancel_seen)) {
            rt->marks |= NPU_MARK_REINIT | (cancel_seen ? NPU_MARK_CANCEL_DONE : 0u);
            ++rt->recoveries;
        } else {
            rt->marks |= NPU_MARK_RECOVER_FAILED;
            rt->failed = 1;
        }
        return;
    }
    if (c->state != A733_NPU_FAULT || rt->failed) return;
    rt->failed = 1;
    rt->hooks.finished(rt->context, s->running, 1);
    for (unsigned i = 0; i < s->client_count; ++i)
        if (s->member[i].has_wait) runtime_reply(s, i, NPU_SERVICE_FAULT, A733_NPU_FAULT);
}
static int controller_active(const struct npu_service *s)
{ return s->controller.state == A733_NPU_BOOTING || s->controller.state == A733_NPU_RUNNING; }
/* Read the idle register, which acknowledges nothing. Full idle is only a
 * hint: it queues the masked progress sequence that a notification runs. */
static int idle_hint(struct npu_service *s)
{
    uint32_t idle;
    if (!s->runtime->hooks.idle_hint || !controller_active(s) ||
        atomic_load_explicit(&s->signals->irq_pending, memory_order_acquire)) return 0;
    if (!s->ops.read(s->cookie, 4, &idle)) {
        s->controller.state = A733_NPU_FAULT; s->controller.fault = A733_NPU_IO;
        return 1;
    }
    if ((idle & UINT32_C(0x7fffffff)) != UINT32_C(0x7fffffff)) return 0;
    ++s->runtime->idle_hints;
    atomic_fetch_or_explicit(&s->signals->irq_pending, IRQ_EVENT, memory_order_release);
    return 1;
}
/* Interrupt progress reads the configuration registers while a job runs, so
 * a pass runs it only when it can change something: a notification or hint
 * is pending, the source is masked or rearming (after a start), the wait
 * timed out, the deadline expired or no job runs. Other passes read only
 * the timer. */
static int runtime_progress(struct npu_service *s, int timed_out)
{
    if (!timed_out && controller_active(s) && !s->irq_masked && !s->irq_rearming &&
        !atomic_load_explicit(&s->signals->irq_pending, memory_order_acquire) &&
        a733_npu_check_timeout(&s->controller) != A733_NPU_FAULT) return 1;
    return irq_progress(s);
}
static void runtime_worker(struct npu_service *s)
{
    for (int timed_out = 0;;) {
        if (s->direct_request) {
            /* This is the only direct handoff point. After acknowledgment
             * this iteration can only park; a claimant may preempt us after
             * its notification without resuming any worker MMIO operation. */
            int ready = s->controller.state == A733_NPU_READY &&
                s->running == NPU_SERVICE_NONE && !s->has_deferred &&
                !s->has_held && !s->runtime->failed && !s->irq_retained &&
                !atomic_load_explicit(&s->signals->irq_pending, memory_order_acquire) &&
                !uxQueueMessagesWaiting(s->requests);
            if (ready) s->direct = s->direct_request;
            s->direct_request = NULL;
            TaskHandle_t owner = s->direct_owner;
            atomic_store_explicit(&s->direct_ready, ready ? 1u : 2u, memory_order_release);
            xTaskNotifyGive(owner);
        }
        if (s->direct) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (atomic_load_explicit(&s->activation_state, memory_order_acquire) != SERVICE_ACTIVE) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if(s->controller.state==A733_NPU_CLOSED&&!s->members&&
           atomic_load_explicit(&s->signals->irq_stopped,memory_order_acquire)){
            /* Runtime slot CLOSE is terminal. Publish after the final reply,
             * then invoke no backend or runtime callback again. Unexpected
             * notifications cannot resume device ownership. */
            atomic_store_explicit(&s->signals->runtime_parked,1,memory_order_release);
            if(s->activation.bootstrap)xTaskNotifyGive(s->activation.bootstrap);
            for(;;)(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        }
        if (!controller_active(s) && s->controller.state != A733_NPU_FAULT)
            (void)s->ops.microseconds(s->cookie);
        if(__builtin_expect(s->observe!=NULL,0)&&s->controller.state==A733_NPU_RUNNING)s->observe(s->observe_cookie,s,0);
        int progressed = runtime_progress(s, timed_out);
        if (s->controller.state == A733_NPU_FAULT) s->runtime->marks |= NPU_MARK_PROGRESS_DONE;
        runtime_settle(s);
        if (timed_out && idle_hint(s)) { timed_out = 0; continue; }
        timed_out = 0;
        struct npu_service_message message;
        int handled = s->has_deferred;
        if (handled) message = s->deferred;
        else handled = xQueueReceive(s->requests, &message, 0) == pdTRUE;
        if (handled) {
            configASSERT(message.client < s->client_count && message.ticket);
            unsigned closed = 0;
            enum npu_service_result result = valid_request(&message.request, NPU_SERVICE_LOAD) ?
                execute_runtime(s, &message, &closed) : NPU_SERVICE_INVALID;
            s->has_deferred = (unsigned)result == SERVICE_DEFERRED;
            if (s->has_deferred) s->deferred = message;
            else if ((unsigned)result != SERVICE_HELD) respond_shared(s, &message, result, 0, closed);
            runtime_settle(s);
        }
        int stepped = s->controller.state != A733_NPU_FAULT && !s->runtime->failed &&
            s->runtime->hooks.step(s->runtime->context);
        runtime_settle(s);
        if (stepped) { (void)idle_hint(s); continue; }
        if (handled && !s->has_deferred) continue;
#ifdef SUNSTONE_A733_OS
        (void)progressed;
        timed_out=service_wait(s);
#else
        TickType_t wait = controller_active(s) || s->has_deferred || !progressed ? 1 :
            s->controller.state == A733_NPU_FAULT ? portMAX_DELAY : pdMS_TO_TICKS(1000);
        if (!s->irq_masked && !s->irq_retained &&
            (s->controller.state == A733_NPU_FAULT || atomic_load_explicit(&s->signals->irq_pending, memory_order_acquire)))
            wait = 1;
        timed_out = !ulTaskNotifyTake(pdTRUE, wait);
#endif
    }
}

static void worker(void *argument)
{
    struct npu_service *s = argument;
    uint64_t level, stack;
    level = npu_arch_current_el(); stack = npu_arch_stack_select();
    configASSERT(level == 4);
#ifdef SUNSTONE_A733_OS
    configASSERT(sunstone_deadline_register());
#endif
#if configNUMBER_OF_CORES > 1
    configASSERT(stack == 0); /* Preemptible EL1 task uses SP_EL0, not an exception stack. */
#else
    (void)stack;
#endif
    if (s->runtime) s->runtime->loop(s);
    for (;;) {
        if(s->restart_irq && atomic_load_explicit(&s->activation_state,memory_order_acquire)==SERVICE_RESUMING) {
            taskENTER_CRITICAL();
            unsigned expected=SERVICE_RESUMING;
            int ready=!atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire) &&
                !s->irq_retained && s->restart_ready(s->activation.context)==1 &&
                s->restart_irq(s->irq_cookie)==1 && s->restart_ready(s->activation.context)==1;
            if(ready) {
                s->restart_reaped=0;s->reopening=1;
                ready=atomic_compare_exchange_strong_explicit(&s->activation_state,&expected,SERVICE_ACTIVE,
                    memory_order_acq_rel,memory_order_acquire);
            }
            if(!ready) {
                atomic_store_explicit(&s->activation_state,SERVICE_RETAINED,memory_order_release);
                irq_fault(s);
            }
            taskEXIT_CRITICAL();
        }
        if (atomic_load_explicit(&s->activation_state,memory_order_acquire)!=SERVICE_ACTIVE) {
            (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
            continue;
        }
        /* Keep the native counter extension current even during long idle
         * sessions. A faulted backend must not receive any further callbacks. */
        if (!active(s->controller.state) && s->controller.state != A733_NPU_FAULT)
            (void)s->ops.microseconds(s->cookie);
        /* At most one request between polls: queued STATUS traffic cannot
         * starve device completion or controller deadlines. */
        int progressed=1;
        if(s->irq_bound)progressed=irq_progress(s);
        else a733_npu_poll(&s->controller);
        if (s->shared) { dispatch(s); release_waits(s); }
        else release_held(s);
        struct npu_service_message message;
#ifndef SUNSTONE_A733_OS
        TickType_t wait = active(s->controller.state) ? 1 :
            (s->controller.state == A733_NPU_FAULT ? portMAX_DELAY : pdMS_TO_TICKS(1000));
        if (s->shared && s->controller.state == A733_NPU_READY && queued(s)) wait = 1;
#endif
        if(s->irq_bound) {
#ifdef SUNSTONE_A733_OS
            if(!progressed){(void)service_wait(s);continue;}
            if(s->has_deferred)message=s->deferred;
            else if(xQueueReceive(s->requests,&message,0)!=pdTRUE) {
                (void)service_wait(s);continue;
            }
        }else if(xQueueReceive(s->requests,&message,0)!=pdTRUE){
            (void)service_wait(s);continue;
        }
#else
            if(!s->irq_masked && !s->irq_retained &&
               (s->controller.state==A733_NPU_FAULT || atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire)))wait=1;
            if(!progressed){(void)ulTaskNotifyTake(pdTRUE,1);continue;}
            if(s->has_deferred)message=s->deferred;
            else if(xQueueReceive(s->requests,&message,0)!=pdTRUE) {
                (void)ulTaskNotifyTake(pdTRUE,wait);continue;
            }
        }else if (xQueueReceive(s->requests, &message, wait) != pdTRUE) continue;
#endif
        configASSERT(message.client < s->client_count && message.ticket);
        unsigned canceled = 0, closed = 0;
        enum npu_service_result result = !s->shared ? execute(s, &message, &canceled) :
            valid_request(&message.request, NPU_SERVICE_WAIT) ? execute_shared(s, &message, &canceled, &closed) :
            NPU_SERVICE_INVALID;
        if((unsigned)result==SERVICE_DEFERRED) {
            s->deferred=message;s->has_deferred=1;
#ifdef SUNSTONE_A733_OS
            (void)service_wait(s);continue;
#else
            (void)ulTaskNotifyTake(pdTRUE,1);continue;
#endif
        }
        s->has_deferred=0;
        if((unsigned)result==SERVICE_HELD) {
            /* One pending request per client: the waiter has no other request. */
            if (s->shared) {
                struct npu_service_member *member = &s->member[message.client];
                configASSERT(!member->has_wait);
                member->wait = message; member->has_wait = 1;
                continue;
            }
            configASSERT(!s->has_held);
            s->held=message;s->has_held=1;continue;
        }
        if (s->shared) {
            respond_shared(s, &message, result, canceled, closed);
            dispatch(s); release_waits(s);
            continue;
        }
        respond(s, &message, result, canceled);
        release_held(s);
    }
}

static int initialize(struct npu_service *s, const struct a733_npu_ops *ops, void *cookie,
                      const struct a733_npu_mmu_image *image, const struct a733_npu_command *commands,
                      unsigned count, UBaseType_t priority, UBaseType_t affinity,
                      const struct npu_service_activation *activation)
{
    struct npu_service_activation saved_activation={0};
    if(activation) {
        memcpy(&saved_activation,activation,sizeof(saved_activation));
        if(!saved_activation.bootstrap || !saved_activation.prepared)return 0;
        activation=&saved_activation;
    }
    if (!s || instance || s->initialized || !ops || !image || !commands ||
        !s->client_count || !count || count > s->table_commands || !priority || priority >= configMAX_PRIORITIES ||
        !affinity || (affinity >> configNUMBER_OF_CORES) ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED ||
        !ops->read || !ops->write || !ops->microseconds || !ops->delay ||
        !ops->barrier || !ops->publish || !ops->finish) return 0;
    const struct a733_npu_ops saved_ops=*ops;
    const struct a733_npu_mmu_image saved_image=*image;
    /* The commands are copied once into the service's own table and checked there, so what is checked is what is kept. */
    if (!a733_span_incl_nonempty(commands, count * sizeof(*commands)) ||
        a733_extents_intersect((uintptr_t)commands, count * sizeof(*commands), (uintptr_t)s->tables, s->table_bytes)) return 0;
    ops=&saved_ops;image=&saved_image;
    for (unsigned i = 0; i < count; ++i) s->commands[i] = commands[i];
    for (unsigned i = 0; i < count; ++i) {
        const struct a733_npu_command *c = &s->commands[i];
        if (!c->device || (c->device & 63) || !c->token || c->readable_bytes < A733_NPU_MIN_COMMAND_BYTES || (c->readable_bytes & 7) ||
            c->readable_bytes > (UINT64_C(1) << 32) - c->device) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (s->commands[j].token == c->token || s->commands[j].device == c->device) return 0;
    }
    s->ops = *ops; s->cookie = cookie; s->image = *image; s->command_count = count;
    s->owner = NPU_SERVICE_NONE;
    s->requests = xQueueCreateStatic(s->client_count, sizeof(struct npu_service_message),
                                     s->request_storage, &s->request_state);
    configASSERT(s->requests);
    for (unsigned i = 0; i < s->client_count; ++i) {
        struct npu_service_client *c = &s->clients[i];
        c->replies = xQueueCreateStatic(1, sizeof(struct npu_service_reply),
                                        c->reply_storage, &c->reply_state);
        configASSERT(c->replies);
    }
    s->activation=activation ? *activation : (struct npu_service_activation){0};
    atomic_init(&s->activation_state,activation ? SERVICE_WAITING : SERVICE_ACTIVE);
    if(!s->signals)s->signals=&s->own_signals;
    atomic_init(&s->signals->irq_pending,0);atomic_init(&s->signals->irq_stopped,0);atomic_init(&s->irq_completions,0);
    atomic_init(&s->signals->runtime_parked,0);
    s->initialized = 1; s->affinity = affinity; instance = s; creating_worker = 1;
#if configNUMBER_OF_CORES > 1
    s->worker = xTaskCreateStaticAffinitySet(worker, "npu-service", NPU_SERVICE_STACK_WORDS,
        s, priority, s->worker_stack, &s->worker_state, affinity);
#else
    s->worker = xTaskCreateStatic(worker, "npu-service", NPU_SERVICE_STACK_WORDS,
        s, priority, s->worker_stack, &s->worker_state);
#endif
    creating_worker = 0;
    configASSERT(s->worker);
    return 1;
}
int npu_service_init(struct npu_service *s,const struct a733_npu_ops *ops,void *cookie,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,
    unsigned count,UBaseType_t priority,UBaseType_t affinity)
{return initialize(s,ops,cookie,image,commands,count,priority,affinity,NULL);}
int npu_service_init_deferred(struct npu_service *s,const struct a733_npu_ops *ops,void *cookie,
    const struct a733_npu_mmu_image *image,const struct a733_npu_command *commands,
    unsigned count,UBaseType_t priority,UBaseType_t affinity,const struct npu_service_activation *activation)
{
    if(!activation)return 0;
    return initialize(s,ops,cookie,image,commands,count,priority,affinity,activation);
}
enum npu_service_result npu_service_activate(struct npu_service *s)
{
    if(!s || s!=instance || !s->initialized || !s->activation.bootstrap ||
        xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING ||
        xTaskGetCurrentTaskHandle()!=s->activation.bootstrap)return NPU_SERVICE_DENIED;
    uint64_t level=npu_arch_current_el();
    if(level!=4)return NPU_SERVICE_DENIED;
    unsigned expected=SERVICE_WAITING;
    if(!atomic_compare_exchange_strong_explicit(&s->activation_state,&expected,SERVICE_CHECKING,
        memory_order_acq_rel,memory_order_acquire))
        return expected==SERVICE_RETAINED ? NPU_SERVICE_FAULT : NPU_SERVICE_BUSY;
    int ready=s->activation.prepared(s->activation.context,&s->ops,s->cookie,&s->image,s->commands,s->command_count);
    unsigned state=ready==1 ? SERVICE_ACTIVE : ready==0 ? SERVICE_WAITING : SERVICE_RETAINED;
    expected=SERVICE_CHECKING;
    if(!atomic_compare_exchange_strong_explicit(&s->activation_state,&expected,state,
        memory_order_acq_rel,memory_order_acquire))return NPU_SERVICE_FAULT;
    if(state==SERVICE_ACTIVE){xTaskNotifyGive(s->worker);return NPU_SERVICE_OK;}
    return state==SERVICE_WAITING ? NPU_SERVICE_BUSY : NPU_SERVICE_FAULT;
}
int npu_service_restart_enable(struct npu_service *s,int (*irq)(void *),int (*ready)(void *))
{
    if(!s || s!=instance || !s->initialized || !irq || !ready || s->restart_irq ||
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
       s->cancel_diagnostic ||
#endif
       !s->irq_bound || s->shared || s->runtime || !s->activation.bootstrap || !s->clients[0].task ||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    for(unsigned i=1;i<s->client_count;++i)if(s->clients[i].task)return 0;
    s->restart_irq=irq;s->restart_ready=ready;return 1;
}
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
int npu_service_cancel_diagnostic_enable(struct npu_service *s,
    struct npu_service_cancel_diagnostic *d,unsigned command,
    uint64_t (*trigger)(void *),int (*check)(void *,unsigned),void *cookie,int (*irq)(void *))
{
    if(!s || s!=instance || !s->initialized || !d || !trigger || !check || !irq || !cookie ||
       s->cancel_diagnostic || s->restart_irq || !s->irq_bound || s->shared || s->runtime ||
       !s->activation.bootstrap || !s->clients[0].task || command!=2 || s->command_count!=3 ||
       s->commands[command].device!=UINT32_C(0xe010a000) || s->commands[command].readable_bytes!=524288 ||
       !command_allowed(s,0,command) || d->command || d->started || d->reaped || d->recovered ||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    for(unsigned i=1;i<s->client_count;++i)if(s->clients[i].task)return 0;
    d->command=command;d->trigger=trigger;d->check=check;d->cookie=cookie;d->irq=irq;
    atomic_init(&d->irq_receipts,0);atomic_init(&d->irq_window,0);
    s->cancel_diagnostic=d;return 1;
}
#endif
static int restart_caller(struct npu_service *s)
{
    uint64_t level=npu_arch_current_el();
    return s && s==instance && s->restart_irq && !s->shared && !s->runtime && level==4 &&
        xTaskGetSchedulerState()==taskSCHEDULER_RUNNING &&
        xTaskGetCurrentTaskHandle()==s->activation.bootstrap;
}
void npu_service_restart_retain(struct npu_service *s)
{
    if(!restart_caller(s))return;
    atomic_store_explicit(&s->activation_state,SERVICE_RETAINED,memory_order_release);
    xTaskNotifyGive(s->worker);
}
enum npu_service_result npu_service_restart_prepare(struct npu_service *s)
{
    if(!restart_caller(s))return NPU_SERVICE_DENIED;
    unsigned state=atomic_load_explicit(&s->activation_state,memory_order_acquire);
    if(state==SERVICE_RETAINED)return NPU_SERVICE_FAULT;
    if(state!=SERVICE_PARKED)return NPU_SERVICE_BUSY;
    if(!s->restart_reaped || s->controller.state!=A733_NPU_CLOSED || s->controller.fault ||
       s->owner!=NPU_SERVICE_NONE || s->job_ticket || s->has_deferred || s->has_held ||
       s->clients[0].pending || uxQueueMessagesWaiting(s->requests) ||
       uxQueueMessagesWaiting(s->clients[0].replies) || !npu_service_irq_stopped(s))return NPU_SERVICE_INVALID;
    return atomic_compare_exchange_strong_explicit(&s->activation_state,&state,SERVICE_PREPARING,
        memory_order_acq_rel,memory_order_acquire) ? NPU_SERVICE_OK : NPU_SERVICE_FAULT;
}
enum npu_service_result npu_service_restart_resume(struct npu_service *s)
{
    if(!restart_caller(s))return NPU_SERVICE_DENIED;
    unsigned state=atomic_load_explicit(&s->activation_state,memory_order_acquire);
    if(state==SERVICE_ACTIVE && s->reopening)return NPU_SERVICE_OK;
    if(state==SERVICE_RETAINED)return NPU_SERVICE_FAULT;
    if(state==SERVICE_RESUMING)return NPU_SERVICE_BUSY;
    if(state!=SERVICE_PREPARING)return NPU_SERVICE_INVALID;
    int ready=s->activation.prepared(s->activation.context,&s->ops,s->cookie,&s->image,s->commands,s->command_count);
    if(ready<0){npu_service_restart_retain(s);return NPU_SERVICE_FAULT;}
    if(!ready)return NPU_SERVICE_BUSY;
    if(!atomic_compare_exchange_strong_explicit(&s->activation_state,&state,SERVICE_RESUMING,
        memory_order_acq_rel,memory_order_acquire))return NPU_SERVICE_FAULT;
    xTaskNotifyGive(s->worker);return NPU_SERVICE_BUSY;
}
int npu_service_share(struct npu_service *s)
{
    if (!s || s != instance || !s->initialized || s->shared || s->restart_irq ||
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        s->cancel_diagnostic ||
#endif
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    /* The controller refuses a command touching internal SRAM before any
     * device access; rejecting it here keeps dispatch refusals impossible. */
    for (unsigned i = 0; i < s->command_count; ++i)
        if ((uint64_t)s->commands[i].device + s->commands[i].readable_bytes > 0x400000 &&
            s->commands[i].device < 0x480000) return 0;
    s->running = NPU_SERVICE_NONE; s->shared = 1;
    return 1;
}
int npu_service_runtime(struct npu_service *s, const struct npu_service_runtime *r, void *context)
{
    static struct npu_service_runtime_state state;
    if (!s || s != instance || !s->initialized || !s->shared || s->runtime || !r ||
        !r->request || !r->step || !r->finished || !r->closed || r->idle_hint > 1 ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    state.hooks = *r; state.context = context; state.loop = runtime_worker;
    s->runtime = &state;
    return 1;
}
int npu_service_start(struct npu_service *s, unsigned client, unsigned command, uint64_t timeout)
{
    struct a733_npu_controller *c = &s->controller;
    if (!s->runtime || client >= s->client_count || !s->irq_bound ||
        s->member[client].state != MEMBER_QUEUED || !s->member[client].has_wait ||
        s->member[client].wait.request.operation != NPU_SERVICE_SUBMIT ||
        c->state != A733_NPU_READY || s->running != NPU_SERVICE_NONE ||
        !command || command >= s->command_count ||
        !command_allowed(s, client, command)) return -1;
    int masked = irq_mask(s);
    if (masked != 1) return masked < 0 || c->state == A733_NPU_FAULT ? -1 : 0;
    if (!a733_npu_submit(c, &s->commands[command], timeout) && c->state != A733_NPU_FAULT) return -1;
    /* A faulted start stays attributed to its member and is retained. */
    s->member[client].state = MEMBER_RUNNING; s->running = client;
    if(__builtin_expect(s->observe!=NULL,0)&&c->state==A733_NPU_RUNNING)s->observe(s->observe_cookie,s,1);
    return c->state == A733_NPU_FAULT ? -1 : 1;
}
void npu_service_release(struct npu_service *s, unsigned client, enum npu_service_result result,
                         enum a733_npu_state state)
{
    configASSERT(s->runtime && client < s->client_count && s->member[client].has_wait &&
                 s->member[client].state == MEMBER_QUEUED);
    runtime_reply(s, client, result, state);
}
void npu_service_retain(struct npu_service *s)
{
    configASSERT(s->runtime);
    if (s->controller.state == A733_NPU_FAULT) return;
    s->controller.state = A733_NPU_FAULT; s->controller.fault = A733_NPU_MEMORY;
}
static int bind_common(struct npu_service *s, unsigned slot, TaskHandle_t task)
{
    if (!s || s != instance || !s->initialized || s->restart_irq ||
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
        s->cancel_diagnostic ||
#endif
        slot >= s->client_count || !task ||
        task == s->worker || s->clients[slot].task ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    for (unsigned i = 0; i < s->client_count; ++i)
        if (s->clients[i].task == task) return 0;
    return 1;
}
static void clear_permissions(struct npu_service *s, unsigned slot)
{
    memset(s->clients[slot].commands, 0, NPU_SERVICE_PERMIT_WORDS(s->table_commands) * sizeof(uint32_t));
}
int npu_service_bind(struct npu_service *s, unsigned slot, TaskHandle_t task, uint32_t commands)
{
    if (!bind_common(s, slot, task) || (commands & 1u) || (s->command_count < 32 && (commands >> s->command_count))) return 0;
    s->clients[slot].task = task;
    clear_permissions(s, slot);
    s->clients[slot].commands[0] = commands;
    return 1;
}
int npu_service_bind_range(struct npu_service *s, unsigned slot, TaskHandle_t task, unsigned first, unsigned count)
{
    if (!bind_common(s, slot, task) || !first || !count || first > s->command_count || count > s->command_count - first) return 0;
    s->clients[slot].task = task;
    clear_permissions(s, slot);
    for (unsigned c = first; c < first + count; ++c) s->clients[slot].commands[c / 32u] |= UINT32_C(1) << (c % 32u);
    return 1;
}
size_t npu_service_tables_bytes(unsigned clients, unsigned commands)
{
    if (!clients || !commands) return 0;
    return NPU_SERVICE_TABLES_BYTES(clients, commands);
}
int npu_service_tables(struct npu_service *s, unsigned clients, unsigned commands, void *memory, size_t bytes)
{
    const size_t need = npu_service_tables_bytes(clients, commands);
    if (!s || s->initialized || s->tables || !need || bytes < need || !memory || ((uintptr_t)memory & 7u) ||
        !a733_span_incl_nonempty(memory, need) || a733_extents_intersect((uintptr_t)memory, need, (uintptr_t)s, sizeof(*s)))
        return 0;
    memset(memory, 0, need);
    uint8_t *at = memory;
#define TAKE(pointer, type, count_, each) do { (pointer) = (type *)at; at += (size_t)(count_) * (each); } while (0)
    TAKE(s->commands, struct a733_npu_command, 1, ((size_t)commands * sizeof(struct a733_npu_command) + 7u) / 8u * 8u);
    TAKE(s->clients, struct npu_service_client, clients, (sizeof(struct npu_service_client) + 7u) / 8u * 8u);
    TAKE(s->member, struct npu_service_member, clients, (sizeof(struct npu_service_member) + 7u) / 8u * 8u);
    const size_t permit = (NPU_SERVICE_PERMIT_WORDS(commands) * 4u + 7u) / 8u * 8u;
    for (unsigned i = 0; i < clients; ++i) s->clients[i].commands = (uint32_t *)(at + (size_t)i * permit);
    at += (size_t)clients * permit;
    TAKE(s->request_storage, uint8_t, clients, (sizeof(struct npu_service_message) + 7u) / 8u * 8u);
#undef TAKE
    s->client_count = clients; s->table_commands = commands; s->tables = memory; s->table_bytes = need;
    return 1;
}
int npu_service_bind_irq(struct npu_service *s,const struct npu_service_irq_ops *ops,void *cookie,
    UBaseType_t route)
{
    if(!s || s!=instance || !s->initialized || s->irq_bound || !ops ||
       !ops->mask || !ops->rearm || !ops->stop || !cookie ||
       !route || (route&(route-1)) || route!=s->affinity ||
       xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    s->irq_ops=*ops;s->irq_cookie=cookie;s->irq_bound=1;return 1;
}
void npu_service_irq_notify_from_isr(struct npu_service *s,int mask_ok,BaseType_t *woken)
{
    configASSERT(s && s==instance && s->initialized && s->irq_bound && woken);
#ifdef SUNSTONE_NPU_ACTIVE_CANCEL
    if(s->cancel_diagnostic) {
        struct npu_service_cancel_diagnostic *d=s->cancel_diagnostic;
        unsigned previous=atomic_fetch_add_explicit(&d->irq_receipts,1,memory_order_acq_rel);
        if(previous==UINT32_MAX || !atomic_load_explicit(&d->irq_window,memory_order_acquire))mask_ok=0;
    }
#endif
    unsigned stopped=atomic_exchange_explicit(&s->signals->irq_stopped,0,memory_order_acq_rel);
    if(s->restart_irq && (stopped ||
       atomic_load_explicit(&s->activation_state,memory_order_acquire)!=SERVICE_ACTIVE)) {
        atomic_store_explicit(&s->activation_state,SERVICE_RETAINED,memory_order_release);
        mask_ok=0;
    }
    atomic_fetch_or_explicit(&s->signals->irq_pending,IRQ_EVENT|(mask_ok==1&&!stopped?0:IRQ_FAILED),memory_order_release);
    vTaskNotifyGiveFromISR(s->worker,woken);
}
int npu_service_irq_stopped(const struct npu_service *s)
{return s && s==instance && s->irq_bound && atomic_load_explicit(&s->signals->irq_stopped,memory_order_acquire) &&
    !atomic_load_explicit(&s->signals->irq_pending,memory_order_acquire);}
unsigned npu_service_irq_completions(const struct npu_service *s)
{return s && s==instance && s->irq_bound ? atomic_load_explicit(&s->irq_completions,memory_order_acquire) : 0;}
static struct npu_service_client *caller(struct npu_service *s, unsigned *slot)
{
    if (!s || s != instance || !s->initialized ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return NULL;
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    for (unsigned i = 0; i < s->client_count; ++i)
        if (s->clients[i].task == task) { *slot = i; return &s->clients[i]; }
    return NULL;
}
int npu_service_send(struct npu_service *s, const struct npu_service_request *request,
                      TickType_t wait, uint64_t *ticket)
{
    if (!request || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot;
    struct npu_service_client *c = caller(s, &slot);
    if (!c || c->pending || c->next_ticket == UINT64_MAX ||
        atomic_load_explicit(&s->activation_state,memory_order_acquire)!=SERVICE_ACTIVE) return 0;
    struct npu_service_message m = {*request, c->next_ticket + 1, slot};
    c->pending = m.ticket;
    if (xQueueSend(s->requests, &m, wait) != pdTRUE) { c->pending = 0; return 0; }
    c->next_ticket = m.ticket; *ticket = m.ticket;
#ifdef SUNSTONE_A733_OS
    xTaskNotifyGive(s->worker);
#else
    if(s->irq_bound)xTaskNotifyGive(s->worker);
#endif
    return 1;
}
int npu_service_collect(struct npu_service *s, uint64_t ticket,
                         struct npu_service_reply *reply, TickType_t wait)
{
    if (!reply || !ticket || wait == portMAX_DELAY) return 0;
    unsigned slot;
    struct npu_service_client *c = caller(s, &slot);
    (void)slot;
    if (!c || c->pending != ticket) return 0;
    struct npu_service_reply value;
    if (xQueueReceive(c->replies, &value, wait) != pdTRUE) return 0;
    configASSERT(value.ticket == ticket);
    c->pending = 0; *reply = value;
    return 1;
}
