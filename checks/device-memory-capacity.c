#include "device_memory_pool.h"
#include "a733_device_memory_layout.h"
#include "boot.h"
#include <string.h>
const unsigned char a733_smp_build_id[32] = {0x71, 0x24, 0x83};
static struct a733_smp_contract contract(uint64_t now)
{
    struct a733_smp_contract c = {.magic=A733_SMP_CONTRACT_MAGIC,.version=2,.bytes=sizeof(c),
        .epoch=7,.generation=3,.inverse_generation=~UINT64_C(3),.armed_at=now,
        .startup_deadline=now+A733_SMP_COUNTER_HZ,.diagnostic_deadline=now+3*A733_SMP_COUNTER_HZ,
        .expires_at=now+4*A733_SMP_COUNTER_HZ,.lease=11,.watchdog_mode=0x41,
        .watchdog_configuration=1,.watchdog_output=2,.fel_flag=0x5aa5a55a,
        .redirect_flag=0xfa50392f,.redirect_entry=0x20,
        .reserved={1,13,A733_DEVICE_WB_START,A733_DEVICE_WB_END,A733_DEVICE_NC_START,A733_DEVICE_NC_END}};
    memcpy(c.build_id,a733_smp_build_id,sizeof(c.build_id));return c;
}
#ifdef CAPACITY_HOST
#include <assert.h>
#include <stdio.h>
#define CHECK(x) assert(x)
/* The pool is compiled with DEVICE_MEMORY_POOL_TRACE: count placement
 * attempts (each compares against all 32 slots), bytes cleaned and the
 * critical-section nesting. Event numbers are those of the pool source. */
enum{TRACE_ENTER,TRACE_EXIT,TRACE_ATTEMPT,TRACE_CLEAN};
static unsigned attempts,depth,max_depth;
static size_t cleaned;
void device_memory_trace(unsigned event,size_t value);
void device_memory_trace(unsigned event,size_t value)
{
    if(event==TRACE_ENTER){++depth;if(depth>max_depth)max_depth=depth;}
    else if(event==TRACE_EXIT){CHECK(depth);--depth;}
    else if(event==TRACE_ATTEMPT)++attempts;
    else if(event==TRACE_CLEAN)cleaned+=value;
}
/* Largest run of free pages. */
static unsigned largest_free(const struct device_memory_pool *p)
{
    unsigned total=(unsigned)(p->backing.bytes/4096),best=0,run=0;
    for(unsigned page=0;page<total;) {
        unsigned next=page+1,busy=0;
        for(unsigned i=0;i<32;++i){const struct device_memory_allocation *a=&p->allocations[i];
            if(a->state && page>=a->first && page<a->first+a->pages){busy=1;next=a->first+a->pages;}}
        if(busy)run=0;else{run+=1;if(run>best)best=run;}
        page=busy?next:page+1;
    }
    return best;
}
/* Device-MMU entries that map an extent with the largest page each part's
 * alignment allows; pages lists the MMU's page sizes, largest first. */
static unsigned mmu_entries(uint64_t address,uint64_t bytes,const uint64_t *pages,unsigned count)
{
    unsigned n=0;
    while(bytes){unsigned i=0;
        while(i+1<count && ((address&(pages[i]-1)) || bytes<pages[i]))++i;
        address+=pages[i];bytes-=pages[i];++n;}
    return n;
}
/* Write-back (`dma32`) admission order of each extended-layout image, from
 * the sources at this revision. Sizes are bytes before page rounding. The
 * `uncached` class minimum is 4 KiB and never raises a caller's alignment,
 * so only the `dma32` orders can change placement. */
