#ifndef SUNSTONE_RTC_PRIVATE_H
#define SUNSTONE_RTC_PRIVATE_H
#include "a733_rtc_application.h"
#include "a733_rtc.h"
#include "gicv3_spi.h"
#include "../board/application.h"
struct rtc_application {
 struct a733_rtc_session session;
 struct a733_rtc driver;
 struct a733_clock_token clock;
 struct gicv3_spi spi;
 StaticTask_t task; StackType_t stack[2048]; TaskHandle_t handle;
 atomic_uint result,cancel,armed,pending;
 struct a733_rtc_snapshot output;
 uint8_t digest[32];
};
extern struct rtc_application rtc;
void rtc_task(void *);
void rtc_application_irq(void);
int rtc_enable_irq(void);
int rtc_copy(struct a733_rtc_snapshot *,uint8_t[32]);
#endif
