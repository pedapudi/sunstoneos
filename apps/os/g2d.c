/* G2D 2D engine service (SS_DEVICE_G2D, include/sunstone_g2d.h, docs/g2d-os.md). One owner task on core 0 does every
 * register access. Open follows the vendor g2d_open order (g2d.c 569-616, 672-688 in the pinned g2d_rcq source):
 * PCK600 domain 1 through the shared power-domain owner, the G2D and DE_SYS resets deasserted, the module clock set to
 * PERI0_300M undivided with its gate off, the bus gate, the module gate, then g2d_bsp_open's local gates and resets.
 * Close gates and resets the engine in the g2d_clock_disable order (641-668); the domain stays ON.
 * An operation follows g2d_mixer_apply (g2d_mixer.c 996-1030): the RCQ command page built by a733_g2d_commands_at
 * (a fill or a blit) or a733_g2d_convert_commands (a convert), the RCQ header address and length, the task-end interrupt enable, then the update bit. INTID 83 is enabled from the
 * open to the close; its handler acknowledges task end, pulses the mixer AHB reset and wakes the owner, in the order of
 * g2d_handle_irq (g2d.c 802-812). The engine's buffers are the application's large regions, mapped for master 10
 * through the OS image's IOMMU owner (apps/os/iommu.h) at the first operation after an open and unmapped at the close. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "a733_clock_reference.h"
#include "a733_g2d.h"
#include "a733_g2d_convert.h"
#include "a733_g2d_irq.h"
#include "a733_g2d_rotate.h"
#include "app_large.h"
#include "gicv3_spi.h"
#include "iommu.h"
#include "owner-memory.h"
#include "a733_gpu_resources.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "a733_platform_locks.h"
#include "a733_power_domains.h"
#include "aarch64_device_memory.h"
#include "device-fault.h"
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
/* Bound of one operation from the update bit to the task-end interrupt: the vendor's WAIT_CMD_TIME_MS for one frame
 * (g2d_driver_i.h 42-44). */
#define G2D_JOB_US 100000u
#define G2D_MASTER 10u
#define G2D_RCQ_BYTES 4096u
#define G2D_RCQ_HEADER_BYTES 128u /* eight 16-byte block headers, the last the zero-dirty terminator */
enum op {NONE,OPEN_OP,CLOSE_OP,RUN_OP};
enum {POSTED=1,DONE=2};
/* Fault steps (ss_g2d_fault): which check refused or failed. Steps below G2D_STEP_WRITES write nothing, so a refusal
 * there leaves the engine closed. */
enum {
    G2D_STEP_NONE, G2D_STEP_RAIL, G2D_STEP_CLOCK, G2D_STEP_PCK, G2D_STEP_SUPPLY, G2D_STEP_ENTRY, G2D_STEP_DOMAIN,
    G2D_STEP_PERI0, G2D_STEP_MODULE, G2D_STEP_ENGINE, G2D_STEP_INTID, G2D_STEP_WRITES=16, G2D_STEP_ROW, G2D_STEP_ROW_TIMEOUT, G2D_STEP_LOCK,
    G2D_STEP_G2D_RESET, G2D_STEP_DE_SYS_RESET, G2D_STEP_MUX, G2D_STEP_BUS_GATE, G2D_STEP_MODULE_GATE,
    G2D_STEP_TOP, G2D_STEP_LOCAL, G2D_STEP_MODULE_WORD, G2D_STEP_GIC, G2D_STEP_CLOSE=32,
    /* An operation. Refusals below G2D_STEP_RUN_WRITES change nothing. A failure that leaves the engine idle and the
     * IOMMU consistent (the command build and the CPU cache maintenance) is also a refusal: the service stays open
     * and its close detaches master 10. A failure that may leave the engine running or the IOMMU or an engine
     * register in an unknown state makes the service FAULT, which retains its gates, leases and mapping. */
    G2D_STEP_RUN=48, G2D_STEP_RUN_SURFACE, G2D_STEP_RUN_GEOMETRY, G2D_STEP_RUN_MEMORY, G2D_STEP_RUN_IOVA,
    G2D_STEP_RUN_WRITES=56, G2D_STEP_RUN_ATTACH, G2D_STEP_RUN_MAP, G2D_STEP_RUN_CACHE, G2D_STEP_RUN_BUSY,
    G2D_STEP_RUN_LAUNCH, G2D_STEP_RUN_TIMEOUT, G2D_STEP_RUN_IRQ, G2D_STEP_RUN_FINISH, G2D_STEP_RUN_IOMMU_FAULT,
    G2D_STEP_RUN_IOMMU, G2D_STEP_CLOSE_GIC=72, G2D_STEP_CLOSE_DETACH, G2D_STEP_CLOSE_STOP, G2D_STEP_CLOSE_RESET,
    G2D_STEP_CLOSE_IDLE
};
static TaskHandle_t owner;
static struct {
    struct a733_power_domain_row row;
    struct a733_rail_token rail;
    struct a733_clock_token clock;
    uint64_t pck_token;
    unsigned pck_client;
    struct ss_g2d_info info;
    struct ss_g2d_closed closed;
    /* The phase and the fault report (apps/os/device-fault.h). The fault must be reported once a close step failed. */
    struct os_device_fault dev;
    atomic_uint request;
    unsigned op;int result;
    uint64_t limit;
    TaskHandle_t waiter;
    /* Operations. job and done belong to the owner task while a RUN_OP request is posted. */
    struct ss_g2d_job job;
    struct ss_g2d_done done;
    struct a733_g2d_session commands;
    struct gicv3_spi gic;
    uint8_t *rcq;
    uint64_t rcq_iova;
    /* The large regions mapped for master 10: one per region an operation names (a job names its output and, for a
     * blit, its source), each with the access its role needs. region is the index + 1, 0 for an unused entry. */
    struct {uint32_t region;struct a733_iommu_span span;} maps[2];
    unsigned attached;
    /* Written by the interrupt handler, read by the owner after the wake. armed is the kind of the operation in
     * flight (A733_G2D_IRQ_MIXER or A733_G2D_IRQ_ROTATE), 0 for none. stray_finishes counts finishes of the other
     * block during it, and stray_word keeps the first one's status word. */
    atomic_uint armed,irq_seen,irq_count,stray,stray_finishes;
    uint32_t irq_status,irq_reset,stray_word;
    uint64_t irq_ticks;
} g2d={.dev={.site=0x931,.owner=&owner}};
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
static int refuse(unsigned step,uint32_t value){return os_device_refuse(&g2d.dev,step,value);}
static int fail(unsigned step,uint32_t value){return os_device_fail(&g2d.dev,step,value,0);}
static void wake_owner(void *unused){(void)unused;if(owner)xTaskNotifyGive(owner);}
static uint64_t owner_us(void){uint64_t t=0;return a733_pmic_now(&providers.shared.pmic,&t)==1?t:0;}
static unsigned settled_supply;

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
     * publishes the result, within its supply deadline (include/a733_power_domains.h). The service reads only the
     * verified bit; every other supply, clock and PPU condition is the owner's admission of the row. The G2D service
     * writes no PMIC register. */
    const uint64_t until=a733_power_supply_deadline(&providers.domains,g2d.limit);
    for(;;){
        const unsigned w=a733_power_authority_load(&providers.shared.rails.authority);
        if(w&A733_POWER_DCDC2_VERIFIED){
            g2d.info.verified_seen_us=owner_us();g2d.info.verify_at_us=providers.domains.verify_at;return 1;
        }
        if((w&A733_POWER_SUPPLY_DONE)||now_us()>=until||!timely())return refuse(G2D_STEP_SUPPLY,w);
        (void)sunstone_deadline_wait_until(until);
    }
}
/* The transition record that the owner published with the row's answer; read after the answer is seen. */
#define SAME(m) offsetof(struct ss_g2d_transition,m)==offsetof(struct a733_power_transition,m)
_Static_assert(sizeof(struct ss_g2d_transition)==sizeof(struct a733_power_transition)&&SAME(policy_before)&&
    SAME(status_before)&&SAME(policy_written)&&SAME(first_word)&&SAME(pending_word)&&SAME(reached_word)&&SAME(all_and)&&
    SAME(all_or)&&SAME(reads)&&SAME(spin_reads)&&SAME(commands)&&SAME(command_at)&&SAME(first_at)&&SAME(pending_at)&&
    SAME(reached_at),"struct ss_g2d_transition mirrors struct a733_power_transition");
