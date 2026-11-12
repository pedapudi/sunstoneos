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
