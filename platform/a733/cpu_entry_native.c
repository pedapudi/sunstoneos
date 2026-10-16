#include "a733_cpu_entry.h"

extern unsigned char a733_cpu_entry_image_start[],a733_cpu_entry_image_end[];
static int boot_state(void)
{
    uint64_t el,sctlr,mpidr;unsigned core;
    __asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    if(el!=12)return 0;
    __asm__ volatile("mrs %0, sctlr_el3\n mrs %1, mpidr_el1":"=r"(sctlr),"=r"(mpidr));
    return !(sctlr&UINT64_C(0x02000005)) && a733_cpu_index(mpidr,&core) && !core;
}
static void barrier(void) {__asm__ volatile("dsb sy":::"memory");}
static void publish(void) {__asm__ volatile("dsb sy\n sev":::"memory");}
static int read_slot(void *unused,unsigned core,struct a733_cpu_slot *out)
{
    (void)unused;
    if(!out || !core || core>=8 || !boot_state())return 0;
    volatile const uint64_t *p=(volatile const uint64_t *)&a733_cpu_mailboxes[core];
    /* Report fields are immutable once ARRIVED, except state and fault details.
     * A publication overlapping the copy yields a pending snapshot. */
    uint64_t before=p[21];barrier();
    uint64_t words[32];
    for(unsigned i=0;i<32;++i)words[i]=p[i];
    barrier();uint64_t after=p[21];
    if(before!=after)words[21]=0;
    unsigned char *dst=(unsigned char *)out;
    const unsigned char *src=(const unsigned char *)words;
    for(size_t i=0;i<sizeof(words);++i)dst[i]=src[i];
    return 1;
}
static int arm(void *unused,unsigned core,const struct a733_cpu_command *command)
{
    (void)unused;
    if(!command || !core || core>=8 || !boot_state())return 0;
    volatile uint64_t *p=(volatile uint64_t *)&a733_cpu_mailboxes[core].command;
    const uint64_t *src=(const uint64_t *)command;
    /* CPU is still OFF. Never overwrite a retained command/report. */
    volatile const uint64_t *whole=(volatile const uint64_t *)&a733_cpu_mailboxes[core];
    for(unsigned i=0;i<32;++i)if(whole[i])return 0;
    for(unsigned i=0;i<16;++i)if(i!=3)p[i]=src[i];
    barrier();p[3]=A733_ENTRY_ARMED;publish();return 1;
}
static int release(void *unused,unsigned core,uint64_t epoch)
{
    (void)unused;
    if(!core || core>=8 || !epoch || !boot_state())return 0;
    volatile struct a733_cpu_command *p=&a733_cpu_mailboxes[core].command;
    if(p->magic!=A733_ENTRY_MAGIC || p->epoch!=epoch || p->core!=core ||
       p->state!=A733_ENTRY_ARMED || p->inverse_epoch!=~epoch)return 0;
    for(unsigned i=0;i<11;++i)if(p->reserved[i])return 0;
    barrier();p->state=A733_ENTRY_RELEASE;publish();return 1;
}
static int now_us(void *unused,uint64_t *out)
{
    (void)unused;uint64_t ticks,frequency;
    if(!out || !boot_state())return 0;
    __asm__ volatile("isb\n mrs %0, cntpct_el0\n mrs %1, cntfrq_el0":"=r"(ticks),"=r"(frequency)::"memory");
    if(!frequency || frequency>UINT32_MAX || ticks/frequency>UINT64_MAX/1000000)return 0;
    uint64_t whole=ticks/frequency*1000000,part=ticks%frequency*1000000/frequency;
    if(whole>UINT64_MAX-part)return 0;
    *out=whole+part;return 1;
}
static int range(uintptr_t base,size_t bytes)
{return base>=UINT32_C(0x40000000) && base<=UINT32_MAX && bytes && bytes<=UINT64_C(0x100000000)-base;}
static int disjoint(uintptr_t a,size_t an,uintptr_t b,size_t bn)
{return a+an<=b || b+bn<=a;}
int a733_cpu_entry_native_init(struct a733_cpu_entry *s,struct a733_cpu_start *p,uint64_t epoch)
{
    uintptr_t entry=(uintptr_t)a733_cpu_secondary_entry,begin=(uintptr_t)a733_cpu_entry_image_start;
    uintptr_t end=(uintptr_t)a733_cpu_entry_image_end,mail=(uintptr_t)a733_cpu_mailboxes;
    uintptr_t stacks=(uintptr_t)a733_cpu_entry_stacks,next=(uintptr_t)a733_cpu_secondary_continue;
    if(!boot_state() || end<=begin || entry!=begin || (entry&2047) ||
       (mail&127) || (stacks&4095) || (next&3) || !range(begin,end-begin) ||
       !range(mail,sizeof(a733_cpu_mailboxes)) || !range(stacks,sizeof(a733_cpu_entry_stacks)) ||
       !range(next,4) || !disjoint(begin,end-begin,mail,sizeof(a733_cpu_mailboxes)) ||
       !disjoint(begin,end-begin,stacks,sizeof(a733_cpu_entry_stacks)) ||
       !disjoint(mail,sizeof(a733_cpu_mailboxes),stacks,sizeof(a733_cpu_entry_stacks)) ||
       !disjoint(next,4,begin,end-begin) || !disjoint(next,4,mail,sizeof(a733_cpu_mailboxes)) ||
       !disjoint(next,4,stacks,sizeof(a733_cpu_entry_stacks)))return 0;
    struct a733_cpu_entry_config c={.power=p,.epoch=epoch,.entry=entry,
        .ops={read_slot,arm,release,now_us}};
    return a733_cpu_entry_init(s,&c);
}