#undef SAME
static void copy_transition(void){memcpy(&g2d.info.transition,&g2d.row.transition,sizeof(g2d.info.transition));}
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
    os_device_open_begin(&g2d.dev);
    memset(&g2d.info,0,sizeof(g2d.info));
    g2d.info.magic=SS_G2D_INFO_MAGIC;g2d.info.bytes=sizeof(g2d.info);
    g2d.info.requested_ticks=ss_counter_ticks();
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
    /* Verification has ended (above), so the owner admits the row at its next step. */
    const uint64_t row_limit=a733_power_row_deadline(&providers.domains,now_us(),g2d.limit);
    for(;;){
        const enum a733_power_row_state r=a733_power_domain_state(&g2d.row);
        if(r==A733_POWER_ROW_ON)break;
        /* A refused row is uncounted and nothing was written: the engine stays closed. The owner made no transition
         * for this request, so the record's transition stays zero rather than carrying the domain's previous one. */
        if(r==A733_POWER_ROW_REFUSED){refuse(G2D_STEP_ROW,r);goto refused;}
        if(r!=A733_POWER_ROW_PENDING)return fail(G2D_STEP_ROW,r);
        if(now_us()>=row_limit||!timely())return fail(G2D_STEP_ROW_TIMEOUT,r);
        (void)sunstone_deadline_wait_until(row_limit);
    }
    g2d.info.domain_on_ticks=ss_counter_ticks();
    g2d.info.row_on_us=owner_us();
    copy_transition();
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
    g2d.info.settled_supply=settled_supply;
    /* INTID 83 enabled for the open engine: its task-end interrupt enable (G2D 0x20 bit 4) stays clear until an
     * operation starts, and the handler does nothing unless an operation armed it. */
    {
        int r=0;
        for(unsigned i=0;i<100&&!r;++i){r=gicv3_spi_rearm(&g2d.gic);if(!r)pause_us(10);}
        if(r!=1)return fail(G2D_STEP_GIC,(uint32_t)r);
    }
    g2d.info.opened_ticks=ss_counter_ticks();
    atomic_store(&g2d.dev.phase,OS_DEVICE_OPEN);
    return 1;
refused:
    /* Nothing was written: the engine stays closed and the leases stay held for the session. The entry words are
     * those read before the refusal, or zero for a refusal before the read. */
    atomic_store(&g2d.dev.phase,OS_DEVICE_CLOSED);
    return -1;
}
/* g2d_clock_disable: module clock, bus clock, then the G2D and the shared DE_SYS resets asserted; G2D is the only
 * DE_SYS consumer in the OS image, and the DE_SYS reset is asserted only after the DE and DI module and bus gates read
 * closed (the vendor asserts the shared reset only for its last consumer). g2d_bsp_close's local writes follow the bus gate in the vendor order and reach a
 * gated block; asserting the bus reset clears those registers, so they are not written. */
