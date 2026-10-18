/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_RTC_INITIALIZE_H
#define SUNSTONE_A733_RTC_INITIALIZE_H
#include "a733_rtc.h"
struct a733_rtc_setting;
/* Retained even on partial failure; raw inherited state is never decoded as a
 * valid calendar. stages1..6 identify admission/source/HMS/DAY/readback/success. */
struct a733_rtc_initialization {
 uint32_t before_control,before_status,before_day,before_hms;
 uint32_t after_control,after_status,after_day,after_hms;
 uint32_t requested,stage,writes,elapsed_us;
};
/* Single-primary EL3, inactive owned RTC/IRQ and exclusive osc32k authority.
 * Every callback and wait is bounded by deadline_us, including settling.
 * No rollback or guessed calendar repair after a partial write. */
int a733_rtc_initialize(const struct a733_rtc_setting *,const struct a733_rtc_ops *,
    void *,uint64_t deadline_us,struct a733_rtc_initialization *);
#endif
