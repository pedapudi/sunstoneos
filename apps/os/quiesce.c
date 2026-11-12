#include "quiesce.h"

extern int os_cpu_return_requested(void);
extern int os_cpu_return_failing(void);
static struct os_quiesce *owners;
static atomic_flag registration = ATOMIC_FLAG_INIT;
static unsigned sealed;
static atomic_uint armed_classes;

int os_quiesce_register(struct os_quiesce *q)
{
    if(!q||!q->step||q->order>OS_QUIESCE_TERMINALS)return 0;
    if(atomic_flag_test_and_set_explicit(&registration,memory_order_acquire))return 0;
    int ok=!sealed&&!os_cpu_return_requested()&&!q->registered;
    for(struct os_quiesce *p=owners;ok&&p;p=p->next)if(p->owner==q->owner)ok=0;
    if(ok){q->next=owners;owners=q;q->registered=1;}
    atomic_flag_clear_explicit(&registration,memory_order_release);
    return ok;
}
int os_quiesce_requested(const struct os_quiesce *q)
{
    return (atomic_load_explicit(&armed_classes,memory_order_acquire)&(1u<<q->order))&&
        atomic_load_explicit(&q->requested,memory_order_acquire)!=0;
}
enum os_quiesce_result os_quiesce_result(const struct os_quiesce *q)
{return (enum os_quiesce_result)atomic_load_explicit(&q->result,memory_order_acquire);}
void os_quiesce_complete(struct os_quiesce *q)
{
    unsigned expected=OS_QUIESCE_PENDING;
    (void)atomic_compare_exchange_strong_explicit(&q->result,&expected,OS_QUIESCE_DONE,memory_order_acq_rel,memory_order_acquire);
}
int os_quiesce_fault(struct os_quiesce *q,uint32_t site,uint32_t value,const void *caller)
{
    if(!os_cpu_return_failing()&&!os_quiesce_requested(q))return 0;
    unsigned none=0;
    if(atomic_compare_exchange_strong_explicit(&q->fault_state,&none,1,memory_order_acq_rel,memory_order_acquire)){
        q->site=site;q->value=value;q->caller=caller;
        atomic_store_explicit(&q->fault_state,2,memory_order_release);
    }
    atomic_store_explicit(&q->result,OS_QUIESCE_FAULT,memory_order_release);
    return 1;
}
int os_quiesce_drained_before(enum os_quiesce_order order)
{
    /* Supervisor only, after registration is sealed. */
    for(struct os_quiesce *p=owners;p;p=p->next)
        if(p->order<order&&os_quiesce_result(p)!=OS_QUIESCE_DONE)return 0;
    return 1;
}
enum os_quiesce_result os_quiesce_step(void (*note)(uint32_t,uint32_t,const void *))
{
    if(!os_cpu_return_requested())return OS_QUIESCE_PENDING;
    if(atomic_flag_test_and_set_explicit(&registration,memory_order_acquire))return OS_QUIESCE_PENDING;
    sealed=1;
    atomic_flag_clear_explicit(&registration,memory_order_release);
    unsigned faults=0;
    for(unsigned order=OS_QUIESCE_QUEUES;order<=OS_QUIESCE_TERMINALS;++order){
        unsigned pending=0;
        /* Peers can share an owner task (radio and headset). Publish the
         * whole class before one callback can park that task. */
        for(struct os_quiesce *p=owners;p;p=p->next)
            if(p->order==order)atomic_store_explicit(&p->requested,1,memory_order_release);
        atomic_fetch_or_explicit(&armed_classes,1u<<order,memory_order_release);
        for(struct os_quiesce *p=owners;p;p=p->next){
            if(p->order!=order)continue;
            enum os_quiesce_result r=os_quiesce_result(p);
            if(r!=OS_QUIESCE_DONE){
                atomic_store_explicit(&p->requested,1,memory_order_release);
                r=p->step(p);
                /* An interrupt may have published a fault during this pass. */
                if(os_quiesce_result(p)==OS_QUIESCE_FAULT&&r==OS_QUIESCE_PENDING)r=OS_QUIESCE_FAULT;
                if(r!=OS_QUIESCE_PENDING){
                    unsigned expected=OS_QUIESCE_PENDING;
                    (void)atomic_compare_exchange_strong_explicit(&p->result,&expected,r,memory_order_acq_rel,memory_order_acquire);
                }
                r=os_quiesce_result(p);
            }
            if(!p->visited&&atomic_load_explicit(&p->fault_state,memory_order_acquire)==2){
                if(note)note(p->site,p->value,p->caller);
                p->visited=1;
            }
            pending|=r==OS_QUIESCE_PENDING;faults|=r==OS_QUIESCE_FAULT;
        }
        if(pending)return OS_QUIESCE_PENDING;
    }
    return faults?OS_QUIESCE_FAULT:OS_QUIESCE_DONE;
}