static int do_close(void)
{
    const unsigned faulted=os_device_close_begin(&g2d.dev);
    g2d.limit=lease_now()+G2D_CALL_US;
    if(g2d.limit>session_deadline())g2d.limit=session_deadline();
    /* After a fault the open may have stopped before the engine's bus clock and reset opened: its registers are
     * touched only while both read open. The fault record keeps the first failure. */
    uint32_t bgr=0x10001u;
    if(faulted&&!rd(CCU+0xa44,&bgr))return fail(G2D_STEP_CLOSE,1);
    const int engine=(bgr&0x10001u)==0x10001u;
    if(engine&&!update(G2D+0x20,0x50,0))return fail(G2D_STEP_CLOSE,0);
    /* INTID 83 disabled and observed disabled, then master 10 detached while the engine still runs, before its gates
     * and reset: the IOMMU's last detach disables both banks and returns the clock lease. */
    {
        int r=gicv3_spi_mask(&g2d.gic);
        for(unsigned i=0;r==1&&i<200;++i){r=gicv3_spi_masked(&g2d.gic);if(r==1)break;if(r==0)pause_us(10);}
        if(r!=1)return fail(G2D_STEP_CLOSE_GIC,(uint32_t)r);
    }
    /* A faulted engine may still be running an operation (a timeout, a launch that did not read back, a busy engine):
     * it is stopped before master 10 returns to bypass, which would widen what it can reach to all of memory. The
     * RCQ update is turned off as g2d_mixer_apply does before a job (g2d_mixer.c 1003), the mixer and rotator AHB
     * resets are pulsed as the vendor's timeout recovery does (g2d_wait_cmd_finish, g2d.c 786; g2d_bsp_reset,
     * g2d_top.c 86-93), and the engine must then read idle: update (0x28 bit 0) and the mixer's start (0x100 bit 31)
     * clear. */
    if(faulted&&engine){
        uint32_t v=0;
        if(!update(G2D+0x28,1u,0))return fail(G2D_STEP_CLOSE_STOP,0);
        if(!update(G2D+0x08,3u,0))return fail(G2D_STEP_CLOSE_RESET,0);
        if(!update(G2D+0x08,3u,3u))return fail(G2D_STEP_CLOSE_RESET,1);
        if(!rd(G2D+0x28,&v)||(v&1u))return fail(G2D_STEP_CLOSE_IDLE,v);
        if(!rd(G2D+0x100,&v)||(v&0x80000000u))return fail(G2D_STEP_CLOSE_IDLE,v);
    }
    /* The owner's record decides: a first attach that faulted after its clock lease records master 10 as attached
     * before any of its writes, although the service never saw the attach complete. */
    if(g2d.attached||(os_iommu_owner()->attached>>G2D_MASTER&1u)){
        if(os_iommu_detach(1u<<G2D_MASTER)!=1)return fail(G2D_STEP_CLOSE_DETACH,os_iommu_owner()->fault_step);
        g2d.attached=0;memset(g2d.maps,0,sizeof(g2d.maps));
    }
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
    atomic_store(&g2d.dev.phase,OS_DEVICE_CLOSED);
    return 1;
}
/* An open service, or a FAULT one whose close has not started: the close then runs the vendor order from the state the
 * fault left, and on success the service is CLOSED with its fault record kept. */
static int closable(void){return os_device_closable(&g2d.dev,1);}
/* --- operations ----------------------------------------------------------------------------------------------- */
const struct large_slot *os_large_slot(unsigned index);
static int run_refuse(unsigned step,uint32_t value){return os_device_refuse(&g2d.dev,step,value);}
static int zero(const void *p,size_t n){const uint8_t *b=p;for(size_t i=0;i<n;++i)if(b[i])return 0;return 1;}
/* Whether bytes at offset lie inside a region. */
static int inside_region(const struct large_slot *l,uint64_t offset,uint64_t bytes)
{
    return offset<=l->bytes&&bytes<=l->bytes-offset;
}
/* The large region holding a surface, or null: ready, writable for an output, and the surface's rows inside it. An
 * output is one of the RGB orders; an NV16 surface's chroma plane must lie in the region too, and an RGB surface names
 * no chroma plane. */
static const struct large_slot *surface_slot(const struct ss_g2d_surface *f,int write)
{
    const struct large_slot *l=os_large_slot(f->region);
    const int yuv=f->format==SS_G2D_FORMAT_NV16&&!write;
    if(!l||__atomic_load_n(&l->state,__ATOMIC_ACQUIRE)!=SS_LARGE_READY||(write&&!(l->flags&SS_LARGE_WRITE))||
       (f->format>7&&!yuv)||f->reserved||f->reserved1||!f->width||!f->height||
       f->stride<(uint64_t)f->width*(yuv?1u:4u)||(f->stride&3)||(f->offset&3))return NULL;
    if(!yuv?f->chroma_offset||f->chroma_stride:
       f->chroma_stride<(((uint64_t)f->width+1)&~UINT64_C(1))||((f->chroma_stride|f->chroma_offset)&3)||
       !inside_region(l,f->chroma_offset,(uint64_t)f->chroma_stride*f->height))return NULL;
    return inside_region(l,f->offset,(uint64_t)f->stride*f->height)?l:NULL;
}
static int g2d_rd(unsigned off,uint32_t *v){return rd(G2D+off,v);}
static int g2d_wr(unsigned off,uint32_t v){return wr(G2D+off,v);}
/* A region an operation uses, and the access its role needs: the output read-write, a source read-only, a region that
 * is both read-write. */
struct need {const struct large_slot *slot;uint32_t region,access;};
static uint64_t mapped_iova(uint32_t region)
{
    for(unsigned i=0;i<2;++i)if(g2d.maps[i].region==region+1)return g2d.maps[i].span.iova;
    return 0;
}
static void record_owner(struct ss_g2d_done *d)
{
    const struct a733_iommu_owner *o=os_iommu_owner();
    d->iommu_fault_step=o->fault_step;d->iommu_fault_word=o->fault_word;d->iommu_fault_read=o->fault_read;
}
/* The operation's regions mapped for master 10, with the command page: an attach at the open's first operation; later,
 * an unmap of each mapped region the operation does not use with the same access, then a map of each one missing. */
