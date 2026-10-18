#ifndef SUNSTONE_A733_RTC_BOOT_H
#define SUNSTONE_A733_RTC_BOOT_H
#include <stdint.h>
/* Single-primary, pre-scheduler EL3 only. Caller authenticates exclusive RTC,
 * R_CCU RTC word and SPI228 ownership, excluding remote writers/other cores.
 * No cleanup releases the bus or ownership after a partial transaction. */
struct a733_rtc_boot_ops {
 int (*read)(void *,uintptr_t,uint32_t *);
 int (*write)(void *,uintptr_t,uint32_t);
 int (*route)(void *,uintptr_t,uint64_t);
};
struct a733_rtc_boot {
 struct a733_rtc_boot_ops ops; void *context;
 uint32_t bus_before,bus_after; unsigned state;
};
/* Before cached boot's global SPI masking, so inherited enable is observable. */
int a733_rtc_boot_bus(struct a733_rtc_boot *,const struct a733_rtc_boot_ops *,void *);
/* After GIC affinity routing is enabled, before secondary CPU release. */
int a733_rtc_boot_route(struct a733_rtc_boot *,uint64_t affinity);
#endif
