#include "boot.h"
#define REQUEST ((volatile const uint64_t *)0x52c00)
#define REPORT ((volatile uint64_t *)0x52d00)
#define CONTRACT ((volatile struct a733_smp_contract *)A733_SMP_CONTRACT_ADDRESS)
#define REQUEST_MAGIC UINT64_C(0x4153373353554c33)
#define LIMIT UINT64_C(24000)
extern _Noreturn void a733_smp_launch_park(void);
static uint32_t rd(uint32_t address) {uint32_t v=*(volatile uint32_t *)(uintptr_t)address;__asm__ volatile("dsb sy":::"memory");return v;}
static uint64_t ticks(void)
{
    uint32_t lo,hi;__asm__ volatile("isb\n mrrc p15,0,%0,%1,c14":"=r"(lo),"=r"(hi)::"memory");
    return ((uint64_t)hi<<32)|lo;
}
static void require(int ok,uint64_t reason)
{if(!ok) {REPORT[0]=reason;__asm__ volatile("dsb sy":::"memory");a733_smp_launch_park();}}
static void within(uint64_t start)
{uint64_t now=ticks();require(now>=start && now-start<LIMIT,9);}
static __attribute__((noinline)) void refused(uint64_t reason)
{
    if(rd(0x02050014)&1)a733_smp_launch_park();
    REPORT[3]=REQUEST[3];REPORT[4]=REQUEST[4];REPORT[5]=REQUEST[6];
    REPORT[0]=reason;
    __asm__ volatile("dsb sy":::"memory");
    REPORT[6]=UINT64_C(0x4153373352454631);
    __asm__ volatile("dsb sy":::"memory");
}
#define ADMIT(ok, reason) do {if(!(ok)){refused(reason);return 0;}} while(0)
int a733_smp_launch_prepare(void)
{
    uint32_t frequency;__asm__ volatile("mrc p15,0,%0,c14,c0,0":"=r"(frequency));
    /* ROM may leave the architectural frequency declaration uninitialized.
     * Raw ticks still use the board's source-established 24 MHz counter; native
     * startup writes CNTFRQ. Reject an incompatible nonzero declaration. */
    ADMIT(!frequency || frequency==A733_SMP_COUNTER_HZ,13);
    /* Request words: magic,version,bytes,epoch,generation,inverse,lease,
     * build-id PA, four build-id words, four admission-digest words. */
    ADMIT(REQUEST[0]==REQUEST_MAGIC && REQUEST[1]==3 && REQUEST[2]==128,1);
    uint64_t epoch=REQUEST[3],generation=REQUEST[4],lease=REQUEST[6],id=REQUEST[7];
    ADMIT(epoch && generation && REQUEST[5]==~generation && lease &&
        id>=0x41000000 && id<=0x411fffe0,2);
    ADMIT(CONTRACT->magic==0 && CONTRACT->version==3 && CONTRACT->bytes==sizeof(*CONTRACT) &&
        CONTRACT->epoch==epoch && CONTRACT->generation==generation &&
        CONTRACT->inverse_generation==~generation && CONTRACT->lease==lease &&
        !CONTRACT->armed_at && !CONTRACT->startup_deadline && !CONTRACT->diagnostic_deadline &&
        !CONTRACT->expires_at && !CONTRACT->watchdog_mode &&
        !CONTRACT->watchdog_configuration && !CONTRACT->watchdog_output &&
        !CONTRACT->fel_flag && !CONTRACT->redirect_flag &&
        !CONTRACT->redirect_entry && !CONTRACT->alternate_flag &&
        CONTRACT->reserved[0] && !CONTRACT->reserved[1],3);
    uint64_t digest=0;
    for(unsigned i=0;i<4;++i){ADMIT(REQUEST[12+i]==CONTRACT->reserved[2+i],3);digest|=REQUEST[12+i];}
    ADMIT(digest,3);
    volatile const uint8_t *actual=(const volatile uint8_t *)(uintptr_t)id;
    volatile const uint8_t *request_id=(const volatile uint8_t *)&REQUEST[8];
    for(unsigned i=0;i<32;++i)ADMIT(actual[i]==request_id[i] && CONTRACT->build_id[i]==request_id[i],4);
    ADMIT(!(rd(0x02050014)&1) && !(rd(0x02050010)&0x100) &&
        !rd(0x070501c0) && !rd(0x070501d4),6);
    /* No watchdog, FEL flag or reset-redirect writes in supervised mode. */
    uint64_t start=ticks();require(start<=UINT64_MAX-20*A733_SMP_COUNTER_HZ,8);
    REPORT[1]=start;REPORT[2]=ticks();
    CONTRACT->armed_at=start;
    CONTRACT->startup_deadline=start+A733_SMP_COUNTER_HZ;
    CONTRACT->diagnostic_deadline=start+20*A733_SMP_COUNTER_HZ;
    CONTRACT->expires_at=start+20*A733_SMP_COUNTER_HZ;
    __asm__ volatile("dsb sy":::"memory");
    CONTRACT->magic=A733_SMP_CONTRACT_MAGIC;
    __asm__ volatile("dsb sy":::"memory");
    within(start);
    return 1;
}
void a733_smp_launch_last_check(void)
{
    require(rd(0x08001004)==0x41000000 && !rd(0x08001008),11);
    within(REPORT[1]);REPORT[0]=12;
    __asm__ volatile("dsb sy":::"memory");
}
