#ifndef SUNSTONE_SDRT_RETURN_H
#define SUNSTONE_SDRT_RETURN_H
#include <stdint.h>
/* Separate NC page from the descriptor chain and from the monitor log. */
#define SDRT_RETURN_ADDRESS UINT64_C(0x42101000)
#define SDRT_RETURN_MAGIC UINT64_C(0x3154455252445353)
enum { SDRT_RETURN_UNARMED=1, SDRT_RETURN_REFUSED, SDRT_RETURN_ARMED,
       SDRT_RETURN_READY, SDRT_RETURN_CONSUMED, SDRT_RETURN_RETAINED };
struct sdrt_return_receipt { uint64_t magic, state; };
#define SDRT_RETURN ((volatile struct sdrt_return_receipt *)(uintptr_t)SDRT_RETURN_ADDRESS)
#endif
