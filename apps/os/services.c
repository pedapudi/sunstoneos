/* Resident service supervision. Active owners report actual bounded progress.
 * A registered blocked owner may instead supply a certificate for its next
 * precise wait and an absolute operation budget within the admitted session.
 * The supervisor accepts only that real wait and a bounded observation period
 * after wake. A failed wait, expired budget or stale active record stops
 * watchdog renewal. */
#include "os.h"
#include "application.h"
#include <stdatomic.h>
#include "sunstone_deadline.h"
#ifdef SUNSTONE_A733_FAULT_PAGE
#include "a733_fault.h"
#endif
uint64_t a733_smp_counter(void);
/* The absolute runtime deadline (apps/board/main.c). The sealed session's
 * board prefix has zero time fields and cannot bound a wait. */
uint64_t board_runtime_deadline_us(void);
/* Windows bound active work and the interval from a wake to its checked
 * result. They are configuration, not board measurements of worst latency. */
static atomic_uint_fast64_t seen[OS_SERVICES];
static _Atomic(TaskHandle_t) blocked_owner[OS_SERVICES];
/* Protected by the kernel critical section, including paired publication. */
static struct { uint64_t deadline,sequence; } blocked[OS_SERVICES];
int board_service_deadline_register(unsigned service)
{
    if(service>=OS_SERVICES||!sunstone_deadline_register())return 0;
    TaskHandle_t self=xTaskGetCurrentTaskHandle(),empty=NULL;
    return atomic_compare_exchange_strong_explicit(&blocked_owner[service],&empty,self,
        memory_order_release,memory_order_relaxed)||empty==self;
}
int board_service_blocked_until(unsigned service,uint64_t deadline)
{
    if(service>=OS_SERVICES)return 0;
    TaskHandle_t owner=atomic_load_explicit(&blocked_owner[service],memory_order_acquire);
    const struct os_session *session=os_session_admitted();
    if(!owner||owner!=xTaskGetCurrentTaskHandle()||!session||
       deadline>board_runtime_deadline_us()||deadline<=a733_smp_counter()/24)return 0;
    taskENTER_CRITICAL();
    struct sunstone_deadline_record r;
    int ok=sunstone_deadline_owner_record(owner,&r)&&!r.failed&&!r.armed&&r.sequence<UINT64_MAX;
    if(ok){blocked[service].deadline=deadline;blocked[service].sequence=r.sequence+1;}
    taskEXIT_CRITICAL();return ok;
}
void board_service_progress(unsigned service)
{
    /* Zero means never; the counter is nonzero once the image runs. */
    if(service<OS_SERVICES){
        TaskHandle_t owner=atomic_load_explicit(&blocked_owner[service],memory_order_acquire);
        if(owner){
            /* An unrelated task cannot disguise a stalled admitted owner. */
            if(owner!=xTaskGetCurrentTaskHandle())return;
            taskENTER_CRITICAL();
            blocked[service].deadline=blocked[service].sequence=0;
            sunstone_deadline_owner_progress(owner);
            atomic_store_explicit(&seen[service],a733_smp_counter()/24|1,memory_order_release);
            taskEXIT_CRITICAL();return;
        }
        atomic_store_explicit(&seen[service],a733_smp_counter()/24|1,memory_order_release);
    }
}
/* As os_services_healthy; *first is the first stale service, or else the
 * first silent one. */