static void unreserve(const unsigned *slots,unsigned n){for(unsigned k=0;k<n;++k)g2d.maps[slots[k]].region=0;}
static int map_regions(const struct need *need,unsigned count)
{
    struct ss_g2d_done *d=&g2d.done;
    unsigned present=0;
    if(g2d.attached){
        for(unsigned i=0;i<2;++i){
            if(!g2d.maps[i].region)continue;
            unsigned keep=0;
            for(unsigned k=0;k<count;++k)
                if(g2d.maps[i].region==need[k].region+1&&g2d.maps[i].span.access==need[k].access){keep=1;present|=1u<<k;}
            if(keep)continue;
            if(os_iommu_unmap(&g2d.maps[i].span,1)!=1){
                record_owner(d);
                return fail(G2D_STEP_RUN_MAP,os_iommu_owner()->fault_step<<20|(os_iommu_owner()->fault_word&0xfffff));
            }
            g2d.maps[i].region=0;
        }
    }
    struct a733_iommu_span fresh[3];
    unsigned slots[2],n=0;
    for(unsigned k=0;k<count;++k){
        if(present>>k&1)continue;
        const struct large_slot *l=need[k].slot;
        unsigned i=0;
        while(i<2&&g2d.maps[i].region)++i;
        if(i==2){unreserve(slots,n);return run_refuse(G2D_STEP_RUN_IOVA,0xfffe);}
        fresh[n]=(struct a733_iommu_span){0,l->base,l->bytes,need[k].access};
        if(os_iommu_iova(l->bytes,UINT64_C(0x200000),&fresh[n].iova)!=1){
            unreserve(slots,n);return run_refuse(G2D_STEP_RUN_IOVA,need[k].region);
        }
        /* Reserved for this region now, so the next missing region takes the other entry; published on success. */
        g2d.maps[i].region=UINT32_MAX;slots[n++]=i;
    }
    if(!g2d.attached){
        if(os_iommu_iova(G2D_RCQ_BYTES,G2D_RCQ_BYTES,&g2d.rcq_iova)!=1){
            unreserve(slots,n);return run_refuse(G2D_STEP_RUN_IOVA,0xffff);
        }
        fresh[n]=(struct a733_iommu_span){g2d.rcq_iova,(uint64_t)(uintptr_t)g2d.rcq,G2D_RCQ_BYTES,A733_IOMMU_READ};
        d->surface_physical=need[0].slot->base;
        const uint64_t t0=now_us();
        const int r=os_iommu_attach(1u<<G2D_MASTER,fresh,n+1);
        const struct a733_iommu_owner *o=os_iommu_owner();
        const struct a733_iommu_owner_entry *e=&o->entry;
        if(e->recorded){
            for(unsigned b=0;b<2;++b){
                d->iommu_bank_words[4*b]=e->enable[b];d->iommu_bank_words[4*b+1]=e->status[b];
                d->iommu_bank_words[4*b+2]=e->l1[b];d->iommu_bank_words[4*b+3]=e->l2[b];
            }
            d->iommu_bypass[0]=e->bypass[0];d->iommu_bypass[1]=e->bypass[G2D_MASTER];
        }
        record_owner(d);d->iommu_refused=o->refused_word;d->directory_physical=o->directory_physical;
        if(r!=1){
            unreserve(slots,n);
            /* A refusal with no write leaves the IOMMU closed; anything else is a fault of the session. The fault
             * value keeps the owner's step and the whole 17-bit register offset. */
            if(o->state!=A733_IOMMU_OWNER_FAULT)return run_refuse(G2D_STEP_RUN_ATTACH,o->refused_word);
            return fail(G2D_STEP_RUN_ATTACH,o->fault_step<<20|(o->fault_word&0xfffff));
        }
        d->attach_us=now_us()-t0;
        g2d.attached=1;d->iommu_attached=1;
        /* The prefetch words the attach wrote and read back, for the record (docs/iommu.md); all ones when the read
         * was refused, which no prefetch word can be. */
        if(os_iommu_prefetch(d->iommu_prefetch)!=1)d->iommu_prefetch[0]=d->iommu_prefetch[1]=UINT32_MAX;
    }else if(n&&os_iommu_map(fresh,n)!=1){
        unreserve(slots,n);
        record_owner(d);
        return fail(G2D_STEP_RUN_MAP,os_iommu_owner()->fault_step<<20|(os_iommu_owner()->fault_word&0xfffff));
    }
    for(unsigned k=0,f=0;k<count;++k){
        if(present>>k&1)continue;
        g2d.maps[slots[f]].region=need[k].region+1;g2d.maps[slots[f]].span=fresh[f];++f;
    }
    return 1;
}
/* The whole 64-byte lines a plane of height rows of stride bytes at offset touches; they lie in its page-aligned
 * region, and the rows' padding is the application's own memory. */
static void plane_lines(const struct large_slot *l,uint64_t offset,uint32_t stride,uint32_t height,uintptr_t *at,
                        size_t *bytes)
{
    const uintptr_t first=(uintptr_t)(l->base+offset);
    *at=first&~(uintptr_t)63;
    *bytes=((first+(size_t)stride*height+63)&~(uintptr_t)63)-*at;
}
static void surface_lines(const struct large_slot *l,const struct ss_g2d_surface *f,uintptr_t *at,size_t *bytes)
{
    plane_lines(l,f->offset,f->stride,f->height,at,bytes);
}
/* The wait for the armed operation's finish, bounded by the vendor's 100 ms: the handler's seen (1 the armed kind
 * finished, 2 it did not), or -1 after a timeout, which fails the operation with the block's status word (status_off,
 * 0x24 for the mixer, ROT_INT for the rotator). */
static int await_finish(struct ss_g2d_done *d,unsigned status_off)
{
    const uint64_t until=now_us()+G2D_JOB_US;
    while(!atomic_load_explicit(&g2d.irq_seen,memory_order_acquire)){
        if(now_us()>=until){
            /* Disarmed first: a handler that already took the interrupt sets irq_seen, which is read once more. */
            if(atomic_exchange(&g2d.armed,0)&&!atomic_load(&g2d.irq_seen)){
                uint32_t status=0;(void)g2d_rd(status_off,&status);
                d->stray_finishes=atomic_load(&g2d.stray_finishes);d->stray_word=g2d.stray_word;
                return fail(G2D_STEP_RUN_TIMEOUT,status);
            }
            while(!atomic_load_explicit(&g2d.irq_seen,memory_order_acquire))pause_us(1);
            break;
        }
        (void)sunstone_deadline_wait_until(until);
    }
    const unsigned seen=atomic_exchange(&g2d.irq_seen,0);
    d->irq_ticks=g2d.irq_ticks;d->irq_count=atomic_load(&g2d.irq_count);d->stray_interrupts=atomic_load(&g2d.stray);
    d->stray_finishes=atomic_load(&g2d.stray_finishes);d->stray_word=g2d.stray_word;
    return (int)seen;
}
/* After either block's finish: master 10's fault scan, then the output's lines dropped from the CPU caches. */
static int finish(struct ss_g2d_done *d,uintptr_t at,size_t bytes)
{
    struct a733_iommu_owner_fault f;
    d->iommu_scan=(uint32_t)os_iommu_scan(G2D_MASTER,&f);
    _Static_assert(sizeof(f)==sizeof(d->iommu_fault),"the fault record fits the done record");
    memcpy(d->iommu_fault,&f,sizeof(f));
    d->iommu_fault_step=os_iommu_owner()->fault_step;d->iommu_fault_word=os_iommu_owner()->fault_word;
    if(d->iommu_scan!=1)return fail(d->iommu_scan==0?G2D_STEP_RUN_IOMMU_FAULT:G2D_STEP_RUN_IOMMU,f.micro_status);
    /* Lines the CPU may have fetched during the operation are dropped before the application reads the output. */
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,at,bytes))return run_refuse(G2D_STEP_RUN_CACHE,1);
    d->done_ticks=ss_counter_ticks();
    return 1;
}
/* A rotate (g2d_rotate_set_para): the regions mapped as for a blit, the source's dirty lines cleaned and the output's
 * cleaned and invalidated, the finish status cleared with the interrupt off, the job's words written in the vendor's
 * order, the control word read back, then the finish-interrupt enable and the start bit; the handler completes it.
 * The vendor never reads the start bit back and sets it again for each job, so it is recorded, not required clear. */
