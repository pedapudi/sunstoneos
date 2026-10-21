/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_MAINTENANCE_WATCHDOG_INTERNAL_H
#define SUNSTONE_A733_MAINTENANCE_WATCHDOG_INTERNAL_H
#include "a733_maintenance_watchdog.h"
#include <stdatomic.h>
/* Private core/native boundary; these are hardware operations, not progress
 * providers. Host checks replace this boundary with recorded MMIO operations. */
/* RTC control bits 8:7 report calendar write busy and stay set while an RTC
 * owner's HMS/DAY write settles. They do not select the LOSC/watchdog clock
 * path, so the reload comparison ignores them; every other control bit,
 * the divider and the source word must match exactly. */
#define A733_WATCHDOG_RTC_BUSY UINT32_C(0x180)
struct a733_watchdog_window {
    uint64_t deadline_us,guard_us;
    uint32_t rtc_control,rtc_divider,rtc_source;
};
int a733_watchdog_hw_admit(const struct a733_watchdog_window *);
int a733_watchdog_hw_reload(const struct a733_watchdog_window *,
    struct a733_counter_clock *,struct a733_platform_locks *,void *,
    atomic_uint *,uint64_t,uint64_t *,uint64_t *);
int a733_maintenance_hw_admit(const struct a733_maintenance_transfer *);
int a733_maintenance_hw_task(void *);
int a733_maintenance_hw_reload(const struct a733_maintenance_transfer *,
    struct a733_counter_clock *,struct a733_platform_locks *,void *,
    atomic_uint *,uint64_t,uint64_t *,uint64_t *);
#endif
