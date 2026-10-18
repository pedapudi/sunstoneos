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
        32*sizeof(struct device_memory_allocation)+8); /* Fixed ledger, no per-page growth. */
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
    device_memory_lookup(&q,l)->state=DEVICE_MEMORY_DEVICE;
    CHECK(!device_memory_release(&q,l) && !device_memory_acquire(&q,1,4096,&l,&e));
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
extern unsigned char __bss_start[],__bss_end[],__tables_start[],__tables_end[];
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
uint64_t a733_smp_counter(void)
{uint64_t v;__asm__ volatile("isb\n mrs %0,cntpct_el0":"=r"(v));return v;}
static void barrier(void){__asm__ volatile("dsb sy":::"memory");}
static void wait(volatile uint64_t *v)
{while(!*v){CHECK(a733_smp_counter()<UINT32_MAX);barrier();}}
void fixture_el3_prepare(unsigned core)
{
    if(core){wait(&tables_published);return;}
    memset(__bss_start,0,(size_t)(__bss_end-__bss_start));
    CHECK(fixture_mode<=2);
    CHECK(!a733_cached_memory_prepare()); /* An available range is not boot admission. */
    struct a733_smp_contract c=contract(a733_smp_counter());startup=c.startup_deadline;
    memcpy((void *)A733_SMP_CONTRACT_ADDRESS,&c,sizeof(c));
    c.reserved[3]-=4096;CHECK(!a733_device_memory_boot_admit(&c));c.reserved[3]+=4096;
    CHECK(a733_device_memory_boot_admit(&c));CHECK(!a733_device_memory_boot_admit(&c));
    CHECK(a733_cached_memory_prepare());
    CHECK(!a733_device_memory_ready() && !a733_memory_reservations_init() && !a733_memory_uncached_init());
    for(unsigned i=64;i<456;++i) {
        uint64_t common=(UINT64_C(0x40000000)+(uint64_t)i*0x200000)|
            (UINT64_C(1)<<54)|(1u<<10)|(3u<<8)|1u|(i<448?4u:8u);
        CHECK(a733_cached_el3_l2[i]==(common|(1u<<5)|(1u<<6)));
        CHECK(a733_cached_el1_l2[i]==(common|(UINT64_C(1)<<53)));
    }
    CHECK(!a733_cached_el1_l2[456] && !a733_cached_el3_l2[456]);
    if(fixture_mode==1)a733_cached_el1_l2[100]=0; /* Interior fault, before immutable publication. */
    CHECK(a733_cpu_coherence_publish(0x41000000,0x200000,UINT32_MAX)==1);
    CHECK(a733_cpu_coherence_publish(0x41200000,(uintptr_t)__bss_end-0x41200000,UINT32_MAX)==1);
    CHECK(a733_cpu_coherence_publish((uintptr_t)__tables_start,(size_t)(__tables_end-__tables_start),UINT32_MAX)==1);
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
    }
    CHECK(a733_smp_pre_scheduler(core,7,startup/24));
    if(core){for(;;)__asm__ volatile("wfe");}
    CHECK(a733_smp_device_preparation_admitted());
    unsigned calls=0;int result;
    do {
        CHECK(!a733_memory_reservations_init() && !a733_memory_uncached_init());
        result=a733_device_memory_prepare();++calls;
        if(fixture_mode==2 && calls==1)++a733_cached_runtime[0].startup_deadline;
    }while(result==0);
    if(fixture_mode) {
        CHECK(result==-1 && !a733_device_memory_ready());
        CHECK(!a733_memory_reservations_init() && !a733_memory_uncached_init());
        a733_cached_runtime[0].startup_deadline=startup;
        CHECK(a733_device_memory_prepare()==-1); /* Fault retention defeats repaired metadata. */
        CHECK(fixture_mode==1?calls>1:calls==2);
    }else {
        CHECK(result==1 && calls==(196608+4096)/64 && a733_device_memory_ready());
        struct device_memory_pool *wb=a733_memory_reservations_init(),*nc=a733_memory_uncached_init();
        CHECK(wb && nc && wb!=nc && wb->backing.cpu==0x48000000 && wb->backing.bytes==(768u<<20));
        CHECK(nc->backing.cpu==0x78000000 && nc->backing.bytes==(16u<<20));
        struct device_memory_lease l;struct device_memory_extent e;
        CHECK(device_memory_acquire_aligned(wb,1,3u*3840*2160*4,16384,&l,&e));
        CHECK(e.physical==0x48000000 && device_memory_retain(wb,l) && !device_memory_release(wb,l));
        CHECK(a733_device_memory_prepare()==1 && a733_memory_reservations_init()==wb);
    }
    fixture_exit(0,0);
}
#endif
