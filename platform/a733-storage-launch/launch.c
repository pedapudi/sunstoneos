/* One storage session; runtime renewal belongs to its admitted supervisor. */
#include "boot.h"
#include "session.h"
#define REQUEST ((volatile const uint64_t *)0x54000)
#define REPORT ((volatile uint64_t *)0x54100)
#define CONTRACT ((volatile struct a733_smp_contract *)A733_SMP_CONTRACT_ADDRESS)
#ifdef A733_MIMUS_REQUEST_MAGIC
#define REQUEST_MAGIC A733_MIMUS_REQUEST_MAGIC
#else
#define REQUEST_MAGIC UINT64_C(0x4153373353544c31)
#endif
#define SESSION ((uint8_t *)(uintptr_t)A733_STORAGE_SESSION_ADDRESS)
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
    ADMIT(frequency==A733_SMP_COUNTER_HZ,13);
    /* Request words: magic,version,bytes,epoch,generation,inverse,lease,
     * build-id PA, four build-id words, session PA, size, two zero words. */
    ADMIT(REQUEST[0]==REQUEST_MAGIC && REQUEST[1]==2 && REQUEST[2]==128,1);
    uint64_t epoch=REQUEST[3],generation=REQUEST[4],lease=REQUEST[6],id=REQUEST[7];
    ADMIT(epoch && generation && REQUEST[5]==~generation && lease &&
        id>=0x41000000 && id<=0x411fffe0,2);
    ADMIT(CONTRACT->magic==0 && CONTRACT->version==1 && CONTRACT->bytes==sizeof(*CONTRACT) &&
        CONTRACT->epoch==epoch && CONTRACT->generation==generation &&
        CONTRACT->inverse_generation==~generation && CONTRACT->lease==lease &&
        !CONTRACT->armed_at && !CONTRACT->startup_deadline && !CONTRACT->diagnostic_deadline &&
        !CONTRACT->expires_at && CONTRACT->watchdog_mode==0x41 &&
        CONTRACT->watchdog_configuration==1 && CONTRACT->watchdog_output==2 &&
        CONTRACT->fel_flag==0x5aa5a55a && CONTRACT->redirect_flag==0xfa50392f &&
        CONTRACT->redirect_entry==0x20 && !CONTRACT->alternate_flag,3);
    ADMIT(REQUEST[12]==A733_STORAGE_SESSION_ADDRESS && REQUEST[13]==A733_STORAGE_SESSION_BYTES &&
        !REQUEST[14] && !REQUEST[15],3);
    ADMIT(CONTRACT->reserved[0]==REQUEST[12] && CONTRACT->reserved[1]==REQUEST[13],3);
    ADMIT(!a733_storage_wire64(SESSION) && a733_storage_wire64(SESSION+8)==1 &&
        a733_storage_wire64(SESSION+16)==A733_STORAGE_SESSION_BYTES &&
        a733_storage_wire64(SESSION+24)==epoch && a733_storage_wire64(SESSION+32)==generation &&
        a733_storage_wire64(SESSION+40)==lease && !a733_storage_wire64(SESSION+640) &&
        !a733_storage_wire64(SESSION+648) && a733_storage_wire_bounds(SESSION,ticks()),3);
#ifdef A733_MIMUS_REQUEST_MAGIC
    ADMIT(a733_mimus_wire_valid(SESSION),3);
    unsigned image_bits=0,recovery_bits=0;
    for(unsigned i=0;i<32;++i) {
        image_bits|=SESSION[48+i];recovery_bits|=SESSION[80+i];
        ADMIT(((const volatile uint8_t *)&CONTRACT->reserved[2])[i]==SESSION[184+i],3);
    }
    ADMIT(image_bits&&recovery_bits,3);
#else
    ADMIT(a733_storage_wire32(SESSION+160)==0x53554731 && a733_storage_wire32(SESSION+164)==1,3);
    unsigned image_bits=0,recovery_bits=0,nonce_bits=0;
    for(unsigned i=0;i<32;++i) {
        image_bits|=SESSION[48+i];recovery_bits|=SESSION[80+i];nonce_bits|=SESSION[184+i];
        ADMIT(SESSION[80+i]==SESSION[360+i] &&
            ((const volatile uint8_t *)&CONTRACT->reserved[2])[i]==SESSION[184+i],3);
    }
    ADMIT(image_bits && recovery_bits && nonce_bits,3);
#endif
    ADMIT(rd(0x07090000)==a733_storage_wire32(SESSION+144) &&
        rd(0x07090060)==a733_storage_wire32(SESSION+148) &&
        rd(0x07090160)==a733_storage_wire32(SESSION+152),3);
    volatile const uint8_t *actual=(const volatile uint8_t *)(uintptr_t)id;
    volatile const uint8_t *request_id=(const volatile uint8_t *)&REQUEST[8];
    for(unsigned i=0;i<32;++i)ADMIT(actual[i]==request_id[i] && CONTRACT->build_id[i]==request_id[i],4);
    for(unsigned i=0;i<4;++i)ADMIT(!(rd(0x03000024)&0x100),5);
    ADMIT(!(rd(0x02050014)&1) && !(rd(0x02050010)&0x100) &&
        !rd(0x070501c0) && !rd(0x070501d4),6);
    /* Exclusive ownership and source qualification are prerequisites. Keys and
     * fixed fields follow the existing watchdog and ROM-return adapters. */
    wr(0x070501c4,0x20);require(rd(0x070501c4)==0x20,7);
    wr(0x070501c0,0xfa50392f);wr(0x07090108,0x5aa5a55a);
    wr(0x02050018,0x16aa0002);wr(0x02050010,0x16aa0001);
    require(rd(0x070501c0)==0xfa50392f && rd(0x07090108)==0x5aa5a55a &&
        (rd(0x02050018)&0xffff)==2 && (rd(0x02050010)&0xffff)==1 &&
        !(rd(0x02050014)&1) && !(rd(0x03000024)&0x100),7);
    uint64_t start=ticks();require(start<=UINT64_MAX-A733_SMP_COUNTER_HZ && a733_storage_wire_bounds(SESSION,start),8);
    REPORT[1]=start;
    wr(0x02050014,0x16aa0041);wr(0x0205000c,0x14af);
    require((rd(0x02050014)&0xffff)==0x41 && (rd(0x02050010)&0xffff)==1 &&
        (rd(0x02050018)&0xffff)==2,8);
    within(start);uint64_t after=ticks();REPORT[2]=after;
    require(after>=start && after-start<LIMIT,9);
    *(uint64_t *)(SESSION+640)=start;*(uint64_t *)(SESSION+648)=after;
    require(rd(0x07090000)==a733_storage_wire32(SESSION+144) &&
        rd(0x07090060)==a733_storage_wire32(SESSION+148) &&
        rd(0x07090160)==a733_storage_wire32(SESSION+152),9);
    CONTRACT->armed_at=start;
    CONTRACT->startup_deadline=start+A733_SMP_COUNTER_HZ;
    CONTRACT->diagnostic_deadline=a733_storage_wire64(SESSION+256)*24;
    CONTRACT->expires_at=a733_storage_wire64(SESSION+136)*24;
    __asm__ volatile("dsb sy":::"memory");
    *(uint64_t *)SESSION=A733_STORAGE_SESSION_MAGIC;
    __asm__ volatile("dsb sy":::"memory");
    CONTRACT->magic=A733_STORAGE_CONTRACT_MAGIC;
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