struct order{const char *image;unsigned count;size_t bytes[8];unsigned session;};
static const struct order orders[]={
    /* NPU convolution parent (arena offset 0x104000 + 20 KiB tables), GPU
     * firmware parent (34 pages), compute payload (88 pages). */
    {"smp-cached-devices",3,{0x109000,34*4096,88*4096},3},
    {"smp-cached-devices, largest NPU layout",3,{0x316000,34*4096,88*4096},3},
    /* GPU firmware and compute parents, then the 1080p graphics session:
     * free-list backing of (1024 + 28) pages and the render payload. */
    {"gpu-render, gpu-display, displayport, dsi-scanout",4,{34*4096,88*4096,1052*4096,77824},2},
    /* Full-HD H.264 input and two output copies; the high profile keeps five
     * copies and 1 MiB of metadata. */
    {"h264 full HD",3,{4194304,3110400,3110400},3},
    {"h264 high",7,{2097152,3110400,3110400,3110400,3110400,3110400,1048576},7},
    {"hevc",3,{524288,462848,462848},3},
    {"h264-encode",3,{925696,1048576,1048576},3},
    /* Motion mode with three repeats: five input frames, four output frames. */
    {"deinterlace",2,{460800*5,460800*4},2},
    {"g2d 640x480 XRGB",2,{1228800,1228800},2},
};
static void replay(const struct order *o)
{
    static const uint64_t gpu_pages[]={2u<<20,64u<<10,4096};
    struct device_memory_pool cls={0},plain={0};
    CHECK(device_memory_pool_init_typed(&cls,0x48000000,0x48000000,768u<<20,DEVICE_MEMORY_CLASS_DMA32,
        DEVICE_MEMORY_WRITE_BACK));
    CHECK(device_memory_pool_init(&plain,0x48000000,0x48000000,768u<<20));
    struct device_memory_lease leases[8],l;struct device_memory_extent e,f;
    unsigned worst=0,entries=0,entries_4k=0;size_t first_use=0;
    long long smallest_delta=0;
    for(unsigned i=0;i<o->count;++i) {
        /* The largest free extent before each allocation, against the same
         * order with 4 KiB alignment. */
        long long delta=(long long)largest_free(&cls)-(long long)largest_free(&plain);
        if(delta<smallest_delta)smallest_delta=delta;
        attempts=0;cleaned=0;
        size_t watermark=(size_t)cls.clean_end*4096;
        CHECK(device_memory_acquire(&cls,1,o->bytes[i],&leases[i],&e)); /* No newly refused request. */
        /* Only the part below the watermark is cleaned. A page there that was
         * never handed out, such as one in an alignment gap, is cleaned too. */
        size_t offset=(size_t)(e.physical-0x48000000);
        CHECK(attempts*32<=33*32 && cleaned==(offset>=watermark?0:watermark-offset<e.bytes?watermark-offset:e.bytes));
        first_use+=cleaned;
        if(attempts*32>worst)worst=attempts*32;
        CHECK(!(e.physical&(device_memory_class_alignment(DEVICE_MEMORY_CLASS_DMA32,o->bytes[i])-1)));
        entries+=mmu_entries(e.physical,e.bytes,gpu_pages,3);entries_4k+=(unsigned)(e.bytes/4096);
        CHECK(device_memory_acquire(&plain,1,o->bytes[i],&l,&f));
    }
    /* The image's last session closes and reopens: the free extents return
     * to their state before it opened, and a reopen cleans the recycled pages
     * once. */
    unsigned before_close=largest_free(&cls),first=o->count-o->session;
    struct device_memory_extent opened[8];
    for(unsigned i=first;i<o->count;++i)CHECK(device_memory_describe(&cls,leases[i],&opened[i],NULL));
    for(unsigned i=o->count;i-->first;)CHECK(device_memory_release(&cls,leases[i]));
    size_t recycled=0;cleaned=0;
    for(unsigned i=first;i<o->count;++i){
        CHECK(device_memory_acquire(&cls,1,o->bytes[i],&leases[i],&e));
        CHECK(e.physical==opened[i].physical && e.bytes==opened[i].bytes);recycled+=e.bytes;}
    CHECK(cleaned==recycled && largest_free(&cls)==before_close);
    CHECK(!depth);
    printf("replay %s: %u extents, worst %u slot comparisons, largest free extent change %lld pages, "
        "GPU MMU entries %u (4 KiB pages %u), first use cleaned %zu bytes, reopen cleaned %zu bytes\n",
        o->image,o->count,worst,smallest_delta,entries,entries_4k,first_use,recycled);
}
/* apps/gpu-finite cleans the fixed range 0x48000000..0x48086000 at exit, which
 * covers its write-back extents (34, 88 and 12 pages) only under 4 KiB
 * placement. With the dma32 class the 88-page extent moves to 64 KiB
 * alignment and ends at 0x48088000, so the fixed range misses two pages. The
 * composition links frozen pool objects today; this records the placement a
 * rebuild against this pool would get. */
