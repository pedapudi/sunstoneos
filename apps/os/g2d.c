/* G2D 2D engine service (SS_DEVICE_G2D, include/sunstone_g2d.h, docs/g2d-os.md). One owner task on core 0 does every
 * register access. Open follows the vendor g2d_open order (g2d.c 569-616, 672-688 in the pinned g2d_rcq source):
 * PCK600 domain 1 through the shared power-domain owner, the G2D and DE_SYS resets deasserted, the module clock set to
 * PERI0_300M undivided with its gate off, the bus gate, the module gate, then g2d_bsp_open's local gates and resets.
 * Close gates and resets the engine in the g2d_clock_disable order (641-668); the domain stays ON. The INTID 83 route
 * is emitted for an application that declares the device; this stage runs no job, so the INTID stays disabled and its
 * handler touches no register. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "a733_clock_reference.h"
#include "a733_gpu_resources.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "a733_platform_locks.h"
#include "a733_power_domains.h"
#include "aarch64_device_memory.h"
#include "quiesce.h"
#include "sunstone_crumb.h"
#include "sunstone_deadline.h"
#include "sunstone_g2d.h"
#include <stdatomic.h>
#include <string.h>
#ifdef SUNSTONE_A733_OS_GPU
_Static_assert(OS_GPU_CLIENT==7u,"the G2D client choice skips the GPU queue's client");
#endif

#define CCU UINT32_C(0x02002000)
#define R_CCU UINT32_C(0x07010000)
#define RTC UINT32_C(0x07090000)
#define G2D UINT32_C(0x05440000)
#define PCK1 UINT32_C(0x07061000)
#define GICD UINT32_C(0x03400000)
#define G2D_INTID 83u
#define G2D_INTID_BIT (1u<<(G2D_INTID%32u))
#define G2D_OWNER UINT64_C(0x4732440000000001) /* "G2D", lease owner of the rail, clock and domain row */
#define G2D_MODULE_HZ 300000000u               /* PERI0_300M undivided: sun60iw2p1.dtsi 3383-3384 */
#define G2D_MODULE_WORD 0u                     /* CCU 0xa40 with the gate off: mux 0 (PERI0_300M), M field 0 (divide by 1) */
/* Bound of one open or close, inside the session deadline: the supply verification (5,160 + 100,000 us after provider
 * start), the domain transition (10,000 us) and the register sequence, with margin. */
#define G2D_CALL_US 500000u
enum phase {CLOSED,OPENING,OPEN,CLOSING,FAULT};
enum op {NONE,OPEN_OP,CLOSE_OP};
enum {POSTED=1,DONE=2};
/* Fault steps (ss_g2d_fault): which check refused or failed. Steps below G2D_STEP_WRITES write nothing, so a refusal
 * there leaves the engine closed. */
enum {
    G2D_STEP_NONE, G2D_STEP_RAIL, G2D_STEP_CLOCK, G2D_STEP_PCK, G2D_STEP_SUPPLY, G2D_STEP_ENTRY, G2D_STEP_DOMAIN,
    G2D_STEP_PERI0, G2D_STEP_MODULE, G2D_STEP_ENGINE, G2D_STEP_INTID, G2D_STEP_WRITES=16, G2D_STEP_ROW, G2D_STEP_ROW_TIMEOUT, G2D_STEP_LOCK,
    G2D_STEP_G2D_RESET, G2D_STEP_DE_SYS_RESET, G2D_STEP_MUX, G2D_STEP_BUS_GATE, G2D_STEP_MODULE_GATE,
    G2D_STEP_TOP, G2D_STEP_LOCAL, G2D_STEP_MODULE_WORD, G2D_STEP_CLOSE=32
};
static struct {
    struct a733_power_domain_row row;
    struct a733_rail_token rail;
    struct a733_clock_token clock;
    uint64_t pck_token;
    unsigned pck_client;
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
    atomic_uint phase,request,quiesce_wanted,quiesced;
    unsigned op;int result;
    uint32_t fault_step,fault_value;
    uint64_t limit;
    TaskHandle_t waiter;
} g2d;
static TaskHandle_t owner;
static StaticTask_t owner_tcb;
static StackType_t owner_stack[2048];

