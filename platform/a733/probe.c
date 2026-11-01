#include "sunstone_probe.h"
#include "boot.h"
#include "sunstone_crumb.h"
#define RECOVERY __attribute__((section(".text.probe_recovery"),noinline))
#define SESSION ((volatile const struct sunstone_probe_session *)SUNSTONE_PROBE_SESSION_ADDRESS)
#define RECEIPT ((volatile struct sunstone_probe_result *)SUNSTONE_PROBE_RESULT_ADDRESS)
#define MAILBOX ((volatile uint64_t *)(SUNSTONE_PROBE_SESSION_ADDRESS+128))
#define WDT UINT64_C(0x02050000)
#define KEY 0x16aa0000u
static struct sunstone_probe_session admitted __attribute__((section(".monitor_state")));
uint64_t sunstone_probe_deadline __attribute__((section(".monitor_state")));
static uint64_t started __attribute__((section(".monitor_state")));
extern void clean_data_cache(void);
extern _Noreturn void sunstone_probe_smc(unsigned status);
extern _Noreturn void sunstone_probe_rom(void);
/* The default owner retains normalized static OFF through ROM return.
 * Acquisition is entry-only; release stays in memtest's recovery closure. */
__attribute__((weak)) RECOVERY uint64_t sunstone_probe_release_secondaries(void)
{return SUNSTONE_PROBE_READY;}
static RECOVERY uint64_t hash(const volatile uint8_t *p,uint64_t bytes)
{
    uint64_t h=UINT64_C(0xcbf29ce484222325);
    while(bytes--){h^=*p++;h*=UINT64_C(0x100000001b3);}
    return h;
}
/* First failed secondary observation, kept outside EL1-cleared BSS. */
static volatile struct { uintptr_t address; uint32_t observed,check; } secondary_fault
    __attribute__((section(".monitor_state")));
