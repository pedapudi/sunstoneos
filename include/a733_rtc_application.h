#ifndef SUNSTONE_A733_RTC_APPLICATION_H
#define SUNSTONE_A733_RTC_APPLICATION_H
#include "a733_board_application.h"
#include "a733_rtc_initialize.h"
#define A733_RTC_CONTRACT_MAGIC UINT64_C(0x4153373352544331)
#define A733_RTC_SESSION_MAGIC UINT64_C(0x4153373352544131)
/* Evidence authenticates exclusive RTC/IRQ228/R_CCU word ownership, exclusion
 * of remote writers and manual recovery. Adoption requires a stable external
 * clock; explicit initialization/runtime modes carry separate write authority.
 * Nonzero hashes do not establish that these physical facts are true. */
struct a733_rtc_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,authority;
 uint8_t initial[32],clock[32],electrical[32],supervision[32];
 uint32_t alarm_seconds,reserved;
};
_Static_assert(sizeof(struct a733_rtc_session)==768,"RTC session wire layout");
#define A733_RTC_INIT_SOURCE 1u
#define A733_RTC_SET_CALENDAR 2u
#define A733_RTC_RUNTIME_CALENDAR 4u
struct a733_rtc_setting {uint32_t flags,day,hms,reserved;};
struct a733_rtc_session_set {
 struct a733_rtc_session base;
 struct a733_rtc_setting setting;
};
_Static_assert(sizeof(struct a733_rtc_session_set)==784,"RTC explicit setting session");
int a733_rtc_setting_valid(const struct a733_rtc_setting *);
const struct a733_rtc_setting *rtc_admitted_setting(void);
const struct a733_rtc_initialization *rtc_admitted_initialization(void);
int a733_rtc_session_valid(const struct a733_rtc_session *);
int rtc_session_prepare(const struct a733_smp_contract *);
int rtc_boot_route(void);
const struct a733_rtc_session *rtc_admitted_session(void);
#endif
