#ifndef SUNSTONE_RTC_ALARM_RETURNING_MONITOR_H
#define SUNSTONE_RTC_ALARM_RETURNING_MONITOR_H
#include <stdint.h>
/* Written only at EL3; outside the BSS cleared by EL1. EL1 never publishes
 * cached data into this record. Final EL3 cleanup checks live devices. */
struct rtc_alarm_route {
    uint64_t entry_el,entry_sctlr,entry_scr,return_el,return_sctlr;
    uint32_t reason,baseline,mutated,ready,restored,retained;
    uint32_t ctlr,typer,enabled,pending,active,group,modifier,configuration,priority;
    uint64_t affinity;
    uint32_t alarm_day,alarm_hms;
};
extern volatile struct rtc_alarm_route rtc_alarm_route;
#endif