static int do_rotate(const struct ss_g2d_job *j,struct ss_g2d_done *d,const struct large_slot *l,
                     const struct large_slot *src)
{
    const int swap=j->degrees==90||j->degrees==270;
    const uint32_t ow=swap?j->h:j->w,oh=swap?j->w:j->h;
    if(!a733_g2d_inside(j->sx,j->sy,j->w,j->h,j->source.width,j->source.height)||
       !a733_g2d_inside(j->dx,j->dy,ow,oh,j->output.width,j->output.height))return run_refuse(G2D_STEP_RUN_GEOMETRY,2);
    if(!g2d.rcq&&!(g2d.rcq=os_owner_memory(G2D_RCQ_BYTES,G2D_RCQ_BYTES,OS_MEMORY_DEVICE)))
        return run_refuse(G2D_STEP_RUN_MEMORY,0);
    const int same=j->source.region==j->output.region;
    const struct need need[2]={{l,j->output.region,A733_IOMMU_READ|A733_IOMMU_WRITE},{src,j->source.region,A733_IOMMU_READ}};
    if(map_regions(need,same?1:2)!=1)return -1;
    const uint64_t output_iova=mapped_iova(j->output.region)+j->output.offset;
    const uint64_t source_iova=mapped_iova(j->source.region)+j->source.offset;
    d->output_iova=(uint32_t)output_iova;d->source_iova=(uint32_t)source_iova;
    const struct a733_g2d_rotate r={source_iova,output_iova,j->source.stride,j->output.stride,j->sx,j->sy,j->dx,j->dy,
                                    j->w,j->h,j->source.format,j->degrees,j->flip};
    struct a733_g2d_rot_word words[A733_G2D_ROT_WORDS];
    if(a733_g2d_rotate_words(&r,words)!=A733_G2D_ROT_WORDS)return run_refuse(G2D_STEP_RUN_GEOMETRY,3);
    uintptr_t at,source_at;size_t bytes,source_bytes;
    surface_lines(l,&j->output,&at,&bytes);
    surface_lines(src,&j->source,&source_at,&source_bytes);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,at,bytes))return run_refuse(G2D_STEP_RUN_CACHE,0);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN,source_at,source_bytes))return run_refuse(G2D_STEP_RUN_CACHE,2);
    __asm__ volatile("dsb sy":::"memory");
    const unsigned rot_int=A733_G2D_ROT_BASE+A733_G2D_ROT_INT,rot_ctl=A733_G2D_ROT_BASE+A733_G2D_ROT_CTL;
    uint32_t v=0;
    /* Writing one clears the finish status and leaves the interrupt disabled. */
    if(!g2d_wr(rot_int,A733_G2D_ROT_INT_DONE)||!g2d_rd(rot_int,&v)||(v&A733_G2D_ROT_INT_DONE))
        return fail(G2D_STEP_RUN_LAUNCH,v);
    for(unsigned i=0;i+2<A733_G2D_ROT_WORDS;++i)
        if(!g2d_wr(words[i].offset,words[i].value))return fail(G2D_STEP_RUN_LAUNCH,1);
    if(!g2d_rd(rot_ctl,&d->rot_ctl)||d->rot_ctl!=words[0].value)return fail(G2D_STEP_RUN_LAUNCH,4);
    atomic_store_explicit(&g2d.armed,A733_G2D_IRQ_ROTATE,memory_order_release);
    if(!g2d_wr(words[A733_G2D_ROT_WORDS-2].offset,words[A733_G2D_ROT_WORDS-2].value))return fail(G2D_STEP_RUN_LAUNCH,2);
    d->launch_ticks=ss_counter_ticks();
    if(!g2d_wr(words[A733_G2D_ROT_WORDS-1].offset,words[A733_G2D_ROT_WORDS-1].value))return fail(G2D_STEP_RUN_LAUNCH,3);
    const int seen=await_finish(d,rot_int);
    if(seen<0)return -1;
    d->rot_int=g2d.irq_status;
    if(seen!=1||!(g2d.irq_status&A733_G2D_ROT_INT_DONE))return fail(G2D_STEP_RUN_IRQ,g2d.irq_status);
    /* The handler wrote the finish word back: the status reads clear. Recorded only: the control word, the timeout
     * word and the mixer's 0x24. */
    if(!g2d_rd(rot_int,&d->rot_int_after)||(d->rot_int_after&A733_G2D_ROT_INT_DONE))
        return fail(G2D_STEP_RUN_FINISH,d->rot_int_after);
    if(!g2d_rd(rot_ctl,&d->rot_ctl_after)||!g2d_rd(A733_G2D_ROT_BASE+A733_G2D_ROT_TIMEOUT,&d->rot_timeout)||
       !g2d_rd(0x24,&d->status_after))return fail(G2D_STEP_RUN_FINISH,0x28000);
    return finish(d,at,bytes);
}
/* g2d_mixer_apply: update and task-end interrupt off, the RCQ header address and length, the interrupt on, then the
 * update bit; the handler completes it. The caller has built the command page and maintained the CPU caches. */
