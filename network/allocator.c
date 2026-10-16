#include "FreeRTOS.h"
#include <stdint.h>
/* The pinned kernel heap_4 owns this finite arena. No host heap or ISR allocator. */
_Alignas(16) uint8_t ucHeap[configTOTAL_HEAP_SIZE];
