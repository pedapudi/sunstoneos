/* SPDX-License-Identifier: Apache-2.0 */
/* Shared Bluetooth range helpers: NULL, empty, adjacent, overlapping and
 * address-space-end cases for both the strict and the empty-tolerant form.
 * The saturating deadline sum that the Bluetooth timers use is compared with
 * a 128-bit sum over edge values. */
#include "bt_memory.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    static unsigned char buffer[64];
    const unsigned char *a=buffer,*b=buffer+16;
    /* Adjacent ranges share no byte; one shared byte overlaps. */
    assert(bt_range_disjoint(a,16,b,16) && bt_range_disjoint(b,16,a,16));
    assert(!bt_range_disjoint(a,17,b,16) && !bt_range_disjoint(b,16,a,17));
    assert(!bt_range_disjoint(a,64,b,1) && !bt_range_disjoint(b,1,a,64) && !bt_range_disjoint(a,8,a,8));
    /* NULL and a range reaching the end of the address space fail closed. */
    assert(!bt_range_disjoint(NULL,1,b,16) && !bt_range_disjoint(a,16,NULL,1) && !bt_range_disjoint(NULL,0,b,16));
    const void *top=(const void *)(UINTPTR_MAX-7);
    assert(bt_range_disjoint(top,7,a,16) && !bt_range_disjoint(top,8,a,16) && !bt_range_disjoint(a,16,top,9));
    assert(!bt_range_disjoint(a,SIZE_MAX,b,1));
    /* A nonempty range keeps those rules in the empty-tolerant form. */
    assert(!bt_range_disjoint_empty(NULL,1,b,16) && !bt_range_disjoint_empty(top,8,a,16) && !bt_range_disjoint_empty(a,17,b,16));
    assert(bt_range_disjoint_empty(a,16,b,16));
    /* An empty range holds no byte: inside another range, at it, or NULL. */
    assert(bt_range_disjoint(b,0,a,64)==0 && bt_range_disjoint(a+64,0,a,64));
    assert(bt_range_disjoint_empty(b,0,a,64) && bt_range_disjoint_empty(a,64,b,0) && bt_range_disjoint_empty(NULL,0,a,64));
    assert(bt_range_disjoint_empty(NULL,0,NULL,0) && bt_range_disjoint_empty(top,0,top,8));
    static const uint64_t edges[]={0,1,2,1000000,60000000,UINT64_MAX/24,UINT64_MAX/2,UINT64_MAX-60000000,UINT64_MAX-1,UINT64_MAX};
    for(unsigned i=0;i<sizeof(edges)/sizeof(edges[0]);++i)for(unsigned j=0;j<sizeof(edges)/sizeof(edges[0]);++j){
        unsigned __int128 sum=(unsigned __int128)edges[i]+edges[j];
        assert(a733_deadline_after_saturating(edges[i],edges[j])==(sum>UINT64_MAX?UINT64_MAX:(uint64_t)sum));
    }
    puts("Saturating deadline sum: matches the 128-bit sum and saturates at UINT64_MAX.");
    puts("Shared Bluetooth range helpers: adjacency, overlap, NULL, empty and address-space-end cases passed.");
    return 0;
}
