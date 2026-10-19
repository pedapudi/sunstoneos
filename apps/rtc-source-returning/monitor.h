#ifndef SUNSTONE_RTC_SOURCE_RETURNING_MONITOR_H
#define SUNSTONE_RTC_SOURCE_RETURNING_MONITOR_H
#include <stdint.h>
struct rtc_source_admission {
    uint64_t el,sctlr,scr;
    uint32_t reason,ready,ctlr,typer,enabled,pending,active;
};
extern volatile struct rtc_source_admission rtc_source_admission;
#endif
