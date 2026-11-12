#ifndef INC_FREERTOS_H
#define INC_FREERTOS_H
#include <assert.h>
typedef long BaseType_t;
#define configASSERT(condition) assert(condition)
#define portGET_CORE_ID() 0u
#define portMAX_API_PRIORITY_MASK 128u
#endif
