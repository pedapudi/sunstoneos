/* Local-layout QEMU fixture for the cached table builder
 * (platform/a733-smp-cached/memory.c), linked with the production cached SMP
 * linker script and run on one core. checks/device-memory-capacity.py builds
 * it with and without the uncached pool and the read-only GPU output view,
 * and runs each image with the DRAM decode check passing (mode 0) and failing
 * (mode 1) under an execution trace. Semihosting markers separate the traced
 * calls in this order: the builder before the decode check, the check, the
 * builder after it, the local pool preparation, one getter call, one
 * aarch64_device_translate() call, one aarch64_device_translate_el0() call
 * and an empty pair that measures the marker cost. */
#include "a733_cached_memory.h"
#include "a733_cpu_coherence.h"
#include "a733_device_memory_layout.h"
#include "a733_memory_local.h"
#include "a733_memory_reservations.h"
#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
#include <string.h>
#define MONITOR __attribute__((section(".bss.a733_smp_monitor"),aligned(128)))
#define BIT(n) (UINT64_C(1)<<(n))
unsigned char fixture_el3_stacks[8*16384] MONITOR;
unsigned char fixture_el1_stacks[8*32768] __attribute__((aligned(4096)));
volatile uint32_t fixture_mode=0xaabbccdd; /* Runner replaces only this fixture control. */
extern unsigned char __bss_start[],__bss_end[],__tables_start[],__tables_end[];
extern unsigned char __device_pool_start[],__device_pool_end[],__uncached_pool_start[],__uncached_pool_end[];
extern _Noreturn void fixture_exit(unsigned,uint64_t);
static void say(const char *m)
{
    register uintptr_t operation __asm__("x0")=4,argument __asm__("x1")=(uintptr_t)m;
    __asm__ volatile("hlt #0xf000":"+r"(operation),"+r"(argument)::"memory");
}
static void say_hex(const char *label,uint64_t v)
{
    char m[80];unsigned n=0;
    while(label[n] && n<60){m[n]=label[n];++n;}
    for(int i=15;i>=0;--i){unsigned d=(unsigned)(v>>(i*4))&15;m[n++]=(char)(d<10?'0'+d:'a'+d-10);}
    m[n++]='\n';m[n]=0;say(m);
}
static void check(int condition,unsigned line)
{
    if(condition)return;
    say_hex("FAIL ",line);fixture_exit(1,line);
}
#define CHECK(x) check(!!(x),__LINE__)
static int decode_passes(void){return fixture_mode==0;}
static struct a733_dram_range decode_seen[4];
static unsigned decode_count,decode_calls;
int a733_dram_decode_check(const struct a733_dram_range *r,unsigned n)
{
    say("TRACE decode\n");
    ++decode_calls;decode_count=n;
    for(unsigned i=0;i<n && i<4;++i)decode_seen[i]=r[i];
    say("TRACE decode end\n");
    return decode_passes();
}
uint64_t a733_smp_counter(void)
{uint64_t v;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(v));return v;}
/* The local EL1 map: image RAM and boot assets below 0x48000000 from
 * a733_cpu_coherence_tables(), the uncached block when linked, the read-only
 * GPU output view when built, and DRAM up to 0x340000000 when the decode
 * check passed. */
