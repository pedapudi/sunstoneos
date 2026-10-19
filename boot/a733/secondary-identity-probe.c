// One sequential batch through the original power and entry controllers.
#include "secondary-identity-probe.h"
#include "a733_cpu_entry.h"
#include "boot.h"
#ifdef SUNSTONE_SECONDARY_LOCAL
#include "secondary-local-probe.h"
#endif

#define HEADER ((volatile struct identity_header *)IDENTITY_REPORT)
#define SLOTS ((volatile struct identity_slot *)IDENTITY_SLOTS)
static void barrier(void) { __asm__ volatile("dsb sy":::"memory"); }
static uint32_t read32(uintptr_t address)
{ barrier();uint32_t value=*(volatile uint32_t *)address;barrier();return value; }
static void write32(uintptr_t address,uint32_t value)
{ barrier();*(volatile uint32_t *)address=value;barrier(); }
static int before(uint64_t deadline)
{ return a733_smp_counter()<deadline; }
static int native_read(void *context,uintptr_t address,uint32_t *out)
{
    if(!a733_smp_power_address(*(unsigned *)context,address,0))return 0;
    *out=read32(address);return 1;
}
static int native_write(void *context,uintptr_t address,uint32_t value)
{
    if(!a733_smp_power_address(*(unsigned *)context,address,1))return 0;
    write32(address,value);return 1;
}
static int native_barrier(void *context) {(void)context;barrier();return 1;}
static int native_now(void *context,uint64_t *out)
{(void)context;*out=a733_smp_counter()/24;return 1;}