static void gpu_finite(void)
{
    static const unsigned pages[]={34,88,12};
    static const uint64_t plain_at[]={0x48000000,0x48022000,0x4807a000},class_at[]={0x48000000,0x48030000,0x48022000};
    struct device_memory_pool cls={0},plain={0};
    CHECK(device_memory_pool_init_typed(&cls,0x48000000,0x48000000,768u<<20,DEVICE_MEMORY_CLASS_DMA32,
        DEVICE_MEMORY_WRITE_BACK));
    CHECK(device_memory_pool_init(&plain,0x48000000,0x48000000,768u<<20));
    uint64_t plain_end=0,class_end=0;
    for(unsigned i=0;i<3;++i) {
        struct device_memory_lease l;struct device_memory_extent e;
        CHECK(device_memory_acquire(&plain,1,pages[i]*4096u,&l,&e) && e.physical==plain_at[i]);
        if(e.physical+e.bytes>plain_end)plain_end=e.physical+e.bytes;
        CHECK(device_memory_acquire(&cls,1,pages[i]*4096u,&l,&e) && e.physical==class_at[i]);
        if(e.physical+e.bytes>class_end)class_end=e.physical+e.bytes;
    }
    CHECK(plain_end==0x48086000 && class_end==0x48088000);
    printf("replay gpu-finite: write-back extents end at %#llx with 4 KiB placement and %#llx with dma32\n",
        (unsigned long long)plain_end,(unsigned long long)class_end);
}
/* Deliberately simple page oracle, independent of extent-jumping placement. */
static unsigned oracle(const struct device_memory_pool *p,unsigned pages,unsigned alignment)
{
    unsigned total=(unsigned)(p->backing.bytes/4096);
    for(unsigned first=0;first+pages<=total;++first) {
        if((p->backing.physical+(uint64_t)first*4096)&(alignment-1))continue;
        unsigned i;
        for(i=0;i<32;++i) {
            const struct device_memory_allocation *a=&p->allocations[i];
            if(a->state && first<a->first+a->pages && a->first<first+pages)break;
        }
        if(i==32)return first;
    }
    return UINT32_MAX;
}
int main(void)
{
    struct device_memory_pool p={0};struct device_memory_lease leases[32],l;
    struct device_memory_extent e;
    CHECK(sizeof(p)==sizeof(struct device_memory_extent)+sizeof(uint64_t)+
        32*sizeof(struct device_memory_allocation)+16); /* Fixed ledger, no per-page growth. */
    CHECK(device_memory_pool_init(&p,0x48000000,0x48000000,768u<<20));
    for(unsigned i=0;i<32;++i) {
        unsigned bytes=(i%7+1)*1024*1024+17,align=4096u<<(i%5);
        unsigned expected=oracle(&p,(bytes+4095)/4096,align);
        CHECK(expected!=UINT32_MAX);
        CHECK(device_memory_acquire_aligned(&p,i+1,bytes,align,&leases[i],&e));
        CHECK(e.physical==0x48000000+(uint64_t)expected*4096 && !(e.physical&(align-1)));
    }
    CHECK(!device_memory_acquire(&p,99,4096,&l,&e));
    for(unsigned i=0;i<32;i+=2)CHECK(device_memory_release(&p,leases[i]));
    for(unsigned i=0;i<16;++i) {
        unsigned bytes=(i%4+1)*1024*1024,align=16384;
        unsigned expected=oracle(&p,(bytes+4095)/4096,align);
        CHECK(device_memory_acquire_aligned(&p,100+i,bytes,align,&l,&e));
        CHECK(e.physical==0x48000000+(uint64_t)expected*4096);
        CHECK(device_memory_retain(&p,l) && !device_memory_release(&p,l));
    }
    CHECK(device_memory_release(&p,leases[1]));
    CHECK(!device_memory_acquire_aligned(&p,1,4096,12288,&l,&e));
    CHECK(!device_memory_acquire(&p,1,(768u<<20)+1,&l,&e));
    p.next_generation=UINT64_MAX;
    CHECK(device_memory_acquire(&p,1,4096,&l,&e) && l.generation==UINT64_MAX);
    CHECK(device_memory_release(&p,l) && !device_memory_acquire(&p,1,4096,&l,&e));
    struct device_memory_pool q={0};
    CHECK(device_memory_pool_init(&q,0x48001000,0x48001000,(768u<<20)-4096));
    CHECK(device_memory_acquire_aligned(&q,1,4096,16384,&l,&e) && e.physical==0x48004000);
    CHECK(device_memory_release(&q,l));
    CHECK(device_memory_acquire(&q,1,q.backing.bytes,&l,&e));
    device_memory_lookup(&q,l)->state=DEVICE_MEMORY_DEVICE; /* As a holder does. */
    CHECK(!device_memory_release(&q,l) && !device_memory_acquire(&q,1,4096,&l,&e));
    /* Class minimums: dma32 raises a smaller caller alignment; NONE keeps it. */
    CHECK(device_memory_class_alignment(DEVICE_MEMORY_CLASS_DMA32,2u<<20)==(2u<<20));
    CHECK(device_memory_class_alignment(DEVICE_MEMORY_CLASS_DMA32,(2u<<20)-1)==(64u<<10));
    CHECK(device_memory_class_alignment(DEVICE_MEMORY_CLASS_DMA32,(64u<<10)-1)==4096);
    CHECK(device_memory_class_alignment(DEVICE_MEMORY_CLASS_NONE,64u<<20)==4096);
    for(unsigned i=0;i<sizeof(orders)/sizeof(*orders);++i)replay(&orders[i]);
    gpu_finite();
    CHECK(max_depth==1 && !depth);
    struct a733_smp_contract c=contract(100);
    CHECK(sizeof(c)==224 && a733_smp_contract_valid(&c,100));
    c.version=1;CHECK(!a733_smp_contract_valid(&c,100));c.version=2;
    for(unsigned i=0;i<6;++i){uint64_t saved=c.reserved[i];c.reserved[i]=0;
        CHECK(!a733_smp_contract_valid(&c,100));c.reserved[i]=saved;}
    c.build_id[31]^=1;CHECK(!a733_smp_contract_valid(&c,100));c.build_id[31]^=1;
    c.lease=0;CHECK(!a733_smp_contract_valid(&c,100));c.lease=11;
    CHECK(!a733_smp_contract_valid(&c,c.startup_deadline));
    puts("capacity allocator and version-2 admission passed");return 0;
}
#else
#include "a733_cached_memory.h"
#include "a733_memory_reservations.h"
#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
#include "cached.h"
#define MONITOR __attribute__((section(".bss.a733_smp_monitor"),aligned(128)))
unsigned char fixture_el3_stacks[8*16384] MONITOR;
unsigned char fixture_el1_stacks[8*32768] __attribute__((aligned(4096)));
static volatile uint64_t tables_published MONITOR,el3_active[8] MONITOR;
static uint64_t startup;
volatile uint32_t fixture_mode=0xaabbccdd; /* Runner replaces only this fixture control. */
volatile uint8_t ucPrimaryCoreInitDoneFlag;
struct a733_cached_runtime a733_cached_runtime[8];
extern unsigned char __bss_start[],__bss_end[];
extern int a733_smp_pre_scheduler(unsigned long,uint64_t,uint64_t);
extern _Noreturn void fixture_exit(unsigned,uint64_t);
static void check(int condition,unsigned line)
{
    if(condition)return;
    char message[]="FAIL 00000000\n";
    for(unsigned i=0;i<8;++i){unsigned d=(line>>(i*4))&15;message[12-i]=(char)(d<10?'0'+d:'a'+d-10);}
    register uintptr_t operation __asm__("x0")=4,argument __asm__("x1")=(uintptr_t)message;
    __asm__ volatile("hlt #0xf000":"+r"(operation),"+r"(argument)::"memory");fixture_exit(1,line);
}
#define CHECK(x) check(!!(x),__LINE__)
static void say_hex(const char *label,uint64_t v)
{
    char m[80];unsigned n=0;
    while(label[n] && n<60){m[n]=label[n];++n;}
    for(int i=15;i>=0;--i){unsigned d=(unsigned)(v>>(i*4))&15;m[n++]=(char)(d<10?'0'+d:'a'+d-10);}
    m[n++]='\n';m[n]=0;
    register uintptr_t operation __asm__("x0")=4,argument __asm__("x1")=(uintptr_t)m;
    __asm__ volatile("hlt #0xf000":"+r"(operation),"+r"(argument)::"memory");
}
/* The fixture links no kernel. These stand-ins give the production pool
 * object the kernel critical section's effect on one core: interrupts masked,
 * nesting counted, the outer exit restoring the saved mask. */
