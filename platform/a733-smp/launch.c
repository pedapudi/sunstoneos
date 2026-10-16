#include "boot.h"
#define REQUEST ((volatile const uint64_t *)0x52800)
#define REPORT ((volatile uint64_t *)0x52900)
#define CONTRACT ((volatile struct a733_smp_contract *)A733_SMP_CONTRACT_ADDRESS)
#define REQUEST_MAGIC UINT64_C(0x415337334c4e4331)
#define LIMIT UINT64_C(24000)
extern _Noreturn void a733_smp_launch_park(void);
static uint32_t rd(uint32_t address) {uint32_t v=*(volatile uint32_t *)(uintptr_t)address;__asm__ volatile("dsb sy":::"memory");return v;}
static void wr(uint32_t address,uint32_t value) {*(volatile uint32_t *)(uintptr_t)address=value;__asm__ volatile("dsb sy":::"memory");}
static uint64_t ticks(void)
{
    uint32_t lo,hi;__asm__ volatile("isb\n mrrc p15,0,%0,%1,c14":"=r"(lo),"=r"(hi)::"memory");
    return ((uint64_t)hi<<32)|lo;
}
static void require(int ok,uint64_t reason)
{if(!ok) {REPORT[0]=reason;__asm__ volatile("dsb sy":::"memory");a733_smp_launch_park();}}
static void within(uint64_t start)
{uint64_t now=ticks();require(now>=start && now-start<LIMIT,9);}
void a733_smp_launch_prepare(void)
{
    /* Request words: magic,version,bytes,epoch,generation,inverse,lease,
     * build-id PA, four build-id words, four zero reserved words. */
    require(REQUEST[0]==REQUEST_MAGIC && REQUEST[1]==1 && REQUEST[2]==128,1);
    uint64_t epoch=REQUEST[3],generation=REQUEST[4],lease=REQUEST[6],id=REQUEST[7];
    require(epoch && generation && REQUEST[5]==~generation && lease &&
        id>=0x41000000 && id<=0x411fffe0,2);
    require(CONTRACT->magic==0 && CONTRACT->version==1 && CONTRACT->bytes==sizeof(*CONTRACT) &&
        CONTRACT->epoch==epoch && CONTRACT->generation==generation &&
        CONTRACT->inverse_generation==~generation && CONTRACT->lease==lease &&
        !CONTRACT->armed_at && !CONTRACT->startup_deadline && !CONTRACT->diagnostic_deadline &&
        !CONTRACT->expires_at && CONTRACT->watchdog_mode==0x41 &&
        CONTRACT->watchdog_configuration==1 && CONTRACT->watchdog_output==2 &&
        CONTRACT->fel_flag==0x5aa5a55a && CONTRACT->redirect_flag==0xfa50392f &&
        CONTRACT->redirect_entry==0x20 && !CONTRACT->alternate_flag,3);
    for(unsigned i=0;i<4;++i)require(!REQUEST[12+i],3);
    for(unsigned i=0;i<6;++i)require(!CONTRACT->reserved[i],3);
    volatile const uint8_t *actual=(const volatile uint8_t *)(uintptr_t)id;
    volatile const uint8_t *request_id=(const volatile uint8_t *)&REQUEST[8];
    for(unsigned i=0;i<32;++i)require(actual[i]==request_id[i] && CONTRACT->build_id[i]==request_id[i],4);
    for(unsigned i=0;i<4;++i)require(!(rd(0x03000024)&0x100),5);
    require(!(rd(0x02050014)&1) && !(rd(0x02050010)&0x100) &&
        !rd(0x070501c0) && !rd(0x070501d4),6);
    /* Exclusive ownership and source qualification are prerequisites. Keys and
     * fixed fields follow the existing watchdog and ROM-return adapters. */
    wr(0x070501c4,0x20);require(rd(0x070501c4)==0x20,7);
    wr(0x070501c0,0xfa50392f);wr(0x07090108,0x5aa5a55a);
    wr(0x02050018,0x16aa0002);wr(0x02050010,0x16aa0001);
    require(rd(0x070501c0)==0xfa50392f && rd(0x07090108)==0x5aa5a55a &&
        (rd(0x02050018)&0xffff)==2 && (rd(0x02050010)&0xffff)==1 &&
        !(rd(0x02050014)&1) && !(rd(0x03000024)&0x100),7);
    uint64_t start=ticks();require(start<=UINT64_MAX-4*A733_SMP_COUNTER_HZ,8);
    REPORT[1]=start;
    wr(0x02050014,0x16aa0041);wr(0x0205000c,0x14af);
    require((rd(0x02050014)&0xffff)==0x41 && (rd(0x02050010)&0xffff)==1 &&
        (rd(0x02050018)&0xffff)==2,8);
    within(start);REPORT[2]=ticks();
    CONTRACT->armed_at=start;
    CONTRACT->startup_deadline=start+A733_SMP_COUNTER_HZ;
    CONTRACT->diagnostic_deadline=start+2*A733_SMP_COUNTER_HZ;
    CONTRACT->expires_at=start+4*A733_SMP_COUNTER_HZ;
    __asm__ volatile("dsb sy":::"memory");
    CONTRACT->magic=A733_SMP_CONTRACT_MAGIC;
    __asm__ volatile("dsb sy":::"memory");
    within(start);
}
void a733_smp_launch_last_check(void)
{
    require(rd(0x08001004)==0x41000000 && !rd(0x08001008),11);
    within(REPORT[1]);REPORT[0]=12;
    __asm__ volatile("dsb sy":::"memory");
}
