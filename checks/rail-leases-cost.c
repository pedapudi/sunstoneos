/* SPDX-License-Identifier: Apache-2.0 */
/* Freestanding AArch64 entry points for checks/rail-leases-cost.py. Each
 * cost_* function runs one ledger or registry operation on statically placed
 * objects, so the emulator counts its instructions and the cache lines it
 * touches. The file uses only the public ledger, registry and GPU manager
 * interface, so it builds against the merge-base tree as well. */
#include "a733_rail_leases.h"
#include "a733_gpu_clock_parent.h"
#include <stddef.h>

void *memset(void *d,int c,size_t n){unsigned char *p=d;while(n--)*p++=(unsigned char)c;return d;}
void *memcpy(void *d,const void *s,size_t n){unsigned char *p=d;const unsigned char *q=s;while(n--)*p++=*q++;return d;}

static uint64_t clock_us=1;
static int now(void *v,uint64_t *out){(void)v;*out=++clock_us;return 1;}
static int read32(void *v,uintptr_t a,uint32_t *out){(void)v;(void)a;*out=0;return 1;}
static int update32(void *v,uintptr_t a,uint32_t r,uint32_t s,uint64_t d){(void)v;(void)a;(void)r;(void)s;(void)d;return 1;}
static int rail(void *v,struct a733_gpu_resource_rail *r,uint64_t d)
{(void)v;(void)d;*r=(struct a733_gpu_resource_rail){.chip=3,.enable=8,.voltage=30};return 1;}

struct a733_clock_leases cost_clocks __attribute__((aligned(64)));
struct a733_gpu_resource_manager cost_gpu __attribute__((aligned(64)));
struct a733_rail_leases cost_rails __attribute__((aligned(64)));
static struct a733_clock_token clocks[128];
static struct a733_rail_token rails[128];
static struct a733_rail_borrow borrow;
static const struct a733_gpu_resource_ops ops={now,read32,update32,rail};
unsigned long cost_ledger_bytes=sizeof(struct a733_rail_leases);
unsigned long cost_registry_bytes=sizeof(struct a733_clock_leases);

/* Rails that every ledger version accepts shared. */
static const enum a733_rail shared_rails[]={A733_RAIL_BLDO1,A733_RAIL_CLDO3,A733_RAIL_BLDO5,
    A733_RAIL_ELDO6,A733_RAIL_DCDC8,A733_RAIL_DCDC9,A733_RAIL_CLDO1,A733_RAIL_CLDO5};
static unsigned clock_count,rail_count;

/* Returns 0 on success, else the failing step. */
int cost_setup(unsigned clock_leases,unsigned rail_leases)
{
    if(!a733_clock_leases_init(&cost_clocks,now,0))return 1;
    if(!a733_gpu_resources_init(&cost_gpu,&ops,0))return 2;
    const struct a733_rail_binding b={&cost_clocks,&cost_gpu,99,now,0};
    if(!a733_rail_leases_init(&cost_rails,&b))return 3;
    /* Local controller scopes are exclusive and independent, so many owners
     * can hold them at once; shared OSC fills the remaining slots. */
    static const a733_clock_mask_t locals[]={A733_CLOCK_UART0,A733_CLOCK_UART1,A733_CLOCK_UART2,
        A733_CLOCK_SPI0,A733_CLOCK_SPI1,A733_CLOCK_SDC1,A733_CLOCK_TWI9,A733_CLOCK_TWI11};
    for(unsigned i=0;i<clock_leases;++i) {
        a733_clock_mask_t shared=i<8 ? 0 : A733_CLOCK_OSC,exclusive=i<8 ? locals[i] : 0;
        if(a733_clock_acquire(&cost_clocks,100+i,shared,exclusive,UINT64_MAX,&clocks[i])!=A733_CLOCK_OK)return 4;
    }
    for(unsigned i=0;i<rail_leases;++i) {
        enum a733_rail_result r=a733_rail_acquire(&cost_rails,100+i,
            A733_RAIL_BIT(shared_rails[i%8]),0,UINT64_MAX,&rails[i]);
        if(r!=A733_RAIL_PENDING)return 5;
        if(a733_rail_poll(&cost_rails,&rails[i])!=A733_RAIL_READY)return 6;
    }
    clock_count=clock_leases;rail_count=rail_leases;
    if(a733_rail_borrow(&cost_rails,&rails[rail_leases-1],rails[rail_leases-1].shared,&borrow)!=A733_RAIL_READY)return 7;
    if(a733_rail_end_borrow(&cost_rails,&borrow)!=A733_RAIL_READY)return 8;
    return 0;
}
/* The measured tokens are the last ones granted. */
int cost_rail_status(void){return a733_rail_status(&cost_rails,&rails[rail_count-1])!=A733_RAIL_READY;}
int cost_clock_status(void){return a733_clock_token_status(&cost_clocks,&clocks[clock_count-1])!=A733_CLOCK_OK;}
int cost_cycle(void)
{
    struct a733_rail_token t;
    if(a733_rail_acquire(&cost_rails,9,A733_RAIL_BIT(A733_RAIL_BLDO1),0,UINT64_MAX,&t)!=A733_RAIL_PENDING)return 1;
    if(a733_rail_poll(&cost_rails,&t)!=A733_RAIL_READY)return 2;
    return a733_rail_release(&cost_rails,&t,UINT64_MAX)!=A733_RAIL_READY ? 3 : 0;
}
int cost_borrow(void)
{return a733_rail_borrow(&cost_rails,&rails[rail_count-1],rails[rail_count-1].shared,&borrow)!=A733_RAIL_READY;}
int cost_end_borrow(void){return a733_rail_end_borrow(&cost_rails,&borrow)!=A733_RAIL_READY;}
/* The check the PMIC provider makes before each bus transaction. */
#ifdef A733_POWER_AUTHORITY_LINE
/* Kept out of line so that the count includes it. */
__attribute__((noinline)) int authority_check(void)
{return a733_power_authority_admits(&cost_rails.authority,A733_POWER_PMIC_TRANSFER);}
int cost_authority(void){return !authority_check();}
#else
int cost_authority(void)
{
    return a733_clock_token_status(&cost_clocks,&clocks[clock_count-1])!=A733_CLOCK_OK ||
        a733_rail_borrow_status(&cost_rails,&borrow)!=A733_RAIL_READY;
}
#endif