int identity_primary_run(void)
{
    const volatile struct a733_smp_contract *admission=(void *)A733_SMP_CONTRACT_ADDRESS;
    uint64_t mpidr,el,sctlr,daif;
    __asm__ volatile("mrs %0,mpidr_el1\n mrs %1,CurrentEL\n mrs %2,sctlr_el3\n mrs %3,daif"
        :"=r"(mpidr),"=r"(el),"=r"(sctlr),"=r"(daif));
    unsigned boot;
    if(el!=12 || !a733_cpu_index(mpidr,&boot) || boot || (sctlr&A733_ENTRY_SCTLR_REJECT) ||
       (daif&0x3c0)!=0x3c0 || !admission->epoch || !admission->lease ||
       !before(admission->startup_deadline))return 0;
    // Host zeroes the entire report before upload; stale state is never adopted.
    const volatile uint64_t *words=(void *)HEADER;
    for(unsigned i=0;i<IDENTITY_REPORT_BYTES/8;++i)if(words[i])return 0;
    HEADER->magic=IDENTITY_MAGIC;HEADER->version=1;
    HEADER->bytes=IDENTITY_REPORT_BYTES;HEADER->epoch=admission->epoch;
    HEADER->deadline=admission->startup_deadline;HEADER->state=1;barrier();
#ifdef SUNSTONE_SECONDARY_LOCAL
    if(!secondary_distributor_begin(HEADER->deadline))return 0;
#endif
    for(unsigned core=IDENTITY_FIRST_CORE;core<8;++core) {
        volatile struct identity_slot *slot=&SLOTS[core];
        uintptr_t config=0x08001000+core*0x1000,control=0x07050200+core*4;
        uintptr_t mode=control+0x20,status=0x07052008+core*0x1000;
        HEADER->core=core;barrier();
        if(!before(HEADER->deadline) || (read32(status)&15))return 0;
        slot->architecture=read32(config);slot->entry_low=read32(config+4);
        slot->entry_high=read32(config+8);slot->control=read32(control);slot->mode=read32(mode);
        if((slot->control|slot->mode)&1)return 0;
        // Each CPU receives fresh one-shot controller objects and mailbox state.
        struct a733_cpu_start power={0};struct a733_cpu_entry owner={0};
        struct a733_cpu_start_config start={.ops={native_read,native_write,native_barrier,native_now},
            .context=&core,.boot_mpidr=mpidr,.entry=(uintptr_t)a733_cpu_secondary_entry,
            .lease=admission->lease,.deadline=HEADER->deadline/24,.core=core};
        if(!a733_cpu_start_init(&power,&start) ||
           !a733_cpu_entry_native_init(&owner,&power,admission->epoch))return 0;
        while(a733_cpu_entry_poll(&owner)==A733_CPU_ENTRY_PENDING) {}
        slot->owner_state=owner.state;slot->report_state=owner.report.state;barrier();
        if(owner.state!=A733_CPU_ENTRY_DISPATCHED || owner.report.state!=A733_ENTRY_RELEASED)return 0;
#ifdef SUNSTONE_SECONDARY_COHERENCE
        while(!slot->reserved[0]) {barrier();if(!before(HEADER->deadline))return 0;}
        if(slot->reserved[0]!=1)return 0;
#else
        while(!slot->ready) {barrier();if(!before(HEADER->deadline))return 0;}
        if(slot->ready!=1)return 0;
#endif
        if(slot->epoch!=HEADER->epoch || slot->core!=core ||
           slot->midr!=owner.report.midr || slot->mpidr!=owner.report.mpidr ||
           slot->sctlr3!=owner.report.sctlr || slot->target_control!=slot->control ||
           slot->target_mode!=slot->mode || (slot->power_before&1))return 0;
#ifdef SUNSTONE_SECONDARY_COHERENCE
    }
#ifdef SUNSTONE_ALL_CORE_PRE_SCHEDULER
    // The primary joins only after all seven targets have been released. The
    // outer owner retains the distributor until CPU0 restores and targets are OFF.
    extern int primary_local_cache_probe(uint64_t epoch, uint64_t deadline);
    extern void a733_cached_block_walks(void);
    uint64_t sctlr1,tcr1,restored_sctlr1,restored_tcr1;
    __asm__ volatile("mrs %0,sctlr_el1\n mrs %1,tcr_el1":"=r"(sctlr1),"=r"(tcr1));
    HEADER->reserved[0]=sctlr1;HEADER->reserved[1]=tcr1;barrier();
    // The reused prefix ends after restoring CPU0's original EL1 controls.
    // Reapply the production preparation before EL3 enables its caches.
    a733_cached_block_walks();
    if(primary_local_cache_probe(HEADER->epoch,HEADER->deadline)!=1)return 0;
    __asm__ volatile("msr sctlr_el1,%0\n isb\n msr tcr_el1,%1\n isb"
        ::"r"(sctlr1),"r"(tcr1):"memory");
    __asm__ volatile("mrs %0,sctlr_el1\n mrs %1,tcr_el1":"=r"(restored_sctlr1),"=r"(restored_tcr1));
    HEADER->reserved[2]=restored_sctlr1;HEADER->reserved[3]=restored_tcr1;barrier();
    if(restored_sctlr1!=sctlr1 || restored_tcr1!=tcr1)return 0;
#endif
    // All targets are released before waiting for cached shared-memory work.
    for(unsigned core=IDENTITY_FIRST_CORE;core<8;++core) {
        volatile struct identity_slot *slot=&SLOTS[core];
        uintptr_t config=0x08001000+core*0x1000,control=0x07050200+core*4;
        uintptr_t mode=control+0x20,status=0x07052008+core*0x1000;
        HEADER->core=core;barrier();
        while(!slot->ready) {barrier();if(!before(HEADER->deadline))return 0;}
        if(slot->ready!=1)return 0;
#endif
        slot->request=HEADER->epoch;barrier();__asm__ volatile("sev":::"memory");
        while(slot->ack!=HEADER->epoch) {barrier();if(!before(HEADER->deadline))return 0;}
        do {if(!before(HEADER->deadline))return 0;slot->off=read32(status);}while(slot->off&15);
        // ACK alone cannot release ownership. Actual OFF precedes every restore.
        write32(config+4,(uint32_t)slot->entry_low);write32(config+8,(uint32_t)slot->entry_high);
        write32(config,(uint32_t)slot->architecture);write32(mode,(uint32_t)slot->mode);
        write32(control,(uint32_t)slot->control);
        if(read32(config+4)!=slot->entry_low || read32(config+8)!=slot->entry_high ||
           read32(config)!=slot->architecture || read32(mode)!=slot->mode ||
           read32(control)!=slot->control || (read32(status)&15))return 0;
        slot->restored=1;barrier();HEADER->completed|=UINT64_C(1)<<core;barrier();
    }
#ifdef SUNSTONE_SECONDARY_LOCAL
    if(!secondary_distributor_restore(HEADER->deadline))return 0;
#endif
#ifdef SUNSTONE_SECONDARY_COHERENCE
    // Every writer has completed NS clean+invalidate and reached observed OFF.
    // Capture the resulting physical memory before the host restores its backup.
    extern unsigned char a733_cpu_el1_stacks[];
    const volatile uint64_t *shared=(void *)a733_cpu_el1_stacks;
    volatile uint64_t *copy=(void *)0x4201b200;
    for(unsigned i=0;i<4096/8;++i)copy[i]=shared[i];
#ifdef SUNSTONE_ALL_CORE_PRE_SCHEDULER
    extern unsigned char probe_scheduler_state[];
    const volatile uint64_t *state=(void *)probe_scheduler_state;
    for(unsigned i=0;i<4096/8;++i)copy[4096/8+i]=state[i];
#endif
    barrier();
#endif
    HEADER->state=2;barrier();return 1;
}
