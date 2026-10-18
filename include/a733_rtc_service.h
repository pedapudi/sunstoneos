#ifndef SUNSTONE_A733_RTC_SERVICE_H
#define SUNSTONE_A733_RTC_SERVICE_H
#include "a733_rtc_initialize.h"
/* Task-context access for the two registered native clients. Caller identity is
 * the current task handle. IDs are never reused during this retained session. */
enum rtc_request_kind {RTC_REQUEST_READ=1,RTC_REQUEST_SET,RTC_REQUEST_ALARM};
enum rtc_request_outcome {RTC_REQUEST_OK=1,RTC_REQUEST_CANCELLED,
 RTC_REQUEST_LATE_CANCEL,RTC_REQUEST_BUSY,RTC_REQUEST_RETAINED,RTC_REQUEST_INVALID};
struct rtc_request {uint32_t kind,day,hms,seconds;};
struct rtc_service_result {
 uint64_t id;
 uint32_t client,kind,outcome,reserved;
 struct a733_rtc_time time;
 struct a733_rtc_calendar_result calendar;
 struct a733_rtc_snapshot alarm;
 uint32_t padding;
};
#define RTC_SERVICE_RECORDS 12
struct rtc_service_output {
 uint32_t count,complete,retained,omitted;
 struct rtc_service_result records[RTC_SERVICE_RECORDS];
};
/* submit:1 queued,0 slot occupied/queue busy,2 invalid/unregistered/refused.
 * copy:1 immutable result,0 pending,2 stale/unregistered/refused; clears output
 * unless1. release succeeds only for this task's matching terminal ID.
 * cancel never applies to a subsequently submitted operation. */
int rtc_service_submit(const struct rtc_request *,uint64_t *id);
int rtc_service_copy(uint64_t id,struct rtc_service_result *);
int rtc_service_release(uint64_t id);
int rtc_service_cancel(uint64_t id);
#endif
