/* Resident service supervision. Each listed service records the counter time
 * of its latest bounded loop iteration; the supervising task renews the armed
 * watchdog only while every record is within its service's window, and makes
 * no renewal before every service has reported once. A failed
 * service parks or suspends without recording, so its window expires, renewal
 * stops and the watchdog resets the board within four seconds of the last
 * renewal. */
#include "os.h"
#include <stdatomic.h>
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
int os_services_healthy(uint64_t now)
{
    int pending=0;
    for(unsigned i=0;i<OS_SERVICES;++i){
        uint64_t t=atomic_load_explicit(&seen[i],memory_order_acquire);
        if(!t){pending=1;continue;}
        /* A record newer than now comes from another core's later read. */
        if(t<now&&now-t>os_services[i].window_us)return 0;
    }
    return pending?-1:1;
}
/* Supervising task only. */
int board_resident_renew(void *supervisor,uint64_t first_expiry)
{
    static uint64_t renewed;
    uint64_t ticks=a733_smp_counter(),now=ticks/24;
    int healthy=os_services_healthy(now);
    /* Services start after the supervisor's first pass. Until each has
     * reported once, renewal waits and the loader's first expiry bounds
     * startup. Records never return to zero, so this precedes any renewal. */
    if(healthy<0)return ticks<first_expiry;
    if(!healthy)return 0;
    if(renewed&&now-renewed<OS_RENEW_US)return 1;
    if(os_watchdog_reload(supervisor)!=1)return 0;
    renewed=now;return 1;
}
