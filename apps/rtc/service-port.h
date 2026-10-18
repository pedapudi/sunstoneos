#ifndef SUNSTONE_RTC_SERVICE_PORT_H
#define SUNSTONE_RTC_SERVICE_PORT_H
#include "FreeRTOS.h"
#include "task.h"
#include "a733_rtc_service.h"
/* Concrete RTC provider boundary shared with the real FreeRTOS host fixture.
 * The native implementation retains the lease and checks it on every access. */
int rtc_service_live(void);
uint64_t rtc_service_deadline(void);
int rtc_service_now(void *,uint64_t *);
int rtc_service_read(void *,uint32_t,uint32_t *);
int rtc_service_write(void *,uint32_t,uint32_t);
int rtc_service_quiet(unsigned pending);
int rtc_service_mask(void);
int rtc_service_enable(void);
int rtc_service_pending(void);
int rtc_service_irq_fault(void);
void rtc_service_armed(unsigned);
void rtc_service_clear_pending(void);
void rtc_service_fault(void);
void rtc_service_hash(const void *,unsigned,uint8_t[32]);
void rtc_service_terminal(const struct rtc_service_output *);
int rtc_service_prepare(TaskHandle_t clients[2],uint32_t day,uint32_t hms,unsigned alarm_seconds);
void rtc_service_task(void);
void rtc_service_client(void *);
int rtc_service_active_cancel(void);
int rtc_service_first_attempt(void);
void rtc_service_note_attempt(void);
int rtc_copy_service(struct rtc_service_output *,uint8_t[32]);
#endif