static uint64_t critical_nesting,critical_saved;
void vTaskEnterCritical(void)
{
    uint64_t daif;__asm__ volatile("mrs %0,daif\n msr daifset,#3":"=r"(daif)::"memory");
    if(!critical_nesting++)critical_saved=daif;
}
void vTaskExitCritical(void)
{
    CHECK(critical_nesting);
    if(!--critical_nesting)__asm__ volatile("msr daif,%0"::"r"(critical_saved):"memory");
}
/* The counting image wraps the translation helper to count AT instructions;
 * the measured image does not, and there the wrapper is never called. */
static unsigned translations;
extern int __real_aarch64_device_translate(uintptr_t,int,struct aarch64_translation *) __attribute__((weak));
int __wrap_aarch64_device_translate(uintptr_t a,int write,struct aarch64_translation *t)
{++translations;return __real_aarch64_device_translate(a,write,t);}
/* PMU event 0x08, instructions architecturally executed, at EL1; QEMU counts
 * it under -icount. */
static void pmu_start(void)
{
    uint64_t event=8,enable=1,control=3;
    __asm__ volatile("msr pmevtyper0_el0,%0\n msr pmcntenset_el0,%1\n msr pmcr_el0,%2\n isb"
        ::"r"(event),"r"(enable),"r"(control):"memory");
}
static uint64_t pmu_read(void){uint64_t v;__asm__ volatile("isb\n mrs %0,pmevcntr0_el0":"=r"(v)::"memory");return v;}
#define BIT(n) (UINT64_C(1)<<(n))
/* Modes 4 and 5 pass the DRAM decode check; mode 5 and 6 stop after the
 * builder for an execution trace, with semihosting markers around the
 * builder and around the check, so the trace counts exclude the check. */