static RECOVERY int off_word(uintptr_t address,uint32_t mask,uint32_t expected,unsigned check)
{
    uint32_t observed=mmio_read(address);
    if((observed&mask)==expected)return 1;
    secondary_fault.address=address;secondary_fault.observed=observed;secondary_fault.check=check;
    return 0;
}
static RECOVERY int secondaries_off(void)
{
    for(unsigned c=1;c<8;++c){
        uintptr_t p=0x07052000u+(uintptr_t)c*0x1000;
        /* Preserve the original short-circuit read order and every predicate. */
        if(!off_word(p+0xfc8,0xfff00fff,0x0b60043b,0) ||
           !off_word(p+0xfcc,UINT32_MAX,0x11,1) ||
           !off_word(p+0xfb0,0x101101ff,0x10110100,2) ||
           !off_word(p,UINT32_MAX,0,3) || !off_word(p+4,UINT32_MAX,0,4) ||
           !off_word(p+8,UINT32_MAX,0,5) ||
           !off_word(0x07050200+c*4,3,2,6) || !off_word(0x07050220+c*4,1,0,7))return 0;
    }
    return 1;
}
/* Entry-only state and helpers must not enlarge memtest's return closure. */
static void entry_word(uintptr_t address,uint32_t mask,uint32_t expected,unsigned check)
{
    if(!off_word(address,mask,expected,check))
        sunstone_probe_fault(0x20+secondary_fault.check,secondary_fault.observed,
            (uintptr_t)SUNSTONE_CRUMB_HERE(),secondary_fault.address);
}
struct entry_window { uint64_t last,deadline; unsigned polls; };
static void entry_bound(struct entry_window *w,uintptr_t address)
{
    uint64_t now=counter();
    if(now<w->last || now>=w->deadline || ++w->polls>65536)
        sunstone_probe_fault(0x28,now,(uintptr_t)SUNSTONE_CRUMB_HERE(),address);
    w->last=now;
}
__attribute__((weak)) uint64_t sunstone_probe_acquire_secondaries(void)
{
    struct { uint32_t policy,control,mode; } inherited[7];
    uint32_t dynamic=0;
    struct entry_window w={.last=counter()};
    w.deadline=w.last+COUNTER_HZ/10;
    if(w.deadline>sunstone_probe_deadline)w.deadline=sunstone_probe_deadline;
    /* Preflight every core before any write; never shut down an active owner.
     * Only acknowledged static/dynamic OFF and clear start requests qualify. */
    for(unsigned c=1;c<8;++c){
        uintptr_t p=0x07052000u+(uintptr_t)c*0x1000;
        entry_bound(&w,p);
        entry_word(p+0xfc8,0xfff00fff,0x0b60043b,0);
        entry_word(p+0xfcc,UINT32_MAX,0x11,1);
        entry_word(p+0xfb0,0x101101ff,0x10110100,2);
        uint32_t policy=mmio_read(p);
        if(policy&~0x100u)sunstone_probe_fault(0x23,policy,(uintptr_t)SUNSTONE_CRUMB_HERE(),p);
        inherited[c-1].policy=policy;
        if(policy)dynamic|=1u<<c;
        entry_word(p+4,UINT32_MAX,0,4);
        entry_word(p+8,UINT32_MAX,policy,5);
        uintptr_t control=0x07050200u+c*4;
        uint32_t v=mmio_read(control);
        if(v&1)sunstone_probe_fault(0x26,v,(uintptr_t)SUNSTONE_CRUMB_HERE(),control);
        inherited[c-1].control=v;
        v=mmio_read(control+0x20);
        if(v&1)sunstone_probe_fault(0x27,v,(uintptr_t)SUNSTONE_CRUMB_HERE(),control+0x20);
        inherited[c-1].mode=v;
    }
    /* LAST is overwritten by subsequent progress. Keep the full inherited
     * profile in the diagnostic words until a real first fault replaces it. */
    volatile uint32_t *rtc=(void *)(uintptr_t)SUNSTONE_CRUMB_RTC;
    unsigned record=(rtc[SUNSTONE_CRUMB_FAULT/4]>>24)!=SUNSTONE_CRUMB_FAULT_MAGIC;
    if(record){
        rtc[SUNSTONE_CRUMB_ESR/4]=dynamic;
        rtc[SUNSTONE_CRUMB_FAR/4]=0;
        __asm__ volatile("dsb sy":::"memory");
        rtc[SUNSTONE_CRUMB_ELR/4]=SUNSTONE_CRUMB_PROBE_POLICY_MAGIC;
        __asm__ volatile("dsb sy":::"memory");
    }
    for(unsigned c=1;c<8;++c){
        uintptr_t p=0x07052000u+(uintptr_t)c*0x1000,control=0x07050200u+c*4;
        entry_bound(&w,p);
        entry_word(control,UINT32_MAX,inherited[c-1].control,6);
        entry_word(control+0x20,UINT32_MAX,inherited[c-1].mode,7);
        entry_word(p,UINT32_MAX,inherited[c-1].policy,3);
        sunstone_crumb_el3(SUNSTONE_CRUMB_PROBE_POLICY,(c<<12)|inherited[c-1].policy);
        if(inherited[c-1].policy){mmio_write(p,0);__asm__ volatile("dsb sy":::"memory");}
        /* DEN0051E 3.4.3/4.1.1: PWSR dynamic-disable acknowledgment and
         * actual OFF complete after any in-flight transition. PWPR can read
         * its inherited value until then; verify it only after completion. */
        for(;;){
            uint32_t state=mmio_read(p+8);
            entry_bound(&w,p+8);
            entry_word(control,UINT32_MAX,inherited[c-1].control,6);
            entry_word(control+0x20,UINT32_MAX,inherited[c-1].mode,7);
            if(!state)break;
            if(state&~0x108u)sunstone_probe_fault(0x25,state,(uintptr_t)SUNSTONE_CRUMB_HERE(),p+8);
        }
        entry_word(p,UINT32_MAX,0,3);
        /* Vendor CPU_OFF blocks this core's GIC wake, preserving other bits.
         * No CPU0, cluster, domain, rail, reset or clock register is written. */
        if(!(inherited[c-1].control&2)){
            mmio_write(control,inherited[c-1].control|2);
            __asm__ volatile("dsb sy":::"memory");
        }
        entry_word(control,UINT32_MAX,inherited[c-1].control|2,6);
        if(record){
            rtc[SUNSTONE_CRUMB_FAR/4]|=1u<<c;
            __asm__ volatile("dsb sy":::"memory");
        }
    }
    return SUNSTONE_PROBE_READY;
}
/* Device-specific emergency cutoff is bounded and independent of EL1 state. */
RECOVERY __attribute__((weak)) void sunstone_probe_abort_devices(void) {}
RECOVERY _Noreturn void sunstone_probe_fault(uint64_t site,uint64_t esr,uint64_t elr,uint64_t far)
{
    __asm__ volatile("msr daifset,#15":::"memory");
    if(!elr && site!=0x104)elr=(uintptr_t)__builtin_return_address(0);
    volatile uint32_t *rtc=(void *)(uintptr_t)SUNSTONE_CRUMB_RTC;
    if((rtc[SUNSTONE_CRUMB_FAULT/4]>>24)!=SUNSTONE_CRUMB_FAULT_MAGIC){
        rtc[SUNSTONE_CRUMB_ESR/4]=(uint32_t)esr;
        rtc[SUNSTONE_CRUMB_ELR/4]=(uint32_t)elr;
        rtc[SUNSTONE_CRUMB_FAR/4]=(uint32_t)far;
        __asm__ volatile("dsb sy":::"memory");
        rtc[SUNSTONE_CRUMB_FAULT/4]=(site==0x104 ? 0xfa010000u : 0xfa030000u)|((uint32_t)site&255);
        __asm__ volatile("dsb sy":::"memory");
        /* Publish the origin before cutoff. A recursive abort faults with this
         * magic already set, so it neither overwrites evidence nor retries. */
        sunstone_probe_abort_devices();
    }
    for(;;)__asm__ volatile("wfe");
}
RECOVERY void sunstone_probe_progress_el3(void)
{
    if(counter()>=sunstone_probe_deadline || (mmio_read(WDT+0x14)&0xffff)!=0xb1)sunstone_probe_timeout();
    mmio_write(WDT+0x0c,0x14af);sunstone_crumb_el3(SUNSTONE_CRUMB_PROBE_PROGRESS,0);
}
RECOVERY _Noreturn void sunstone_probe_timeout(void)
{sunstone_probe_fault(2,0,0,0);}
void sunstone_probe_begin(void)
{
    uint64_t el,mpidr,ctl;
    __asm__ volatile("mrs %0,CurrentEL\n mrs %1,mpidr_el1\n mrs %2,sctlr_el3":"=r"(el),"=r"(mpidr),"=r"(ctl));
    const struct sunstone_probe_marker *m=&sunstone_probe_marker;

    admitted=*SESSION;
    if(admitted.magic!=SUNSTONE_PROBE_SESSION_MAGIC || admitted.version!=1 || admitted.bytes!=128 ||
       !admitted.epoch || !admitted.generation || !admitted.lease || !admitted.seconds ||
       admitted.seconds>m->seconds || admitted.reserved[0] || admitted.reserved[1] || admitted.reserved[2] ||
       admitted.input_bytes!=m->input_bytes ||
       hash((void *)(uintptr_t)m->input_address,m->input_bytes)!=admitted.input_hash ||
       (mmio_read(WDT+0x14)&0xffff)!=m->watchdog_mode ||
       (mmio_read(WDT+0x10)&0xffff)!=1 || (mmio_read(WDT+0x18)&0xffff)!=2)
        sunstone_probe_fault(1,0,0,0);
    mmio_write(SUNSTONE_CRUMB_RTC+SUNSTONE_CRUMB_TAG,0x5c0d0000u|(admitted.epoch&0xffff));
    sunstone_crumb_el3(SUNSTONE_CRUMB_PROBE_BEGIN,0);
    /* Entry faults retain the initial sampled CPU/PPU word and its origin.
     * These checks have the same predicates and order as the combined gate. */
    if(el!=12)sunstone_probe_fault(0x10,el,(uintptr_t)SUNSTONE_CRUMB_HERE(),0);
    if(mpidr&0xffffff)sunstone_probe_fault(0x11,mpidr,(uintptr_t)SUNSTONE_CRUMB_HERE(),0);
    if(ctl&0x1005)sunstone_probe_fault(0x12,ctl,(uintptr_t)SUNSTONE_CRUMB_HERE(),0);
    started=counter();sunstone_probe_deadline=started+admitted.seconds*COUNTER_HZ;
    if(sunstone_probe_acquire_secondaries()!=SUNSTONE_PROBE_READY)sunstone_probe_fault(0x16,0,0,0);
    if(!secondaries_off())sunstone_probe_fault(0x20+secondary_fault.check,
        secondary_fault.observed,(uintptr_t)SUNSTONE_CRUMB_HERE(),secondary_fault.address);
    MAILBOX[0]=MAILBOX[1]=MAILBOX[2]=0;
    mmio_write(SUNSTONE_CRUMB_RTC+SUNSTONE_CRUMB_TAG,0x5c0d0000u|(admitted.epoch&0xffff));
    sunstone_crumb_el3(SUNSTONE_CRUMB_PROBE_BEGIN,0);
}
_Noreturn void sunstone_probe_complete(unsigned status,size_t bytes)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if((el!=4 && el!=12) || bytes>sunstone_probe_marker.payload_bytes ||
       sunstone_probe_quiesce()!=SUNSTONE_PROBE_READY){
        if(el==12)sunstone_probe_fault(3,0,0,0);
        sunstone_probe_smc(0xffffffffu);
    }
    /* Publish through the caller's actual security-state mapping. No writer
     * may touch the payload after the quiescence callback completes. */
    uintptr_t p=sunstone_probe_marker.payload_address&~(uintptr_t)63;
    for(uintptr_t end=sunstone_probe_marker.payload_address+bytes;p<end;p+=64)
        __asm__ volatile("dc cvac,%0"::"r"(p):"memory");
    __asm__ volatile("dsb sy":::"memory");
    MAILBOX[0]=bytes;MAILBOX[1]=status;MAILBOX[2]=SUNSTONE_PROBE_READY;
    __asm__ volatile("dsb sy":::"memory");
    if(el==12)sunstone_probe_return(status);
    sunstone_probe_smc(status);
}
RECOVERY _Noreturn void sunstone_probe_return(unsigned status)
{
    uint64_t el,ctl,el1;
    __asm__ volatile("msr daifset,#15\n mrs %0,CurrentEL\n mrs %1,sctlr_el3\n mrs %2,sctlr_el1":"=r"(el),"=r"(ctl),"=r"(el1)::"memory");
    if(el!=12 || (ctl&0x1005) || (el1&0x1005) || status==0xffffffffu ||
       counter()>=sunstone_probe_deadline || MAILBOX[2]!=SUNSTONE_PROBE_READY || MAILBOX[1]!=status ||
       MAILBOX[0]>sunstone_probe_marker.payload_bytes || !secondaries_off())sunstone_probe_fault(4,0,0,0);
    if(sunstone_probe_restore_interrupts()!=SUNSTONE_PROBE_READY)sunstone_probe_fault(6,0,0,0);
    sunstone_crumb_el3(SUNSTONE_CRUMB_PROBE_CLEAN,0);
    clean_data_cache();
    volatile struct sunstone_probe_result *r=RECEIPT;
    r->magic=SUNSTONE_PROBE_RESULT_MAGIC;r->version=1;r->bytes=128;
    r->epoch=admitted.epoch;r->generation=admitted.generation;r->lease=admitted.lease;
    r->status=status;r->payload_bytes=MAILBOX[0];
    r->payload_hash=hash((void *)(uintptr_t)sunstone_probe_marker.payload_address,r->payload_bytes);
    r->cleanup=7;r->retired=0xfe;r->start_ticks=started;r->end_ticks=counter();r->failure_site=0;
    /* Finish I-cache work while secondary ownership is still held. The ROM
     * tail must contain no cache instruction after the optional release. */
    __asm__ volatile("ic iallu\n dsb sy\n isb":::"memory");
    if(sunstone_probe_release_secondaries()!=SUNSTONE_PROBE_READY)sunstone_probe_fault(0x17,0,0,0);
    if(counter()>=sunstone_probe_deadline)sunstone_probe_timeout();
    mmio_write(WDT+0x14,KEY);
    uint32_t mode=mmio_read(WDT+0x14);
    if(mode&1)sunstone_probe_fault(5,0,0,0);
    r->watchdog_mode=mode;r->complete=SUNSTONE_PROBE_COMPLETE;
    sunstone_crumb_el3(SUNSTONE_CRUMB_RETURN_ROM,mode);
    sunstone_probe_rom();
}
