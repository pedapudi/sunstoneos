#ifndef SUNSTONE_RUNTIME_H
#define SUNSTONE_RUNTIME_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void runtime_init(void);
// Maximum bytes obtained by the allocator through _sbrk, including free blocks.
// This is not the sum of currently live allocations.
size_t runtime_heap_peak(void);
#ifdef __cplusplus
}
#endif
#endif