static int decode_passes(void){return fixture_mode==4 || fixture_mode==5;}
static int tracing(void){return fixture_mode==5 || fixture_mode==6;}
static struct a733_dram_range decode_seen[4];
static unsigned decode_count,decode_calls;
int a733_dram_decode_check(const struct a733_dram_range *r,unsigned n)
{
    if(tracing())say_hex("TRACE decode ",0);
    ++decode_calls;decode_count=n;
    for(unsigned i=0;i<n && i<4;++i)decode_seen[i]=r[i];
    if(tracing())say_hex("TRACE decode end ",0);
    return decode_passes();
}
/* The EL1 map of docs/memory-protection.md, entry by entry. Level-1 entry 0
 * keeps SH = 0 until the table builder sets the outer-shareable field. */
static uint64_t golden_l2(unsigned i)
{
    uint64_t a=UINT64_C(0x40000000)+((uint64_t)i<<21),normal=a|BIT(10)|(UINT64_C(3)<<8)|1;
    if(i==8)return normal|4|(UINT64_C(3)<<6);              /* Text: EL0/EL1 read-only, executable. */
    if(i==16 || i>=456)return 0;                           /* Monitor and tables; no owner. */
    if(i<64)return normal|4|BIT(6)|BIT(53)|BIT(54);        /* Image, assets: EL0/EL1 RW, XN. */
    return normal|(i<448?4:8)|BIT(53)|BIT(54);             /* dma32 WB, uncached NC: EL1 only, XN. */
}
static void golden_tables(void)
{
    CHECK(a733_cached_el1_l1[0]==(BIT(54)|BIT(53)|BIT(10)|(2u<<8)|1));
    CHECK(a733_cached_el1_l1[1]==((uintptr_t)a733_cached_el1_l2|3));
    /* The builder passed one range, 0x80000000-0x340000000, and wrote level-1
     * entries 2-12 for it only when the check passed. */
    CHECK(decode_calls==1 && decode_count==1 && decode_seen[0].base==UINT64_C(0x80000000) &&
          decode_seen[0].size==A733_DRAM_MAP_END-UINT64_C(0x80000000));
    uint64_t mapped=0,valid=0;
    for(unsigned i=2;i<512;++i) {
        uint64_t pa=(uint64_t)i<<30;
        CHECK(a733_cached_el1_l1[i]==(decode_passes() && pa<A733_DRAM_MAP_END?A733_EL1_DRAM_BLOCK(pa):0));
        if(a733_cached_el1_l1[i]){mapped+=1024;++valid;}
    }
    /* EL3 maps nothing above 0x80000000. */
    for(unsigned i=2;i<512;++i)CHECK(!a733_cached_el3_l1[i]);
    for(unsigned i=0;i<512;++i) {
        uint64_t v=a733_cached_el1_l2[i];
        CHECK(v==golden_l2(i));
        /* Every valid pool entry is a block descriptor, which lets one AT
         * stand for the whole block; no entry is writable and executable. */
        if(v&1){CHECK((v&3)==1);mapped+=2;++valid;}
        CHECK(!(v&1) || (v&BIT(7)) || ((v&BIT(53)) && (v&BIT(54))));
    }
    /* MiB of DRAM mapped at EL1: 910 below 0x80000000, plus 11 GiB above it
     * when the decode check passes. */
    CHECK(mapped==(decode_passes()?12174u:910u));
    say_hex("MEASURE el1_mapped_mib=",mapped);
    say_hex("MEASURE el1_valid_blocks=",valid);
}
uint64_t a733_smp_counter(void)
{uint64_t v;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(v));return v;}
static void barrier(void){__asm__ volatile("dsb sy":::"memory");}
static void wait(volatile uint64_t *v)
{while(!*v){CHECK(a733_smp_counter()<UINT32_MAX);barrier();}}
void fixture_el3_prepare(unsigned core)
{
    if(core){wait(&tables_published);return;}
    memset(__bss_start,0,(size_t)(__bss_end-__bss_start));
    CHECK(fixture_mode<=6);
    CHECK(!a733_cached_memory_prepare()); /* An available range is not boot admission. */
    struct a733_smp_contract c=contract(a733_smp_counter());startup=c.startup_deadline;
    memcpy((void *)A733_SMP_CONTRACT_ADDRESS,&c,sizeof(c));
    c.reserved[3]-=4096;CHECK(!a733_device_memory_boot_admit(&c));c.reserved[3]+=4096;
    CHECK(a733_device_memory_boot_admit(&c));CHECK(!a733_device_memory_boot_admit(&c));
    if(tracing())say_hex("TRACE prepare ",0);
    int prepared=a733_cached_memory_prepare();
    if(tracing()){say_hex("TRACE end ",0);CHECK(prepared);fixture_exit(0,0);}
    CHECK(prepared);
    CHECK(!a733_device_memory_ready() && !a733_memory_reservations_init() && !a733_memory_uncached_init());
    for(unsigned i=64;i<456;++i) {
        uint64_t common=(UINT64_C(0x40000000)+(uint64_t)i*0x200000)|
            (UINT64_C(1)<<54)|(1u<<10)|(3u<<8)|1u|(i<448?4u:8u);
        CHECK(a733_cached_el3_l2[i]==(common|(1u<<5)|(1u<<6)));
    }
    CHECK(!a733_cached_el3_l2[456]);
    golden_tables();
    if(fixture_mode==1){a733_cached_el1_table_sum[0]-=a733_cached_el1_l2[100];a733_cached_el1_l2[100]=0;}
    CHECK(a733_cpu_coherence_publish(0x41000000,0x200000,UINT32_MAX)==1);
    CHECK(a733_cpu_coherence_publish(0x41200000,(uintptr_t)__bss_end-0x41200000,UINT32_MAX)==1);
    barrier();tables_published=1;barrier();
}
static uint64_t translate3(uintptr_t a)
{uint64_t p;__asm__ volatile("at s1e3w,%1\n isb\n mrs %0,par_el1":"=r"(p):"r"(a):"memory");return p;}
void fixture_el3_mapped(unsigned core)
{
    barrier();el3_active[core]=1;barrier();for(unsigned i=0;i<8;++i)wait(&el3_active[i]);
    if(!core)for(uintptr_t a=0x48000000;a<0x79000000;a+=0x200000) {
        uint64_t p=translate3(a),q=translate3(a+0x1ff000);
        CHECK(!(p&1) && !(q&1) && (p>>56)==(a<0x78000000?0xffu:0x44u));
        CHECK((p&UINT64_C(0x000ffffffffff000))==a && (q&UINT64_C(0x000ffffffffff000))==a+0x1ff000);
    }
    CHECK(translate3(0x79000000)&1);CHECK(translate3(0x41000000)&1);
}
static uint64_t translate0(uintptr_t a)
{uint64_t p;__asm__ volatile("at s1e0r,%1\n isb\n mrs %0,par_el1":"=r"(p):"r"(a):"memory");return p;}
void fixture_el1(unsigned core)
{
    a733_cached_runtime[core].epoch=7;a733_cached_runtime[core].startup_deadline=startup;
    if(!core) {
        struct aarch64_translation t;
        CHECK(!aarch64_device_translate(0x42000000,0,&t) && !aarch64_device_translate(0x421f0000,0,&t));
        CHECK(!aarch64_device_translate(0x79000000,0,&t));
        for(uintptr_t a=0x48000000;a<0x79000000;a+=0x200000)CHECK(translate0(a)&1);
        /* Above 0x80000000: EL1 read/write write-back inner shareable when the
         * check passed, nothing otherwise; never EL0. */
        for(uint64_t a=UINT64_C(0x80000000);a<A733_DRAM_MAP_END;a+=UINT64_C(0x3ffff000)) {
            for(int w=0;w<2;++w) {
                CHECK(aarch64_device_translate((uintptr_t)a,w,&t)==decode_passes());
                CHECK(!decode_passes() || (t.physical==a && t.attributes==0xff && t.shareability==3));
                CHECK(!aarch64_device_translate_el0((uintptr_t)a,w,&t));
            }
        }
        CHECK(!aarch64_device_translate(A733_DRAM_MAP_END,0,&t));
    }
    CHECK(a733_smp_pre_scheduler(core,7,startup/24));
    if(core){for(;;)__asm__ volatile("wfe");}
    CHECK(a733_smp_device_preparation_admitted());
    CHECK(!a733_memory_reservations_init() && !a733_memory_uncached_init());
    if(fixture_mode==2)++a733_cached_runtime[0].startup_deadline; /* Changed admitted deadline. */
    translations=0;pmu_start();
    uint64_t before=pmu_read();
    int result=a733_device_memory_prepare();
    uint64_t spent=pmu_read()-before;
    if(fixture_mode==1 || fixture_mode==2) {
        CHECK(result==-1 && !a733_device_memory_ready());
        CHECK(!a733_memory_reservations_init() && !a733_memory_uncached_init());
        a733_cached_runtime[0].startup_deadline=startup;
        CHECK(a733_device_memory_prepare()==-1); /* Fault retention defeats repaired metadata. */
    }else if(fixture_mode==3) {
        say_hex("MEASURE prepare_translations=",translations);
        CHECK(result==1 && translations==392 && a733_device_memory_ready());
    }else {
        say_hex("MEASURE prepare_instructions=",spent);
        CHECK(result==1 && !translations && spent<=25000 && a733_device_memory_ready());
        struct device_memory_pool *wb=a733_memory_reservations_init(),*nc=a733_memory_uncached_init();
        CHECK(wb && nc && wb!=nc && wb->backing.cpu==0x48000000 && wb->backing.bytes==(768u<<20));
        CHECK(nc->backing.cpu==0x78000000 && nc->backing.bytes==(16u<<20));
        CHECK(wb->memory_class==DEVICE_MEMORY_CLASS_DMA32 && wb->memory_type==DEVICE_MEMORY_WRITE_BACK);
        CHECK(nc->memory_class==DEVICE_MEMORY_CLASS_NONE && nc->memory_type==DEVICE_MEMORY_NON_CACHEABLE);
        struct device_memory_lease l,z;struct device_memory_extent e,f,g;enum device_memory_state s;
        CHECK(device_memory_acquire_aligned(wb,1,3u*3840*2160*4,16384,&l,&e));
        CHECK(e.physical==0x48000000 && device_memory_retain(wb,l) && !device_memory_release(wb,l));
        /* A 64 KiB request in dma32 is raised to 64 KiB alignment; the
         * uncached pool keeps 4 KiB. Reacquiring a written write-back extent
         * runs the production clean at EL1; the uncached pool runs none. */
        for(unsigned pass=0;pass<2;++pass) {
            struct device_memory_pool *p=pass?nc:wb;
            CHECK(device_memory_acquire(p,2,65536,&z,&f));
            CHECK(!(f.physical&(pass?4095:65535)));
            for(size_t i=0;i<f.bytes;i+=8)*(volatile uint64_t *)(f.cpu+i)=~(uint64_t)i;
            CHECK(device_memory_release(p,z) && device_memory_acquire(p,2,65536,&z,&g));
            CHECK(g.physical==f.physical && p->clean_end*4096u>=g.physical+g.bytes-p->backing.physical);
            CHECK(device_memory_describe(p,z,&f,&s) && s==DEVICE_MEMORY_CPU && f.physical==g.physical);
            CHECK(device_memory_release(p,z) && !device_memory_describe(p,z,&f,&s) && !critical_nesting);
        }
        CHECK(a733_device_memory_prepare()==1 && a733_memory_reservations_init()==wb);
    }
    fixture_exit(0,0);
}
#endif