static uint64_t now_us(void){return sunstone_deadline_now_us();}
static uint64_t lease_now(void){uint64_t t=0;return a733_counter_clock_now(&providers.shared.counter,&t)==1?t:0;}
static uint64_t session_deadline(void){return board.session.deadline_us-10000;}
static int timely(void){uint64_t t=lease_now();return t&&t<g2d.limit&&board_live(NULL);}
static void pause_us(uint32_t us){const uint64_t t=now_us()+us;while(now_us()<t)(void)sunstone_deadline_wait_until(t);}
static int device(uintptr_t a)
{
    static const struct a733_mapping_rule rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_ANY}},0,1};
    return a733_mapping_check(aarch64_device_translate,a,a,&rule);
}
static int rd(uintptr_t a,uint32_t *v){if(!device(a))return 0;*v=a733_mmio_read32_dsb_sy_both(a);return 1;}
static int wr(uintptr_t a,uint32_t v){if(!device(a))return 0;a733_mmio_write32_dsb_sy_both(a,v);return 1;}
/* Read-modify-write of mask to value with readback; the caller holds the register lock for CCU words. */
static int update(uintptr_t a,uint32_t mask,uint32_t value)
{
    uint32_t v,r;
    return rd(a,&v)&&wr(a,(v&~mask)|value)&&rd(a,&r)&&r==((v&~mask)|value);
}
static int refuse(unsigned step,uint32_t value){g2d.fault_step=step;g2d.fault_value=value;return -1;}
static int fail(unsigned step,uint32_t value)
{
    if(!g2d.fault_step){g2d.fault_step=step;g2d.fault_value=value;}
    atomic_store(&g2d.phase,FAULT);return -1;
}
static void wake_owner(void *unused){(void)unused;if(owner)xTaskNotifyGive(owner);}

static const uintptr_t platform_words[SS_G2D_PLATFORM_WORDS]={
    [SS_G2D_CCU_PLL_REF]=CCU,[SS_G2D_CCU_PERI0]=CCU+0xa0,[SS_G2D_CCU_PERI0_GATES]=CCU+0x1908,
    [SS_G2D_CCU_PERI0_STATUS]=CCU+0x1988,[SS_G2D_CCU_G2D_CLK]=CCU+0xa40,[SS_G2D_CCU_G2D_BGR]=CCU+0xa44,
    [SS_G2D_CCU_DE_SYS_BGR]=CCU+0xa74,[SS_G2D_CCU_AHB_GATES]=CCU+0x5c0,[SS_G2D_CCU_MBUS_GATES]=CCU+0x5e0,
    [SS_G2D_RCCU_PCK]=R_CCU+0x1ac,[SS_G2D_PCK1_POLICY]=PCK1,[SS_G2D_PCK1_STATUS]=PCK1+8,[SS_G2D_PCK1_DELAY]=PCK1+0xc10,
    [SS_G2D_CCU_IOMMU0]=CCU+0x58c,[SS_G2D_CCU_IOMMU1]=CCU+0x5b4,[SS_G2D_RTC_OSC]=RTC+0x160,
    [SS_G2D_CCU_DE_CLK]=CCU+0xa00,[SS_G2D_CCU_DE_BGR]=CCU+0xa04,[SS_G2D_CCU_DI_CLK]=CCU+0xa20,
    [SS_G2D_CCU_DI_BGR]=CCU+0xa24,[SS_G2D_GICD_ENABLE]=GICD+0x100+4*(G2D_INTID/32),
    [SS_G2D_GICD_PENDING]=GICD+0x200+4*(G2D_INTID/32),[SS_G2D_GICD_ACTIVE]=GICD+0x300+4*(G2D_INTID/32)};
