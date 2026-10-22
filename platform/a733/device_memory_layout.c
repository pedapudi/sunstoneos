#include "a733_device_memory_layout.h"
int a733_device_memory_layout_valid(const struct a733_device_memory_layout *p)
{
    return p && p->profile==A733_DEVICE_MEMORY_LAYOUT && p->wb_start==A733_DEVICE_WB_START &&
        p->wb_end==A733_DEVICE_WB_END && p->nc_start==A733_DEVICE_NC_START && p->nc_end==A733_DEVICE_NC_END;
}
int a733_device_memory_handoff_valid(const struct a733_device_memory_layout *p, const uint64_t w[6])
{
    return a733_device_memory_layout_valid(p) && w && w[0]==p->profile && w[1] &&
        w[2]==p->wb_start && w[3]==p->wb_end && w[4]==p->nc_start && w[5]==p->nc_end;
}
#if defined(SUNSTONE_A733_EXTENDED_DEVICE_MEMORY)
#include "a733_cached_memory.h"
#include "a733_cpu_coherence.h"
#include "a733_cpu_start.h"
#include "aarch64_device_memory.h"
#include "a733_memory_reservations.h"
#include "a733_memory_uncached.h"
#include "boot.h"
#include "cached.h"
extern unsigned char __device_memory_profile[], __device_pool_start[], __device_pool_end[];
extern unsigned char __uncached_pool_start[], __uncached_pool_end[], __heap_end[];
extern int a733_smp_device_preparation_admitted(void);
enum { EMPTY, BOUND, TABLES, READY, FAULT };
/* Written by the primary core only: at EL3 before the tables are built, and at
 * EL1 by the single preparation call before the scheduler starts. Starting the
 * scheduler orders every write before every task, so plain accesses suffice. */
static struct {
    struct a733_device_memory_layout layout;
    uint64_t epoch, startup_deadline;
    unsigned state;
} binding;
static int linked(struct a733_device_memory_layout *p)
{
    *p=(struct a733_device_memory_layout){(uintptr_t)__device_memory_profile,
        (uintptr_t)__device_pool_start,(uintptr_t)__device_pool_end,
        (uintptr_t)__uncached_pool_start,(uintptr_t)__uncached_pool_end};
    return a733_device_memory_layout_valid(p) && (uintptr_t)__heap_end<=UINT32_C(0x42000000);
}
static int cold_primary(void)
{
    uint64_t el,mpidr,daif,s3,s1;unsigned core;
    __asm__ volatile("mrs %0, CurrentEL":"=r"(el));if(el!=12)return 0;
    __asm__ volatile("mrs %0, mpidr_el1\n mrs %1, daif\n mrs %2, sctlr_el3\n mrs %3, sctlr_el1"
        :"=r"(mpidr),"=r"(daif),"=r"(s3),"=r"(s1));
    return a733_cpu_index(mpidr,&core) && !core && (daif&0x3c0)==0x3c0 && !((s3|s1)&UINT64_C(0x02001005));
}
int a733_device_memory_boot_admit(const struct a733_smp_contract *c)
{
    struct a733_device_memory_layout p;
    if(!cold_primary() || binding.state!=EMPTY || !c || !linked(&p) ||
        !a733_smp_contract_valid(c,a733_smp_counter()) || c->version!=2 ||
        !a733_device_memory_handoff_valid(&p,c->reserved))return 0;
    volatile const uint64_t *source=(const volatile uint64_t *)A733_SMP_CONTRACT_ADDRESS;
    const uint64_t *copy=(const uint64_t *)c;
    for(unsigned i=0;i<sizeof(*c)/8;++i)if(source[i]!=copy[i])return 0;
    __asm__ volatile("dsb sy":::"memory");
    if(source[0]!=c->magic || source[4]!=c->generation || !a733_smp_contract_valid(c,a733_smp_counter()))return 0;
    binding.layout=p;binding.epoch=c->epoch;binding.startup_deadline=c->startup_deadline;
    binding.state=BOUND;return 1;
}
int a733_device_memory_tables_admitted(struct a733_device_memory_layout *p)
{
    if(!p || !cold_primary() || binding.state!=BOUND)return 0;
    *p=binding.layout;return 1;
}
int a733_device_memory_tables_complete(void)
{
    if(!cold_primary() || binding.state!=BOUND)return 0;
    binding.state=TABLES;return 1;
}
int a733_device_memory_ready(void)
{
    return binding.state==READY;
}
static int prepared_profile(void)
{
    uint64_t ttbr,tcr,mair;
    __asm__ volatile("mrs %0, ttbr0_el1\n mrs %1, tcr_el1\n mrs %2, mair_el1"
        :"=r"(ttbr),"=r"(tcr),"=r"(mair));
    return ttbr==(uintptr_t)a733_cached_el1_l1 && tcr==A733_CPU_COHERENCE_TCR &&
        mair==A733_CPU_COHERENCE_MAIR && a733_cached_runtime[0].epoch==binding.epoch &&
        a733_cached_runtime[0].startup_deadline==binding.startup_deadline &&
        aarch64_device_cache_stride() && a733_smp_device_preparation_admitted();
}
int a733_device_memory_prepare(void)
{
    /* Only the primary at EL1 changes the state; another caller is refused
     * without poisoning a preparation it does not own. */
    uint64_t el,mpidr;unsigned core;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, mpidr_el1":"=r"(el),"=r"(mpidr));
    if(el!=4 || !a733_cpu_index(mpidr,&core) || core)return -1;
    if(binding.state==READY)return 1;
    /* Each pool preparation checks one write translation per 2 MiB block:
     * 384 for dma32 and 8 for uncached. A refusal is retained. */
    if(binding.state!=TABLES || !prepared_profile() ||
       !a733_memory_reservations_prepare() || !a733_memory_uncached_prepare()) {
        binding.state=FAULT;return -1;
    }
    binding.state=READY;return 1;
}
#endif
