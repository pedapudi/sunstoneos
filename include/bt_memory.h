/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_MEMORY_H
#define SUNSTONE_BT_MEMORY_H
#include <stddef.h>
#include <stdint.h>
/* Exact1 when [a,a+an) and [b,b+bn) share no byte. A NULL pointer or a range
 * that reaches the end of the address space fails closed as overlapping. */
static inline int bt_range_disjoint(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return a && b && an<=UINTPTR_MAX-x && bn<=UINTPTR_MAX-y && (x+an<=y || y+bn<=x);
}
/* As above, except that an empty range holds no byte and is disjoint from
 * everything, even when its pointer is NULL. For optional parameter buffers. */
static inline int bt_range_disjoint_empty(const void *a,size_t an,const void *b,size_t bn)
{return !an || !bn || bt_range_disjoint(a,an,b,bn);}
#endif
