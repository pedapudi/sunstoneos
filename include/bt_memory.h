/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_MEMORY_H
#define SUNSTONE_BT_MEMORY_H
#include "a733_range.h"
/* Bluetooth names for two shared predicates in a733_range.h.
 *
 * Returns 1 when [a,a+an) and [b,b+bn) share no byte. A NULL pointer or a range
 * that reaches the end of the address space fails closed as overlapping. */
static inline int bt_range_disjoint(const void *a,size_t an,const void *b,size_t bn)
{return a733_ranges_disjoint(a,an,b,bn);}
/* As above, except that an empty range holds no byte and is disjoint from
 * everything, even when its pointer is NULL. For optional parameter buffers. */
static inline int bt_range_disjoint_empty(const void *a,size_t an,const void *b,size_t bn)
{return a733_ranges_disjoint_or_empty(a,an,b,bn);}
#endif
