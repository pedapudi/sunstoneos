/* Resident service supervision. Each listed service records the counter time
 * of its latest bounded loop iteration; the supervising task renews the armed
 * watchdog only while every record is within its service's window, and makes
 * no renewal before every service has reported once. A failed
 * service parks or suspends without recording, so its window expires, renewal
 * stops and the watchdog resets the board within four seconds of the last
 * renewal. */
#include "os.h"
#include <stdatomic.h>
#ifdef SUNSTONE_A733_FAULT_PAGE
#include "a733_fault.h"
#endif
uint64_t a733_smp_counter(void);
/* Windows exceed each loop's expected pause: the board owners, including the
 * radio owner and network client, the Bluetooth owner and the headset client
 * iterate every tick and the thermal reader every 25 ms. The recognizer reports from its
 * wait loops and from the yield checkpoint that inference calls. The capture
 * client reports from each pass without an unanswered DMA request. The windows
 * are configuration; no board measurement of the longest pause exists. */
const struct os_service os_services[OS_SERVICES]={
    {"board-pmic",1000000},{"board-usb0",1000000},{"board-typec",1000000},
    {"board-power",1000000},{"board-usb2",1000000},{"mimus",3000000},{"thermal",1000000},
#ifdef SUNSTONE_A733_OS_NPU
    /* One request can spend two seconds sending and two collecting. */
    {"npu",5000000},
#endif
#ifdef SUNSTONE_A733_OS_AUDIO
    {"audio",1000000},
#endif
#ifdef SUNSTONE_A733_OS_RADIO
    {"radio",1000000},{"network",1000000},
#endif
#ifdef SUNSTONE_A733_OS_HEADSET
    {"bluetooth",1000000},{"headset",1000000},
#endif
};
static atomic_uint_fast64_t seen[OS_SERVICES];
void board_service_progress(unsigned service)
{
    /* Zero means never; the counter is nonzero once the image runs. */
    if(service<OS_SERVICES)atomic_store_explicit(&seen[service],a733_smp_counter()/24|1,memory_order_release);
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
        if(!session){*first=OS_SERVICE_MIMUS;return 0;}
        required=(1u<<OS_SERVICES)-1;
        if(!a733_board_host_workload(session->board.workload))
            required&=~((1u<<2)|(1u<<3)|(1u<<4));
    }
    for(unsigned i=0;i<OS_SERVICES;++i){
        /* main.c creates these three tasks only for host workloads. Keep
         * their stable service IDs without demanding reports from absent tasks. */
        if(!(required&(1u<<i)))continue;
#ifdef SUNSTONE_A733_OS_NPU
        /* The finite caller publishes only after actual local stop. Its
         * suspended terminal state needs no artificial heartbeat. */
        if(i==OS_SERVICE_NPU&&os_npu_complete())continue;
#endif
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
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
/* A GPU loss: a GPU fault ended the boot, or the recognizer went silent while
 * it waited for OPEN or for a job's record. The supervisor calls this after
 * board_live's acquire of the board failure, and the recognizer records the
 * session state before its release of that failure, so no renewal pass is
 * needed: after board_fail, supervise() never calls board_resident_renew. */
uint64_t board_resident_stop_detail(void)
{
    unsigned gpu=os_dispatch_gpu_state();
    if(gpu==OS_DISPATCH_GPU_FAULT||(gpu==OS_DISPATCH_GPU_WAITING&&stop_detail==OS_SERVICE_MIMUS+1))
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