static int mixer_run(struct ss_g2d_done *d,uintptr_t at,size_t bytes)
{
    /* g2d_mixer_apply writes the update bit to 0 before each job and never relies on the engine clearing it; the
     * engine must then read idle: update off and the mixer not started. */
    uint32_t v=0;
    if(!g2d_wr(0x28,0)||!g2d_rd(0x28,&v)||(v&1)||!rd(G2D+0x100,&v)||(v&0x80000000))return fail(G2D_STEP_RUN_BUSY,v);
    /* Only task end must read clear before the interrupt is enabled. cfg_finish (bit 2) stays set after a job and a
     * write of one does not clear it (board run 1794722460072814405-2b77c3eb); its interrupt enable (0x20 bit 6) is
     * never set, and g2d_mixer_apply neither clears nor reads it. */
    if(!g2d_wr(0x20,0)||!g2d_wr(0x24,1)||!g2d_rd(0x24,&d->status_launch)||(d->status_launch&1))
        return fail(G2D_STEP_RUN_LAUNCH,d->status_launch);
    if(!g2d_wr(0x2c,(uint32_t)g2d.rcq_iova)||!g2d_wr(0x30,0)||!g2d_wr(0x34,G2D_RCQ_HEADER_BYTES))
        return fail(G2D_STEP_RUN_LAUNCH,1);
    atomic_store_explicit(&g2d.armed,A733_G2D_IRQ_MIXER,memory_order_release);
    if(!g2d_wr(0x20,0x10))return fail(G2D_STEP_RUN_LAUNCH,2);
    d->launch_ticks=ss_counter_ticks();
    if(!g2d_wr(0x28,1))return fail(G2D_STEP_RUN_LAUNCH,3);
    const int seen=await_finish(d,0x24);
    if(seen<0)return -1;
    d->rcq_status=g2d.irq_status;d->reset_word=g2d.irq_reset;
    if(seen!=1||!(g2d.irq_status&1))return fail(G2D_STEP_RUN_IRQ,g2d.irq_status);
    /* The handler acknowledged task end and pulsed the mixer reset: task end reads clear, the reset word reads back. */
    if(!g2d_rd(0x24,&d->status_after)||(d->status_after&1)||!g2d_rd(0x08,&v)||v!=(g2d.irq_reset|1u))
        return fail(G2D_STEP_RUN_FINISH,v);
    /* Recorded only: whether the engine clears the update bit, and the mixer control word. */
    if(!g2d_rd(0x28,&d->update_after)||!rd(G2D+0x100,&d->mixer_after))return fail(G2D_STEP_RUN_FINISH,0x100);
    return finish(d,at,bytes);
}
/* A convert (g2d_bsp_bitblt with a YUV source): the regions mapped as for a blit, the command page from
 * a733_g2d_convert_commands at the planes' device addresses, the output's lines cleaned and invalidated and both source
 * planes' cleaned, then the mixer's launch. */
_Static_assert(SS_G2D_CSC_BT601_LIMITED==A733_G2D_CSC_BT601_LIMITED&&SS_G2D_CSC_BT601_FULL==A733_G2D_CSC_BT601_FULL&&
    SS_G2D_CSC_BT709_LIMITED==A733_G2D_CSC_BT709_LIMITED&&SS_G2D_CSC_BT709_FULL==A733_G2D_CSC_BT709_FULL&&
    SS_G2D_FORMAT_NV16==A733_G2D_YUV422_SP_A,"the job's codes are the builder's");