static const uint16_t local_words[SS_G2D_LOCAL_WORDS]={0x00,0x04,0x08,0x0c,0x10,0x18,0x20,0x24,0x28,0x100,0x104};
static int read_platform(uint32_t *out)
{
    for(unsigned i=0;i<SS_G2D_PLATFORM_WORDS;++i)if(!rd(platform_words[i],&out[i]))return 0;
    return 1;
}
/* PERI0 must be enabled and locked with its 300M output gated on; the image never programs the PLL. The 24 MHz
 * reference and the N and M fields give PERI0_2X = 1200 MHz, and PERI0_300M is PERI0_2X / 4 (ccu-sun60iw2.c 118-120;
 * manual 4.1.6.249 and 4.1.6.258 for the output gates). */
static int peri0_ready(const uint32_t *w)
{
    const uint32_t ref=w[SS_G2D_CCU_PLL_REF],peri=w[SS_G2D_CCU_PERI0];
    const unsigned n=((peri>>8)&255)+1,div=(((peri>>20)&7)+1)*4;
    uint32_t sdm0,sdm1;
    return a733_clock_ref24_valid(w[SS_G2D_RTC_OSC],ref)&&(peri&0xfe000002)==0xfe000000&&n>=53&&n<=105&&
        24*n==300*div&&rd(CCU+0xa8,&sdm0)&&!(sdm0&0x80000000)&&rd(CCU+0xac,&sdm1)&&!(sdm1&0x8e000000)&&
        (w[SS_G2D_CCU_PERI0_GATES]&0x0a300a30)==0x0a300a30&&(w[SS_G2D_CCU_PERI0_STATUS]&0x0a300000)==0x0a300000;
}
/* The GPU queue's fixed PCK600 client (apps/os/os.h, defined only in an image that links the queue). */
#define G2D_GPU_CLIENT 7u
/* The PCK600 provider client: one the session's board, NPU and GPU clients do not use. */
static unsigned pck_client(void)
{
    const struct os_session *s=os_session_admitted();
    for(unsigned c=A733_GPU_RESOURCE_CLIENTS;c-->0;)
        if(s&&c!=s->board.pck_client&&c!=G2D_GPU_CLIENT&&!(os_granted(OS_GRANT_NPU)&&c==s->npu.pck_client))return c;
    return A733_GPU_RESOURCE_CLIENTS;
}
static int tokens(void)
{
    while(!g2d.rail.id){
        enum a733_rail_result r=a733_rail_acquire(&providers.shared.rails,G2D_OWNER,A733_RAIL_BIT(A733_RAIL_DCDC2),0,
            session_deadline(),&g2d.rail);
        if(r==A733_RAIL_BUSY&&timely()){pause_us(50);continue;}
        if(r!=A733_RAIL_READY&&r!=A733_RAIL_PENDING)return refuse(G2D_STEP_RAIL,r);
    }
    for(;;){
        enum a733_rail_result r=a733_rail_poll(&providers.shared.rails,&g2d.rail);
        if(r==A733_RAIL_READY)break;
        if((r!=A733_RAIL_BUSY&&r!=A733_RAIL_PENDING)||!timely())return refuse(G2D_STEP_RAIL,r);
        pause_us(50);
    }
    while(!g2d.clock.id){
        enum a733_clock_result r=a733_clock_acquire(&providers.shared.clocks,G2D_OWNER,
            A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_PERI0|A733_CLOCK_FABRIC,A733_CLOCK_DE_SYS,session_deadline(),&g2d.clock);
        if(r==A733_CLOCK_BUSY&&timely()){pause_us(50);continue;}
        if(r!=A733_CLOCK_OK)return refuse(G2D_STEP_CLOCK,r);
    }
    while(!g2d.pck_token){
        g2d.pck_client=pck_client();
        if(g2d.pck_client>=A733_GPU_RESOURCE_CLIENTS)return refuse(G2D_STEP_PCK,0xff);
        struct a733_gpu_power_resources ready={0};
        enum a733_gpu_resource_result r=a733_gpu_resources_acquire(&providers.shared.gpu,g2d.pck_client,
            A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,session_deadline(),&g2d.pck_token,&ready);
        if(r==A733_GPU_RESOURCE_BUSY&&timely()){g2d.pck_token=0;pause_us(50);continue;}
        if(r==A733_GPU_RESOURCE_FAULT)return fail(G2D_STEP_PCK,r|g2d.pck_client<<8);
        if(r!=A733_GPU_RESOURCE_OK){g2d.pck_token=0;return refuse(G2D_STEP_PCK,r|g2d.pck_client<<8);}
    }
    /* The power-domain owner verifies DCDC2 enabled at 800 mV once per boot and wakes every registered row when it
     * publishes the result. The service reads only the verified bit; every other supply, clock and PPU condition is
     * the owner's admission of the row. The G2D service writes no PMIC register. */
    const uint64_t until=now_us()+G2D_CALL_US;
    for(;;){
        const unsigned w=a733_power_authority_load(&providers.shared.rails.authority);
        if(w&A733_POWER_DCDC2_VERIFIED)return 1;
        if((w&A733_POWER_SUPPLY_DONE)||now_us()>=until||!timely())return refuse(G2D_STEP_SUPPLY,w);
        (void)sunstone_deadline_wait_until(until);
    }
}
/* Domain-1 status at the request and at each row wake, read only. */
static void sample_status(void)
{
    uint32_t v=0;
    if(!rd(PCK1+8,&v))v=UINT32_MAX;
    const unsigned i=g2d.info.status_reads++;
    if(i<SS_G2D_STATUS_SAMPLES){
        g2d.info.status_word[i]=v;
        g2d.info.status_ticks[i]=(uint32_t)(ss_counter_ticks()-g2d.info.requested_ticks);
    }
}
static uint64_t owner_us(void){uint64_t t=0;return a733_pmic_now(&providers.shared.pmic,&t)==1?t:0;}
/* The owner's command time for domain 1, read after the row's ON answer has published it. */
static uint64_t command_us(void)
{
    for(unsigned i=0;i<A733_POWER_DOMAINS;++i)if(providers.domains.domain[i].id==1)return providers.domains.domain[i].command_at;
    return 0;
}
static unsigned bit8(const uint32_t *w){return (w[SS_G2D_PCK1_POLICY]>>8&1u)|(w[SS_G2D_PCK1_STATUS]>>8&1u)<<1;}
static int lock(void){return a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,g2d.limit)==1;}
static void unlock(void){a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
/* One CCU change under the shared register lock. An uncertain write keeps the lock, so no other writer proceeds. */
static int ccu(unsigned step,uintptr_t a,uint32_t mask,uint32_t value)
{
    if(!timely()||!lock())return fail(G2D_STEP_LOCK,step);
    if(!update(a,mask,value)){uint32_t v=0;(void)rd(a,&v);return fail(step,v);}
    unlock();return 1;
}
static int do_open(void)
{
    atomic_store(&g2d.phase,OPENING);
    memset(&g2d.info,0,sizeof(g2d.info));
    g2d.info.magic=SS_G2D_INFO_MAGIC;g2d.info.bytes=sizeof(g2d.info);
    g2d.info.requested_ticks=ss_counter_ticks();
    g2d.fault_step=0;g2d.fault_value=0;
    g2d.limit=lease_now()+G2D_CALL_US;
    if(g2d.limit>session_deadline())g2d.limit=session_deadline();
    if(tokens()<0)goto refused;
    /* Read-only from here to the row request: the entry words and the owner's domain-level check. */
    if(!read_platform(g2d.info.entry)){refuse(G2D_STEP_ENTRY,0);goto refused;}
    unsigned domain=0;
    if(!a733_power_domain_check(&providers.domains,1,&domain)){
        refuse(G2D_STEP_DOMAIN,g2d.info.entry[SS_G2D_PCK1_POLICY]<<16|(g2d.info.entry[SS_G2D_PCK1_STATUS]&0xffff));
        goto refused;
    }
    g2d.info.domain_entry=domain;
    if(!peri0_ready(g2d.info.entry)){refuse(G2D_STEP_PERI0,g2d.info.entry[SS_G2D_CCU_PERI0]);goto refused;}
    /* The engine starts gated and in reset, with its interrupt neither pending nor active. */
    const uint32_t bgr=g2d.info.entry[SS_G2D_CCU_G2D_BGR];
    if(bgr&0x10001u){refuse(G2D_STEP_ENGINE,bgr);goto refused;}
    const uint32_t irq=(g2d.info.entry[SS_G2D_GICD_PENDING]&G2D_INTID_BIT?1u:0u)|
        (g2d.info.entry[SS_G2D_GICD_ACTIVE]&G2D_INTID_BIT?2u:0u)|(g2d.info.entry[SS_G2D_GICD_ENABLE]&G2D_INTID_BIT?4u:0u);
    if(irq&3u){refuse(G2D_STEP_INTID,irq);goto refused;}
    /* A module clock left running must already read the 300 MHz word; its rate is never changed while gated on. */
    const uint32_t module=g2d.info.entry[SS_G2D_CCU_G2D_CLK];
    if((module&0x80000000)&&(module&0x7fffffff)!=G2D_MODULE_WORD){refuse(G2D_STEP_MODULE,module);goto refused;}
    g2d.info.pck_bit8=bit8(g2d.info.entry);
    /* Power before clocks: the vendor attaches G2D to domain 1 before its probe changes a clock or reset. */
    /* The owner answers on the row's wake, which notifies this task; the wait blocks until that or the deadline. */
    g2d.info.row_request_us=owner_us();
    if(!a733_power_domain_request(&g2d.row,1))return fail(G2D_STEP_ROW,0);
    sample_status();
    const uint64_t row_limit=now_us()+A733_POWER_PCK_BOUND_US+4000;
    for(;;){
        const enum a733_power_row_state r=a733_power_domain_state(&g2d.row);
        if(r==A733_POWER_ROW_ON)break;
        /* A refused row is uncounted and nothing was written: the engine stays closed. */
        if(r==A733_POWER_ROW_REFUSED){refuse(G2D_STEP_ROW,r);goto refused;}
        if(r!=A733_POWER_ROW_PENDING)return fail(G2D_STEP_ROW,r);
        if(now_us()>=row_limit||!timely())return fail(G2D_STEP_ROW_TIMEOUT,r);
        (void)sunstone_deadline_wait_until(row_limit);
        sample_status();
    }
    g2d.info.domain_on_ticks=ss_counter_ticks();
    g2d.info.row_on_us=owner_us();
    g2d.info.pck_command_us=command_us();
    /* g2d_clock_enable: reset (rst_bus_g2d), then the shared DE_SYS reset; the assigned 300 MHz rate is set at probe
     * with the gate off; then the bus clock and the module clock. The DE AHB and DESYS MBUS gates are critical clocks,
     * already on (checked by the domain owner). */
    if(ccu(G2D_STEP_G2D_RESET,CCU+0xa44,1u<<16,1u<<16)<0||
       ccu(G2D_STEP_DE_SYS_RESET,CCU+0xa74,1u<<16,1u<<16)<0||
       (!(module&0x80000000)&&ccu(G2D_STEP_MUX,CCU+0xa40,0x7fffffff,G2D_MODULE_WORD)<0)||
       ccu(G2D_STEP_BUS_GATE,CCU+0xa44,1u,1u)<0||
       ccu(G2D_STEP_MODULE_GATE,CCU+0xa40,0x80000000,0x80000000)<0)return -1;
    /* g2d_bsp_open: mixer and rotation sclk and hclk gates, both AHB resets released, the mclk gate. */
    if(!timely()||!update(G2D+0x00,3,3)||!update(G2D+0x04,3,3)||!update(G2D+0x08,3,3)||!update(G2D+0x18,1,1))
        return fail(G2D_STEP_TOP,0);
    for(unsigned i=0;i<SS_G2D_LOCAL_WORDS;++i)if(!rd(G2D+local_words[i],&g2d.info.local[i]))return fail(G2D_STEP_LOCAL,i);
    if(!read_platform(g2d.info.after))return fail(G2D_STEP_LOCAL,0xff);
    g2d.info.pck_bit8|=bit8(g2d.info.after)<<8;
    if(g2d.info.after[SS_G2D_CCU_G2D_CLK]!=(0x80000000u|G2D_MODULE_WORD))
        return fail(G2D_STEP_MODULE_WORD,g2d.info.after[SS_G2D_CCU_G2D_CLK]);
    g2d.info.module_hz=G2D_MODULE_HZ;
    g2d.info.opened_ticks=ss_counter_ticks();
    atomic_store(&g2d.phase,OPEN);
    return 1;
refused:
    /* Nothing was written: the engine stays closed and the leases stay held for the session. The entry words are
     * those read before the refusal, or zero for a refusal before the read. */
    atomic_store(&g2d.phase,CLOSED);
    return -1;
}
/* g2d_clock_disable: module clock, bus clock, then the G2D and the shared DE_SYS resets asserted; G2D is the only
 * DE_SYS consumer in the OS image, and the DE_SYS reset is asserted only after the DE and DI module and bus gates read
 * closed (the vendor asserts the shared reset only for its last consumer). g2d_bsp_close's local writes follow the bus gate in the vendor order and reach a
 * gated block; asserting the bus reset clears those registers, so they are not written. */
static int do_close(void)
{
    atomic_store(&g2d.phase,CLOSING);
    g2d.limit=lease_now()+G2D_CALL_US;
    if(g2d.limit>session_deadline())g2d.limit=session_deadline();
    if(!update(G2D+0x20,0x50,0))return fail(G2D_STEP_CLOSE,0);
    if(ccu(G2D_STEP_CLOSE+1,CCU+0xa40,0x80000000,0)<0||ccu(G2D_STEP_CLOSE+2,CCU+0xa44,1u,0)<0||
       ccu(G2D_STEP_CLOSE+3,CCU+0xa44,1u<<16,0)<0)return -1;
    uint32_t de_clk,de_bgr,di_clk,di_bgr;
    if(!rd(CCU+0xa00,&de_clk)||!rd(CCU+0xa04,&de_bgr)||!rd(CCU+0xa20,&di_clk)||!rd(CCU+0xa24,&di_bgr))
        return fail(G2D_STEP_CLOSE+6,0);
    g2d.closed.de_di_open=(de_clk>>31)|(de_bgr&1u)<<1|(di_clk>>31)<<2|(di_bgr&1u)<<3;
    if(!g2d.closed.de_di_open&&ccu(G2D_STEP_CLOSE+4,CCU+0xa74,1u<<16,0)<0)return -1;
    if(!rd(CCU+0xa40,&g2d.closed.g2d_clk)||!rd(CCU+0xa44,&g2d.closed.g2d_bgr)||!rd(CCU+0xa74,&g2d.closed.de_sys_bgr)||
       !rd(PCK1+8,&g2d.closed.pck1_status))return fail(G2D_STEP_CLOSE+5,0);
    g2d.closed.closed_ticks=ss_counter_ticks();
    atomic_store(&g2d.phase,CLOSED);
    return 1;
}
static void serve(void)
{
    if(atomic_load_explicit(&g2d.request,memory_order_acquire)!=POSTED)return;
    const unsigned phase=atomic_load(&g2d.phase);
    int r=-1;
    if(g2d.op==OPEN_OP)r=phase==CLOSED?do_open():phase==OPEN?1:-1;
    else if(g2d.op==CLOSE_OP)r=phase==OPEN?do_close():phase==CLOSED?1:-1;
    g2d.result=r;
    atomic_store_explicit(&g2d.request,DONE,memory_order_release);
    os_application_wake_task(g2d.waiter);
}
static void os_g2d_task(void *unused)
{
    (void)unused;
    if(sunstone_deadline_register()!=1)for(;;)vTaskSuspend(NULL);
    for(;;){
        serve();
        if(atomic_load(&g2d.quiesce_wanted)&&atomic_load(&g2d.request)!=POSTED){
            if(atomic_load(&g2d.phase)==OPEN)(void)do_close();
            if(atomic_load(&g2d.phase)==CLOSED)atomic_store(&g2d.quiesced,1);
        }
        (void)sunstone_deadline_wait_until(now_us()+10000000);
    }
}

/* The handler never runs in this stage: INTID 83 stays disabled. */
void os_g2d_irq(void){}

int os_g2d_task_privileged(void (*entry)(void *),void *context){return !context&&entry==os_g2d_task;}
int os_g2d_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
static enum os_quiesce_result quiesce_step(struct os_quiesce *q)
{
    if(atomic_load(&g2d.phase)==FAULT){
        (void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|0x931,
            g2d.fault_step<<24|(g2d.fault_value&0xffffff),SUNSTONE_CRUMB_HERE());
        return OS_QUIESCE_FAULT;
    }
    if(atomic_load(&g2d.quiesced))return OS_QUIESCE_DONE;
    atomic_store(&g2d.quiesce_wanted,1);
    wake_owner(NULL);
    return OS_QUIESCE_PENDING;
}
static struct os_quiesce g2d_quiesce={.owner=OS_QUIESCE_G2D,.order=OS_QUIESCE_DEVICES,.step=quiesce_step};
int os_g2d_start(void)
{
    const struct ss_grant *grant=ss_grant();
    if(!grant||!(grant->devices&SS_DEVICE_G2D))return 1;
    if(!os_session_admitted()||!os_quiesce_register(&g2d_quiesce))return 0;
    /* The row is registered before the power service first runs; the rail token is acquired by the owner task before
     * the first request. */
    if(!a733_power_domain_row_init(&g2d.row,&providers.domains,1,G2D_OWNER,&g2d.rail))return 0;
    g2d.row.wake=wake_owner;
    owner=xTaskCreateStaticAffinitySet(os_g2d_task,"os-g2d",sizeof(owner_stack)/sizeof(owner_stack[0]),NULL,4,
        owner_stack,&owner_tcb,1);
    return owner!=NULL;
}

/* --- application calls ---------------------------------------------------------------------------------------------- */
static int granted(void){const struct ss_grant *g=ss_grant();return g&&(g->devices&SS_DEVICE_G2D)&&owner;}
static int call(unsigned op)
{
    if(!granted()||atomic_load(&g2d.quiesce_wanted))return -1;
    const unsigned state=atomic_load_explicit(&g2d.request,memory_order_acquire);
    if(state==DONE){
        if(g2d.op!=op)return -1;
        const int r=g2d.result;
        atomic_store_explicit(&g2d.request,0,memory_order_release);
        return r;
    }
    if(state==POSTED)return g2d.op==op?0:-1;
    g2d.op=op;g2d.waiter=xTaskGetCurrentTaskHandle();
    atomic_store_explicit(&g2d.request,POSTED,memory_order_release);
    xTaskNotifyGive(owner);
    return 0;
}
int ss_g2d_open(struct ss_g2d_info *info)
{
    const int r=call(OPEN_OP);
    if(r!=0&&info&&g2d.info.magic==SS_G2D_INFO_MAGIC)*info=g2d.info;
    return r;
}
int ss_g2d_close(struct ss_g2d_closed *closed){const int r=call(CLOSE_OP);if(r==1&&closed)*closed=g2d.closed;return r;}
int ss_g2d_fault(uint32_t *step,uint32_t *value)
{
    if(!granted())return -1;
    if(step)*step=g2d.fault_step;
    if(value)*value=g2d.fault_value;
    return g2d.fault_step?1:0;
}
static int ready(void){return atomic_load(&g2d.request)==DONE||atomic_load(&g2d.phase)==FAULT;}
void ss_g2d_wait(uint32_t wait_us)
{
    const uint64_t until=now_us()+(wait_us?wait_us:1);
    if(os_application_wait_unless(until,ready)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x75a);
}
