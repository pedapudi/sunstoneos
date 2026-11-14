/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
void *memset(void *target,int value,size_t bytes)
{
    unsigned char *p=target;
    for (size_t i=0;i<bytes;++i) p[i]=(unsigned char)value;
    return target;
}
void *memcpy(void *target,const void *source,size_t bytes)
{
    unsigned char *p=target;
    const unsigned char *q=source;
    for (size_t i=0;i<bytes;++i) p[i]=q[i];
    return target;
}
/* The kernel's tick scan mask (kernel/scheduler/freertos.c); the check writes it. */
unsigned ss_test_scan_mask=0xffu;
unsigned ss_kernel_tick_scan_mask(void) { return ss_test_scan_mask; }