static int do_convert(const struct ss_g2d_job *j,struct ss_g2d_done *d,const struct large_slot *l,
                      const struct large_slot *src)
{
    if(!a733_g2d_inside(j->sx,j->sy,j->w,j->h,j->source.width,j->source.height)||
       !a733_g2d_inside(j->dx,j->dy,j->ow,j->oh,j->output.width,j->output.height))
        return run_refuse(G2D_STEP_RUN_GEOMETRY,2);
    if(!g2d.rcq&&!(g2d.rcq=os_owner_memory(G2D_RCQ_BYTES,G2D_RCQ_BYTES,OS_MEMORY_DEVICE)))
        return run_refuse(G2D_STEP_RUN_MEMORY,0);
    const int same=j->source.region==j->output.region;
    const struct need need[2]={{l,j->output.region,A733_IOMMU_READ|A733_IOMMU_WRITE},{src,j->source.region,A733_IOMMU_READ}};
    if(map_regions(need,same?1:2)!=1)return -1;
    const uint64_t output_iova=mapped_iova(j->output.region)+j->output.offset;
    const uint64_t luma_iova=mapped_iova(j->source.region)+j->source.offset;
    const uint64_t chroma_iova=mapped_iova(j->source.region)+j->source.chroma_offset;
    if((output_iova|luma_iova|chroma_iova)>>32)return run_refuse(G2D_STEP_RUN_GEOMETRY,4);
    d->output_iova=(uint32_t)output_iova;d->source_iova=(uint32_t)luma_iova;
    const struct a733_g2d_convert c={(uint32_t)luma_iova,(uint32_t)chroma_iova,j->source.stride,j->source.chroma_stride,
        j->sx,j->sy,j->w,j->h,j->source.format,(uint32_t)output_iova,j->output.stride,j->dx,j->dy,j->ow,j->oh,
        j->output.format,j->csc};
    if(!a733_g2d_convert_commands(g2d.rcq,G2D_RCQ_BYTES,&c,(uint32_t)g2d.rcq_iova))return run_refuse(G2D_STEP_RUN_GEOMETRY,3);
    uintptr_t at,luma_at,chroma_at;size_t bytes,luma_bytes,chroma_bytes;
    surface_lines(l,&j->output,&at,&bytes);
    surface_lines(src,&j->source,&luma_at,&luma_bytes);
    plane_lines(src,j->source.chroma_offset,j->source.chroma_stride,j->source.height,&chroma_at,&chroma_bytes);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,at,bytes))return run_refuse(G2D_STEP_RUN_CACHE,0);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN,luma_at,luma_bytes))return run_refuse(G2D_STEP_RUN_CACHE,2);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN,chroma_at,chroma_bytes))return run_refuse(G2D_STEP_RUN_CACHE,3);
    __asm__ volatile("dsb sy":::"memory");
    return mixer_run(d,at,bytes);
}
/* One operation: the job's checks, then a rotate, a convert, or a fill or blit through the native builder. */
static int do_run(void)
{
    const struct ss_g2d_job *j=&g2d.job;
    struct ss_g2d_done *d=&g2d.done;
    memset(d,0,sizeof(*d));
    d->magic=SS_G2D_DONE_MAGIC;d->bytes=sizeof(*d);d->submit_ticks=ss_counter_ticks();
    os_device_clear_fault(&g2d.dev);
    g2d.limit=lease_now()+G2D_CALL_US;
    if(g2d.limit>session_deadline())g2d.limit=session_deadline();
    if(atomic_load(&g2d.irq_seen)||atomic_load(&g2d.stray))
        return run_refuse(G2D_STEP_RUN_IRQ,atomic_load(&g2d.irq_seen)|atomic_load(&g2d.stray)<<8);
    atomic_store(&g2d.stray_finishes,0);g2d.stray_word=0;
    /* A fill writes an ARGB8888 output and reads no source. A blit reads a source of the output's geometry (the
     * builder's one width, height and stride) and converts from its format to the output's. A rotate reads a source
     * of its own geometry in the output's format, through the rotate module. A convert reads an NV16 source of its
     * own geometry and writes an RGB output. */
    const int blit=j->op==SS_G2D_BLIT,rotate=j->op==SS_G2D_ROTATE,convert=j->op==SS_G2D_CONVERT;
    const int reads=blit||rotate||convert;
    if((j->op!=SS_G2D_FILL&&!reads)||(!rotate&&(j->degrees||j->flip))||(!convert&&(j->ow|j->oh|j->csc))||j->reserved||
       (!reads&&(j->sx||j->sy||!zero(&j->source,sizeof(j->source))||j->output.format))||
       (reads&&!convert&&j->source.format>7)||
       (convert&&(j->color||j->source.format!=SS_G2D_FORMAT_NV16||(j->sx&1)||j->csc-1u>=4u))||
       (blit&&(j->color||j->source.width!=j->output.width||j->source.height!=j->output.height||
               j->source.stride!=j->output.stride))||
       (rotate&&(j->color||j->source.format!=j->output.format||j->flip>(SS_G2D_FLIP_H|SS_G2D_FLIP_V)||
                 (j->degrees!=0&&j->degrees!=90&&j->degrees!=180&&j->degrees!=270))))
        return run_refuse(G2D_STEP_RUN,j->op);
    const struct large_slot *l=surface_slot(&j->output,1);
    if(!l)return run_refuse(G2D_STEP_RUN_SURFACE,j->output.region);
    const struct large_slot *src=reads?surface_slot(&j->source,0):NULL;
    if(reads&&!src)return run_refuse(G2D_STEP_RUN_SURFACE,j->source.region|0x100u);
    if(rotate)return do_rotate(j,d,l,src);
    if(convert)return do_convert(j,d,l,src);
    struct a733_g2d_session *c=&g2d.commands;
    memset(c,0,sizeof(*c));
    c->width=j->output.width;c->height=j->output.height;c->stride=j->output.stride;
    c->op=blit?A733_G2D_BLIT:A733_G2D_FILL;
    c->dx=j->dx;c->dy=j->dy;c->w=j->w;c->h=j->h;c->color=j->color;
    if(blit){c->sx=j->sx;c->sy=j->sy;c->format=j->source.format;c->output_format=j->output.format;}
    if(!a733_g2d_geometry_valid(c))return run_refuse(G2D_STEP_RUN_GEOMETRY,0);
    if(!g2d.rcq&&!(g2d.rcq=os_owner_memory(G2D_RCQ_BYTES,G2D_RCQ_BYTES,OS_MEMORY_DEVICE)))
        return run_refuse(G2D_STEP_RUN_MEMORY,0);
    const int same=blit&&j->source.region==j->output.region;
    const struct need need[2]={{l,j->output.region,A733_IOMMU_READ|A733_IOMMU_WRITE},{src,j->source.region,A733_IOMMU_READ}};
    if(map_regions(need,blit&&!same?2:1)!=1)return -1;
    const uint64_t output_iova=mapped_iova(j->output.region)+j->output.offset;
    /* A fill reads no source; the layer's address names the output, which is mapped. */
    const uint64_t source_iova=blit?mapped_iova(j->source.region)+j->source.offset:output_iova;
    d->output_iova=(uint32_t)output_iova;d->source_iova=blit?(uint32_t)source_iova:0;
    if(!a733_g2d_commands_at(g2d.rcq,G2D_RCQ_BYTES,c,(uint32_t)source_iova,(uint32_t)output_iova,(uint32_t)g2d.rcq_iova))
        return run_refuse(G2D_STEP_RUN_GEOMETRY,1);
    /* The output's lines leave the CPU caches before the engine writes it, and a source's dirty lines reach memory
     * before the engine reads it; the command page is non-cacheable. A source in the output's region is covered by
     * the output's maintenance only where they overlap, so it is cleaned on its own. */
    uintptr_t at,source_at=0;size_t bytes,source_bytes=0;
    surface_lines(l,&j->output,&at,&bytes);
    if(blit)surface_lines(src,&j->source,&source_at,&source_bytes);
    if(!aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE,at,bytes))return run_refuse(G2D_STEP_RUN_CACHE,0);
    if(blit&&!aarch64_device_cache(AARCH64_CACHE_CLEAN,source_at,source_bytes))return run_refuse(G2D_STEP_RUN_CACHE,2);
    __asm__ volatile("dsb sy":::"memory");
    return mixer_run(d,at,bytes);
}
static void serve(void)
{
    if(atomic_load_explicit(&g2d.request,memory_order_acquire)!=POSTED)return;
    const unsigned phase=atomic_load(&g2d.dev.phase);
    int r=-1;
    if(g2d.op==OPEN_OP)r=phase==OS_DEVICE_CLOSED?do_open():phase==OS_DEVICE_OPEN?1:-1;
    else if(g2d.op==CLOSE_OP)r=closable()?do_close():phase==OS_DEVICE_CLOSED?1:-1;
    /* A run on a service that is not OPEN is refused with nothing recorded: a FAULT service's fault may already be
     * reported, and its fields must not change under the quiesce step's reads (apps/os/device-fault.h). */
    else if(g2d.op==RUN_OP)r=phase==OS_DEVICE_OPEN?do_run():-1;
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
        if(atomic_load(&g2d.dev.quiesce_wanted)&&atomic_load(&g2d.request)!=POSTED){
            if(closable())(void)do_close();
            if(atomic_load(&g2d.dev.phase)==OS_DEVICE_CLOSED)atomic_store(&g2d.dev.quiesced,1);
        }
        (void)sunstone_deadline_wait_until(now_us()+10000000);
    }
}

