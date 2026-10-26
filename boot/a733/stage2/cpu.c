/* Fixed cold-card CPU owner. Protocol/source pins: cpu-sources.json and
 * docs/designs/cold-cpu-static-off.md. No CORE_RESET or power-rail writes. */
#include <stdint.h>

int a733_stage2_entry_state(void);
int a733_stage2_time_valid(void);
int a733_stage2_cpu_cache_owned(void);
int a733_stage2_cpu_retained(void);

static unsigned attempted;
static uint64_t started, last;
static unsigned samples;
static uint32_t rd(uintptr_t a)
{
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t v=*(volatile const uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}
static void wr(uintptr_t a,uint32_t v)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)a=v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint64_t now(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(v)::"memory");
    return v;
}
static int bounded(void)
{
    uint64_t v=now();
    if(v<last || v-started>=UINT64_C(2400000) || ++samples>65536)return 0;
    last=v;return 1;
}
static uintptr_t ppu(unsigned core) {return 0x07052000u+(uintptr_t)core*0x1000;}
static uintptr_t control(unsigned core) {return 0x07050200u+(uintptr_t)core*4;}
static int requests_clear(unsigned core)
{return !(rd(control(core))&1) && !(rd(control(core)+0x20)&1);}
int a733_stage2_cpu_retained(void) {return attempted!=0;}

int a733_stage2_cpu_cache_owned(void)
{
    uint64_t clidr,selector;
    uint32_t geometry[3];
    /* Fixed A55 L1D32KiB/L2128KiB and DSU L31MiB, each with 64-byte lines.
     * Validate all three before the first maintenance operation. */
    static const uint32_t expected[3]={0x000fe01a,0x003fe01a,0x007fe07a};
    if(attempted || !a733_stage2_entry_state() || !a733_stage2_time_valid() ||
       (rd(0x0701022c)&0x10001)!=0x10001)return 0;
    __asm__ volatile("mrs %0,clidr_el1\n mrs %1,csselr_el1":"=r"(clidr),"=r"(selector));
    if((clidr&0x071fffff)!=0x03000123)return 0;
    for(unsigned level=0;level<3;++level) {
        uint64_t g;
        __asm__ volatile("msr csselr_el1,%1\n isb\n mrs %0,ccsidr_el1":"=r"(g):"r"((uint64_t)level*2):"memory");
        geometry[level]=(uint32_t)g&0x0fffffff;
    }
    __asm__ volatile("msr csselr_el1,%0\n isb"::"r"(selector):"memory");
    for(unsigned level=0;level<3;++level)if(geometry[level]!=expected[level])return 0;
    /* All preflight reads precede the first policy write. AIDR binds the
     * architecture; IIDR binds PCK-600 (revision fields may differ). */
    for(unsigned core=1;core<8;++core) {
        uintptr_t p=ppu(core);
        uint32_t id=rd(p+0xfb0),policy=rd(p);
        if((rd(p+0xfc8)&0xfff00fff)!=0x0b60043b || rd(p+0xfcc)!=0x11 ||
           (id&0x101101ff)!=0x10110100 || rd(p+4) || (policy&~0x100u) ||
           rd(p+8)!=policy || !requests_clear(core))return 0;
    }
    started=last=now();samples=0;
    for(unsigned core=1;core<8;++core) {
        uintptr_t p=ppu(core);
        if(!bounded() || !requests_clear(core) || (rd(p)&~0x100u))return 0;
        attempted=1; /* A write may take effect even if subsequent reads fail. */
        wr(p,0);
        for(;;) {
            if(!bounded() || !requests_clear(core) || rd(p))return 0;
            uint32_t state=rd(p+8);
            if(!state)break;
            /* Dynamic acknowledgment may follow an already-started wake.
             * OFF/ON are admitted while waiting, never as completion. */
            if(state&~0x108u)return 0;
        }
        uint32_t c=rd(control(core));
        if(c&1)return 0;
        wr(control(core),c|2);
        if(rd(control(core))!=(c|2))return 0;
    }
    for(unsigned core=1;core<8;++core)
        if(!bounded() || rd(ppu(core)) || rd(ppu(core)+8) ||
           !requests_clear(core) || !(rd(control(core))&2))return 0;
    /* An in-flight wake can flush private data into L3 after boot0 cleanup.
     * With every writer now excluded, finish all levels before SD admission.
     * Loop dimensions are bounded by the preflight, not untrusted shifts. */
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned level=0;level<3;++level) {
        unsigned ways=level==2?16:4,sets=level==0?128:level==1?512:1024;
        unsigned shift=level==2?28:30;
        for(unsigned way=0;way<ways;++way) {
            if(!bounded())return 0;
            for(unsigned set=0;set<sets;++set) {
                uint64_t operand=((uint64_t)way<<shift)|((uint64_t)set<<6)|(level<<1);
                __asm__ volatile("dc cisw,%0"::"r"(operand):"memory");
            }
        }
        __asm__ volatile("dsb sy":::"memory");
    }
    __asm__ volatile("dsb sy\n isb":::"memory");
    return bounded();
}
