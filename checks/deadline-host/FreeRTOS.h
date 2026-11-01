#ifndef DEADLINE_HOST_FREERTOS_H
#define DEADLINE_HOST_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
void deadline_host_enter(void);
void deadline_host_exit(void);
unsigned deadline_host_isr_enter(void);
void deadline_host_isr_exit(unsigned);
void deadline_host_yield(int);
#define taskENTER_CRITICAL() deadline_host_enter()
#define taskEXIT_CRITICAL() deadline_host_exit()
#define taskENTER_CRITICAL_FROM_ISR() deadline_host_isr_enter()
#define taskEXIT_CRITICAL_FROM_ISR(x) deadline_host_isr_exit(x)
#define portYIELD_FROM_ISR(x) deadline_host_yield(x)
#endif