static int services_state(uint64_t now,unsigned *first)
{
    int pending=0;
    /* The supervisor is the sole reader. The admitted session is sealed for
     * this boot; derive its task set once, outside the steady renewal path.
     * Board service IDs 2..4 are Type-C, power and USB2 (start.c asserts them). */
    static unsigned required;
    if(!required){
        const struct os_session *session=os_session_admitted();
        if(!session){*first=OS_SERVICE_APPLICATION;return 0;}
        required=(1u<<OS_SERVICES)-1;
        if(!a733_board_host_workload(session->board.workload))
            required&=~((1u<<2)|(1u<<3)|(1u<<4));
        /* A service the session does not grant has no task and reports nothing (the grant of each supervised task is
         * declared in apps/os/os-services.h). */
        static const unsigned needs[OS_SERVICES]=OS_SERVICE_GRANTS;
        for(unsigned i=0;i<OS_SERVICES;++i)if(needs[i]&&(session->granted&needs[i])!=needs[i])required&=~(1u<<i);
    }
    for(unsigned i=0;i<OS_SERVICES;++i){
        /* main.c creates these three tasks only for host workloads. Keep
         * their stable service IDs without demanding reports from absent tasks. */
        if(!(required&(1u<<i)))continue;
        if(i==OS_SERVICE_APPLICATION&&(!ss_application.manifest||os_application_finished()))continue;
        /* The finite caller publishes only after actual local stop. Its
         * suspended terminal state needs no artificial heartbeat. */
        if(i==OS_SERVICE_NPU&&os_npu_complete())continue;
        TaskHandle_t owner=atomic_load_explicit(&blocked_owner[i],memory_order_acquire);
        if(owner){
            struct sunstone_deadline_record r;
            taskENTER_CRITICAL();
            uint64_t limit=blocked[i].deadline,sequence=blocked[i].sequence;
            int recorded=sunstone_deadline_owner_record(owner,&r);
            taskEXIT_CRITICAL();
            if(!recorded||r.failed||(limit&&now>=limit)){*first=i;return 0;}
            if(limit&&r.sequence==sequence&&r.deadline_us<=limit){
                /* Alarm expiry can be a successful hold. Its certificate
                 * survives wake until real progress, bounded by both the
                 * operation budget and this service's observation window. */
                uint64_t observation=r.woken_us&&r.woken_us<r.deadline_us?
                    r.woken_us:r.deadline_us;
                /* A lost or late alarm cannot extend the device deadline.
                 * Early device notification starts the same window sooner. */
                if(now>=observation&&now-observation>os_services[i].window_us){*first=i;return 0;}
                if(r.woken_us){
                    continue;
                }
                if(r.armed)continue;
            }
        }
        uint64_t t=atomic_load_explicit(&seen[i],memory_order_acquire);
        if(!t){if(!pending)*first=i;pending=1;continue;}
        /* A record newer than now comes from another core's later read. */
        if(t<now&&now-t>os_services[i].window_us){*first=i;return 0;}
    }
    return pending?-1:1;
}
int os_services_healthy(uint64_t now)
{unsigned first;return services_state(now,&first);}
/* Supervising task only: why renewal was refused. The supervisor stops at
 * its first refusal, so the value is never reset. */
static uint64_t stop_detail;
#ifdef SUNSTONE_A733_OS_GPU
/* A GPU loss: a GPU fault ended the boot, or the recognizer went silent while
 * it waited for OPEN or for a job's record. The supervisor calls this after
 * board_live's acquire of the board failure, and the recognizer records the
 * session state before its release of that failure, so no renewal pass is
 * needed: after board_fail, supervise() never calls board_resident_renew. */
uint64_t board_resident_stop_detail(void)
{
    unsigned gpu=os_gpu_queue_state();
    if(gpu==2||(gpu==1&&stop_detail==OS_SERVICE_APPLICATION+1))
        return OS_STOP_DETAIL_GPU_LOSS;
    return stop_detail;
}
#else
uint64_t board_resident_stop_detail(void){return stop_detail;}
#endif
/* Supervising task only. */
int board_resident_renew(void *supervisor,uint64_t first_expiry)
{
    static uint64_t renewed;
    uint64_t ticks=a733_smp_counter(),now=ticks/24;
    unsigned first=0;
#ifdef SUNSTONE_A733_FAULT_PAGE
    /* A core in the EL3 fault handler has marked its byte; that core may be
     * parked with services that still report. One relaxed load: the byte is
     * the only datum, and the next 20 ms pass sees a later store. */
    if(__atomic_load_n(&a733_fault_pending,__ATOMIC_RELAXED))return 0;
#endif
    int healthy=services_state(now,&first);
    /* Services start after the supervisor's first pass. Until each has
     * reported once, renewal waits and the loader's first expiry bounds
     * startup. Records never return to zero, so this precedes any renewal. */
    if(healthy<0){if(ticks<first_expiry)return 1;stop_detail=first+1;return 0;}
    if(!healthy){stop_detail=first+1;return 0;}
    if(renewed&&now-renewed<OS_RENEW_US)return 1;
    if(os_watchdog_reload(supervisor)!=1)return 0;
    renewed=now;return 1;
}