/* INTID 83 (g2d_handle_irq), shared by the mixer and the rotate module. With nothing armed the line is masked at the
 * distributor and counted, and no engine register is read. With an operation armed, a733_g2d_irq_dispatch decides
 * from 0x20, 0x24 and ROT_INT: a finish of the other block is masked at that block and counted, and the operation
 * stays armed; the armed block's finish is acknowledged (task end written one and the mixer AHB reset pulsed, or
 * ROT_INT written back) and the owner woken; an interrupt with neither masks the line and fails the operation.
 * Built with -mgeneral-regs-only, like every unit it calls. */
void os_g2d_irq(void)
{
    BaseType_t woken=pdFALSE;
    atomic_fetch_add_explicit(&g2d.irq_count,1,memory_order_relaxed);
    const unsigned kind=atomic_load_explicit(&g2d.armed,memory_order_acquire);
    const uintptr_t rot_int=G2D+A733_G2D_ROT_BASE+A733_G2D_ROT_INT;
    uint32_t ctl=0,rcq=0,rot=0;
    struct a733_g2d_irq_action a={0,0,0,0,0,0};
    if(kind){
        ctl=a733_mmio_read32_dsb_sy_both(G2D+0x20);rcq=a733_mmio_read32_dsb_sy_both(G2D+0x24);
        rot=a733_mmio_read32_dsb_sy_both(rot_int);
        a=a733_g2d_irq_dispatch(kind,ctl,rcq,rot);
        if(a.stray){
            if(atomic_fetch_add_explicit(&g2d.stray_finishes,1,memory_order_relaxed)==0)g2d.stray_word=a.mask_mixer?rcq:rot;
            if(a.mask_mixer)a733_mmio_write32_dsb_sy_both(G2D+0x20,ctl&~0x10u);
            if(a.mask_rotate)a733_mmio_write32_dsb_sy_both(rot_int,a733_g2d_rot_int_masked(rot));
        }
        /* Only a stray: the operation stays armed and the owner is not woken. */
        if(a.seen==3)return;
    }
    if(!kind||!atomic_exchange_explicit(&g2d.armed,0,memory_order_acq_rel)){
        /* No operation armed, or the owner's timeout disarmed it first: counted apart, so that it cannot overwrite a
         * completion the owner has not read yet. */
        (void)gicv3_spi_mask(&g2d.gic);
        atomic_fetch_add_explicit(&g2d.stray,1,memory_order_release);
    } else {
        g2d.irq_status=kind==A733_G2D_IRQ_ROTATE?rot:rcq;g2d.irq_ticks=ss_counter_ticks();
        if(a.ack_mixer){
            a733_mmio_write32_dsb_sy_both(G2D+0x24,1u);
            const uint32_t reset=a733_mmio_read32_dsb_sy_both(G2D+0x08);
            g2d.irq_reset=reset;
            a733_mmio_write32_dsb_sy_both(G2D+0x08,reset&~1u);
            a733_mmio_write32_dsb_sy_both(G2D+0x08,reset|1u);
        } else if(a.ack_rotate)a733_mmio_write32_dsb_sy_both(rot_int,rot);
        else (void)gicv3_spi_mask(&g2d.gic);
        atomic_store_explicit(&g2d.irq_seen,a.seen==1?1u:2u,memory_order_release);
    }
    if(owner)vTaskNotifyGiveFromISR(owner,&woken);
    portYIELD_FROM_ISR(woken);
}

int os_g2d_task_privileged(void (*entry)(void *),void *context){return !context&&entry==os_g2d_task;}
int os_g2d_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
/* A FAULT service first gets its close: DONE when every close step read back, FAULT (site 0x931) when one failed
 * (apps/os/device-fault.h). */
static enum os_quiesce_result quiesce_step(struct os_quiesce *q){return os_device_quiesce_step(&g2d.dev,q);}
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
    g2d.gic=(struct gicv3_spi){GICD,G2D_INTID};
    /* No OS-image path changes DCDC2 or DCDC4 before the owner verifies them: DCDC2 has no writer, and the GPU
     * platform's DCDC4 raise holds an exclusive rail lease through its own settle, which the verifier's shared lease
     * waits out (docs/g2d-os.md#the-supply-verification-wait). The NPU startup marks the supply settled on the same
     * grounds. The call takes effect only before the power service first runs. */
    settled_supply=(unsigned)a733_power_domains_settled_supply(&providers.domains);
    owner=xTaskCreateStaticAffinitySet(os_g2d_task,"os-g2d",sizeof(owner_stack)/sizeof(owner_stack[0]),NULL,4,
        owner_stack,&owner_tcb,1);
    return owner!=NULL;
}

/* --- application calls ---------------------------------------------------------------------------------------------- */
static int granted(void){const struct ss_grant *g=ss_grant();return g&&(g->devices&SS_DEVICE_G2D)&&owner;}
static int call(unsigned op)
{
    if(!granted()||atomic_load(&g2d.dev.quiesce_wanted))return -1;
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
    if(step)*step=g2d.dev.fault_step;
    if(value)*value=g2d.dev.fault_value;
    return g2d.dev.fault_step?1:0;
}
int ss_g2d_run(const struct ss_g2d_job *job,struct ss_g2d_done *done)
{
    if(!job)return -1;
    const unsigned state=atomic_load_explicit(&g2d.request,memory_order_acquire);
    if(granted()&&state!=POSTED&&state!=DONE)g2d.job=*job;
    const int r=call(RUN_OP);
    if(r!=0&&done&&g2d.done.magic==SS_G2D_DONE_MAGIC)*done=g2d.done;
    return r;
}
static int ready(void){return atomic_load(&g2d.request)==DONE||atomic_load(&g2d.dev.phase)==OS_DEVICE_FAULT;}
void ss_g2d_wait(uint32_t wait_us)
{
    const uint64_t until=now_us()+(wait_us?wait_us:1);
    if(os_application_wait_unless(until,ready)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x75a);
}