static uint64_t golden_l2(unsigned i)
{
    uint64_t a=UINT64_C(0x40000000)+((uint64_t)i<<21),normal=a|BIT(10)|(UINT64_C(3)<<8)|1;
    int uncached=(uintptr_t)__uncached_pool_start!=(uintptr_t)__uncached_pool_end;
    if(i==8)return normal|4|(UINT64_C(3)<<6);             /* Text: EL0/EL1 read-only, executable. */
    if(i==16)return 0;                                    /* Monitor and tables. */
    if(i==14 && uncached)return normal|8|BIT(6)|BIT(53)|BIT(54);
#if defined(A733_GPU_OUTPUT_EL0_READ_ONLY)
    if(i==54 || i==55)return normal|4|(UINT64_C(3)<<6)|BIT(53)|BIT(54);
#endif
    if(i<64)return normal|4|BIT(6)|BIT(53)|BIT(54);       /* EL0/EL1 RW, XN. */
    return decode_passes() && i<456?A733_EL1_DRAM_BLOCK(a):0;
}
static void golden_tables(void)
{
    CHECK(a733_cached_el1_l1[0]==(BIT(54)|BIT(53)|BIT(10)|(2u<<8)|1));
    CHECK(a733_cached_el1_l1[1]==((uintptr_t)a733_cached_el1_l2|3));
    /* Two ranges, level-2 first; the builder wrote exactly their entries
     * when the check passed. */
    CHECK(decode_calls==1 && decode_count==2 &&
          decode_seen[0].base==UINT64_C(0x48000000) && decode_seen[0].size==UINT64_C(0x31000000) &&
          decode_seen[1].base==UINT64_C(0x80000000) && decode_seen[1].size==A733_DRAM_END-UINT64_C(0x80000000));
    uint64_t mapped=0,valid=0;
    for(unsigned i=2;i<512;++i) {
        uint64_t pa=(uint64_t)i<<30;
        CHECK(a733_cached_el1_l1[i]==(decode_passes() && pa<A733_DRAM_END?A733_EL1_DRAM_BLOCK(pa):0));
        CHECK(!a733_cached_el3_l1[i]);
        if(a733_cached_el1_l1[i]){mapped+=1024;++valid;}
    }
    for(unsigned i=0;i<512;++i) {
        uint64_t v=a733_cached_el1_l2[i];
        CHECK(v==golden_l2(i));
        /* Every valid entry is a block descriptor, which lets one AT stand
         * for a pool's whole block; no entry is writable and executable. */
        if(v&1){CHECK((v&3)==1);mapped+=2;++valid;}
        CHECK(!(v&1) || (v&BIT(7)) || ((v&BIT(53)) && (v&BIT(54))));
        /* EL3 maps nothing above the boot image. */
        if(i>=64)CHECK(!a733_cached_el3_l2[i]);
    }
#if defined(A733_GPU_OUTPUT_EL0_READ_ONLY)
    /* EL3 keeps its read/write view of the GPU output region. */
    CHECK(!(a733_cached_el3_l2[54]&BIT(7)) && !(a733_cached_el3_l2[55]&BIT(7)));
#endif
    /* 126 MiB below 0x48000000 (the monitor block is invalid), plus
     * 0x48000000-0x79000000 and 11 GiB when the check passes. */
    CHECK(mapped==(decode_passes()?12174u:126u));
    say_hex("MEASURE el1_mapped_mib=",mapped);
    say_hex("MEASURE el1_valid_blocks=",valid);
    say_hex("MEASURE tables_bytes=",(uint64_t)(__tables_end-__tables_start));
}
void fixture_el3_prepare(unsigned core)
{
    if(core)fixture_exit(1,0xc0de); /* One core. */
    memset(__bss_start,0,(size_t)(__bss_end-__bss_start));
    CHECK(fixture_mode<=1);
    say("TRACE prepare\n");
    int prepared=a733_cached_memory_prepare();
    say("TRACE prepare end\n");
    CHECK(prepared==1);
    golden_tables();
    CHECK(a733_cpu_coherence_publish(0x41000000,0x200000,UINT32_MAX)==1);
    CHECK(a733_cpu_coherence_publish(0x41200000,(uintptr_t)__bss_end-0x41200000,UINT32_MAX)==1);
    CHECK(a733_cpu_coherence_publish((uintptr_t)__tables_start,(size_t)(__tables_end-__tables_start),UINT32_MAX)==1);
}
void fixture_el3_mapped(unsigned core)
{
    (void)core;
    uint64_t p;
    __asm__ volatile("at s1e3r,%1\n isb\n mrs %0,par_el1":"=r"(p):"r"(UINT64_C(0x80000000)):"memory");
    CHECK(p&1);
}
/* Each case: an address, whether EL1 may read and write it, and whether EL0
 * may read and write it. */
