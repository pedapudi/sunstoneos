/* Resident service supervision. Each listed service records the counter time
 * of its latest bounded loop iteration; the supervising task renews the armed
 * watchdog only while every record is within its service's window. A failed
 * service parks or suspends without recording, so its window expires, renewal
 * stops and the watchdog resets the board within four seconds of the last
 * renewal. */
#include "os.h"
#include <stdatomic.h>
uint64_t a733_smp_counter(void);
/* Windows exceed each loop's expected pause: the board owners iterate every
 * tick and the thermal reader every 25 ms. The recognizer reports from its
 * wait loops and from the yield checkpoint that inference calls. The windows
 * are configuration; no board measurement of the longest pause exists. */
const struct os_service os_services[OS_SERVICES]={
    {"board-pmic",1000000},{"board-usb0",1000000},{"board-typec",1000000},
    {"board-power",1000000},{"board-usb2",1000000},{"mimus",3000000},{"thermal",1000000},
};
static atomic_uint_fast64_t seen[OS_SERVICES];
void board_service_progress(unsigned service)
{
    /* Zero means never; the counter is nonzero once the image runs. */
    if(service<OS_SERVICES)atomic_store_explicit(&seen[service],a733_smp_counter()/24|1,memory_order_release);
}
int os_services_healthy(uint64_t now)
{
    for(unsigned i=0;i<OS_SERVICES;++i){
        uint64_t t=atomic_load_explicit(&seen[i],memory_order_acquire);
        /* A record newer than now comes from another core's later read. */
        if(!t||(t<now&&now-t>os_services[i].window_us))return 0;
    }
    return 1;
}
/* Supervising task only. */
int board_resident_renew(void *supervisor)
{
    static uint64_t renewed;
    uint64_t now=a733_smp_counter()/24;
    if(!os_services_healthy(now))return 0;
    if(renewed&&now-renewed<OS_RENEW_US)return 1;
    if(os_watchdog_reload(supervisor)!=1)return 0;
    renewed=now;return 1;
}
