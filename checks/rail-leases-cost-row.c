/* SPDX-License-Identifier: Apache-2.0 */
/* Freestanding AArch64 entry points for the domain-state read measured by
 * checks/rail-leases-cost.py. With the power-domain owner, cost_row_state()
 * reads an answered row the way the GPU platform poll, the framebuffer and
 * G2D do; the read is inline in the consumer. The second branch serves only
 * rail-leases-cost.py --root on a tree that still has the DE_SYS provider,
 * such as the merge base whose figures docs/designs/power-domains.md
 * records: cost_row_state() then runs one provider poll while the domain
 * waits for its acknowledgement, with callbacks that count lock entries and
 * register reads. */
#include <stddef.h>
#include <stdint.h>
void *memset(void *d,int c,size_t n){unsigned char *p=d;while(n--)*p++=(unsigned char)c;return d;}
void *memcpy(void *d,const void *s,size_t n){unsigned char *p=d;const unsigned char *q=s;while(n--)*p++=*q++;return d;}
unsigned long cost_lock_entries,cost_register_reads;
#if __has_include("a733_power_domain_row.h")
#include "a733_power_domain_row.h"
static struct a733_power_domain_row row;
int cost_setup(unsigned a,unsigned b)
{
    (void)a;(void)b;
    atomic_store_explicit(&row.request,(1u<<1)|1u,memory_order_relaxed);
    atomic_store_explicit(&row.state,(1u<<3)|A733_POWER_ROW_ON,memory_order_relaxed);
    return 0;
}
/* Out of line, so that the count includes the inline read. */
__attribute__((noinline)) static int row_read(void){return a733_power_domain_state(&row)!=A733_POWER_ROW_ON;}
int cost_row_state(void){return row_read();}
#else
#include "a733_de_sys_power.h"
static uint64_t clock_us=1;
static uint32_t words[0x1000/4];
static int authorize(void *v){(void)v;return 1;}
static int now(void *v,uint64_t *out){(void)v;*out=++clock_us;return 1;}
static int enter(void *v,uint64_t d){(void)v;(void)d;++cost_lock_entries;return 1;}
static void leave(void *v){(void)v;}
static int read(void *v,uintptr_t a,uint32_t *out){(void)v;++cost_register_reads;*out=words[(a&0xfff)/4];return 1;}
static int write(void *v,uintptr_t a,uint32_t value){(void)v;words[(a&0xfff)/4]=value;return 1;}
static struct a733_de_sys_power provider;
int cost_setup(unsigned a,unsigned b)
{
    (void)a;(void)b;
    const struct a733_de_sys_power_ops ops={authorize,now,enter,leave,read,write};
    if(!a733_de_sys_power_init(&provider,&ops,0,UINT64_MAX/2))return 1;
    /* INITIAL: delay row and ON command; the domain then waits. */
    if(a733_de_sys_power_poll(&provider)!=0)return 2;
    cost_lock_entries=cost_register_reads=0;
    return 0;
}
int cost_row_state(void){return a733_de_sys_power_poll(&provider)!=0;}
#endif
