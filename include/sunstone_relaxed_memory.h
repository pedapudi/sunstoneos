#ifndef SUNSTONE_RELAXED_MEMORY_H
#define SUNSTONE_RELAXED_MEMORY_H
/* Force-included (-include) into the units that cmake/codegen-align.cmake lists as relaxed. Their calls to the memory routines
 * link runtime/memory_unaligned.c, which reads and writes with unaligned 8-byte accesses. The strict runtime/memory.c and
 * runtime/memmove.c stay for the code that runs with the MMU off or touches Device memory. A call the compiler makes by
 * itself (the copy of a large structure, say) names memcpy and links the strict routine, which is correct and slower. */
#include <stddef.h>
void *ss_memcpy_unaligned(void *restrict dest, const void *restrict src, size_t count);
void *ss_memmove_unaligned(void *dest, const void *src, size_t count);
void *ss_memset_unaligned(void *dest, int value, size_t count);
#define memcpy ss_memcpy_unaligned
#define memmove ss_memmove_unaligned
#define memset ss_memset_unaligned
#endif
