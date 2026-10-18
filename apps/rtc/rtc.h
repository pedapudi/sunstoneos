#ifndef SUNSTONE_RTC_PRIVATE_H
#define SUNSTONE_RTC_PRIVATE_H
#include "a733_rtc_application.h"
#include "a733_rtc.h"
#include "service-port.h"
#include "gicv3_spi.h"
#include "../board/application.h"
struct rtc_initialized_output {struct a733_rtc_snapshot calendar;struct a733_rtc_initialization initialization;};
struct rtc_runtime_output {struct a733_rtc_calendar_result calendar;struct a733_rtc_initialization boot;};
struct rtc_application {
 struct a733_rtc_session session;
 struct a733_rtc_setting setting;
 struct rtc_initialized_output initialized_output;
 uint8_t initialized_digest[32];
 struct rtc_runtime_output runtime_output;
 uint8_t runtime_digest[32];
 unsigned runtime_active,runtime_attempted;
 struct a733_rtc driver;
 struct a733_clock_token clock;
 struct gicv3_spi spi;
 StaticTask_t task,clients[2]; StackType_t stack[2048],client_stacks[2][1024]; TaskHandle_t handle,client_handles[2];
 atomic_uint result,cancel,armed,pending;
 struct a733_rtc_snapshot output;
 uint8_t digest[32];
};
extern struct rtc_application rtc;
void rtc_task(void *);
void rtc_application_irq(void);
int rtc_enable_irq(void);
int rtc_copy(struct a733_rtc_snapshot *,uint8_t[32]);
void rtc_runtime_task(void);
int rtc_copy_runtime(struct rtc_runtime_output *,uint8_t[32]);
int rtc_copy_initialized(struct rtc_initialized_output *,uint8_t[32]);
#endif
