/* Model FreeRTOS types for the diagnostic protocol check. */
#ifndef MODEL_FREERTOS_H
#define MODEL_FREERTOS_H
#include <stdint.h>
typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
typedef uint64_t StackType_t;
typedef struct { int unused; } StaticTask_t;
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
_Noreturn void platform_panic(const char *file, unsigned line);
#define configASSERT(condition) do { if (!(condition)) platform_panic(__FILE__, __LINE__); } while (0)
#endif