struct access{uint64_t address;unsigned el1_read,el1_write,el0_read,el0_write;};
void fixture_el1(unsigned core)
{
    (void)core;
    struct aarch64_translation t;
    int ro=0,high=decode_passes();
#if defined(A733_GPU_OUTPUT_EL0_READ_ONLY)
    ro=1;
#endif
    const struct access cases[]={
        {0x41000000,1,0,1,0},                 /* Text block: read-only at both levels. */
        {0x46c00000,1,!ro,1,!ro},             /* GPU output region. */
        {0x46fff000,1,!ro,1,!ro},
        {0x46a00000,1,1,1,1},                 /* Boot assets beside it. */
        {0x47000000,1,1,1,1},
        {0x48000000,high,high,0,0},           /* New EL1-only blocks. */
        {0x78fff000,high,high,0,0},
        {0x79000000,0,0,0,0},                 /* No owner. */
        {0x80000000,high,high,0,0},
        {0x33ffff000,high,high,0,0},
        {0x340000000,0,0,0,0},
    };
    for(unsigned i=0;i<sizeof cases/sizeof *cases;++i) {
        const struct access *c=&cases[i];
        unsigned expect[4]={c->el1_read,c->el1_write,c->el0_read,c->el0_write};
        for(unsigned k=0;k<4;++k) {
            int ok=k<2?aarch64_device_translate((uintptr_t)c->address,k&1,&t):
                       aarch64_device_translate_el0((uintptr_t)c->address,k&1,&t);
            CHECK(ok==(int)expect[k]);
            CHECK(!ok || (t.physical==c->address && t.attributes==0xff && t.shareability==3));
        }
    }
    /* The getters return NULL until the boot prepares the pools. */
    CHECK(!a733_memory_reservations_init() && !a733_memory_uncached_init());
    say("TRACE local prepare\n");
    int prepared=a733_memory_local_prepare();
    say("TRACE local prepare end\n");
    CHECK(prepared==1);
    say("TRACE getter\n");
    struct device_memory_pool *wb=a733_memory_reservations_init();
    say("TRACE getter end\n");
    struct device_memory_pool *nc=a733_memory_uncached_init();
    uintptr_t ws=(uintptr_t)__device_pool_start,we=(uintptr_t)__device_pool_end;
    uintptr_t ns=(uintptr_t)__uncached_pool_start,ne=(uintptr_t)__uncached_pool_end;
    CHECK(wb && wb->backing.cpu==ws && wb->backing.bytes==we-ws && wb->memory_type==DEVICE_MEMORY_WRITE_BACK);
    CHECK(ns==ne?!nc:nc && nc->backing.cpu==ns && nc->backing.bytes==ne-ns &&
          nc->memory_type==DEVICE_MEMORY_NON_CACHEABLE);
    /* Blocks that hold a pool byte: one write translation each. */
    say_hex("MEASURE local_blocks=",((we-1)>>21)-(ws>>21)+1+(ns==ne?0:((ne-1)>>21)-(ns>>21)+1));
    say("TRACE translate\n");
    int el1=aarch64_device_translate(0x41000000,0,&t);
    say("TRACE translate end\n");
    say("TRACE translate el0\n");
    int el0=aarch64_device_translate_el0(0x41000000,0,&t);
    say("TRACE translate el0 end\n");
    CHECK(el1 && el0);
    say("TRACE empty\n");
    say("TRACE empty end\n");
    /* A second preparation changes nothing and translates nothing. */
    CHECK(a733_memory_local_prepare()==1 && a733_memory_reservations_init()==wb);
    say("PASS local-layout fixture\n");
    fixture_exit(0,0);
}
