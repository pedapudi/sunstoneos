/* Video decoder service (SS_DEVICE_VIDEO_DECODE, include/sunstone_video_decode.h, docs/video-decode-os.md). One owner
 * task, priority 3 pinned to CPU 4, does every register access through the decoder platform binding
 * (drivers/video/a733_video_platform.c). Open is the binding's a733_video_decoder_open: it requests PCK600 domain 2
 * through the shared power-domain owner, and its cold start opens the decoder's MBUS clock gate, releases reset, opens
 * the bus gate, programs PLL_VE0 to 624 MHz and opens the module gate; it then reads the decoder identity. Close is the
 * binding's a733_video_decoder_close: module gate, bus gate, reset, then PLL_VE0 disabled; domain 2 stays ON. Stage 1 runs no job, so INTID 81 stays disabled and its
 * handler touches no register. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "a733_gpu_resources.h"
#include "a733_platform_locks.h"
#include "a733_power_domains.h"
#include "a733_video_platform.h"
#include "quiesce.h"
#include "sunstone_crumb.h"
#include "sunstone_deadline.h"
#include "sunstone_video_decode.h"
#include <stdatomic.h>
#include <string.h>
#ifdef SUNSTONE_VIDEO_DECODE_MEASURE
/* The stage-1 board record (apps/vdec-check/record.h): only the vdec-check image fills and returns it. */
#include "../vdec-check/record.h"
#define RECORD(...) __VA_ARGS__
#else
#define RECORD(...)
#endif

#define CCU UINT32_C(0x02002000)
#define R_CCU UINT32_C(0x07010000)
#define RTC UINT32_C(0x07090000)
#define GICD UINT32_C(0x03400000)
#define VD_INTID 81u
#define VD_OWNER UINT64_C(0x5645440000000001) /* "VED", lease owner of the rail, clocks and domain row */
#define VD_MODULE UINT32_C(0x81000000)         /* CCU 0xa88: gate, mux 1 (PLL_VE0), divider 1 */
#define VD_MODULE_HZ UINT32_C(624000000)       /* the vendor point, A733_VIDEO_VENDOR_HZ */
#define VD_CPU 4u                              /* an A55 core: not core 0 (tick, SPIs, PMIC task), not 6 or 7 */
#define VD_PRIORITY 3u                         /* scheduler band 1 */
/* Bound of one open or close, inside the session deadline: the supply verification (5,160 + 100,000 us after provider
 * start), the domain transition (10,000 us), the PLL lock wait (about 420 us) and the register sequence, with margin.
 * The binding's deadline is fixed for the whole cold start. */
#define VD_CALL_US 500000u
#define VD_BUSY_US 500u /* the block after the binding gives up on a busy registry or register lock */
enum phase {CLOSED,OPENING,OPEN,CLOSING,FAULT};
enum op {OPEN_OP=1,CLOSE_OP};
enum {POSTED=1,DONE=2};
/* Fault steps (ss_video_decode_fault). Steps below VD_STEP_WRITES write nothing, so a refusal there leaves the decoder
 * as it was. */
enum {
    VD_STEP_NONE, VD_STEP_RAIL, VD_STEP_CLOCK, VD_STEP_PCK, VD_STEP_SUPPLY, VD_STEP_CONFIG, VD_STEP_ENTRY_WORDS,
    VD_STEP_ENTRY, VD_STEP_ROW, VD_STEP_ROW_TIMEOUT, VD_STEP_WRITES=16, VD_STEP_COLD, VD_STEP_STARTED_WORDS,
    VD_STEP_CLOSE=32, VD_STEP_CLOSED_WORDS
};
static struct {
    struct a733_power_domain_row row;
    struct a733_rail_token rail;
    struct a733_video_platform_config config;
    struct a733_video_decoder decoder;
    uint64_t pck_token;
    unsigned pck_client,initialized,closed; /* closed: the decoder is closed for this power lifetime */
    RECORD(struct vdec_check_record info;)
    atomic_uint phase,request,quiesce_wanted,quiesced;
    atomic_uint close_started; /* 1 once a close has started since the last open: a FAULT service gets one close. Only
                                * the owner task reads it. */
    /* 1 once the fault must be reported: a close step failed, or the unit's failure was uncertain, which no close
     * settles. The owner task stores it after the fault fields; quiesce_step loads it and nothing else, see there. A
     * reported service stays FAULT, so it is never cleared. */
    atomic_uint reported;
    unsigned op;int result;
    uint32_t fault_step,fault_value;
    uint64_t limit;
    TaskHandle_t waiter;
} vd;
static TaskHandle_t owner;
static StaticTask_t owner_tcb;
static StackType_t owner_stack[2048];

static uint64_t now_us(void){return sunstone_deadline_now_us();}
static uint64_t lease_now(void){uint64_t t=0;return a733_counter_clock_now(&providers.shared.counter,&t)==1?t:0;}
static uint64_t owner_us(void){uint64_t t=0;return a733_pmic_now(&providers.shared.pmic,&t)==1?t:0;}
static uint64_t session_deadline(void){return board.session.deadline_us-10000;}
static int timely(void){uint64_t t=lease_now();return t&&t<vd.limit&&board_live(NULL);}
static void pause_us(uint32_t us){const uint64_t t=now_us()+us;while(now_us()<t)(void)sunstone_deadline_wait_until(t);}
static int refuse(unsigned step,uint32_t value){vd.fault_step=step;vd.fault_value=value;return -1;}
static int fail(unsigned step,uint32_t value)
{
    if(!vd.fault_step){vd.fault_step=step;vd.fault_value=value;}
    atomic_store(&vd.phase,FAULT);
    if(atomic_load(&vd.close_started)||vd.decoder.unit.uncertain)atomic_store_explicit(&vd.reported,1,memory_order_release);
    return -1;
}
static void wake_owner(void *unused){(void)unused;if(owner)xTaskNotifyGive(owner);}
/* The bound on the owner's answer to the row, in the PMIC clock (docs/designs/power-domains.md, "Transitions and
 * polling", and the NPU startup's two waits). Until supply verification ends with DCDC2 verified or the done bit, the
 * answer cannot come before verification does: verification may start A733_POWER_SUPPLY_SETTLE_US after the
 * service's first step, ends within A733_POWER_SUPPLY_BUDGET_US once the provider is READY, and READY comes by the
 * PMIC's startup deadline; the bound is anchored at that deadline or now, whichever is later, plus one tick for the
 * service's wake. Once verification has ended, the owner admits the row at its next step and the transition ends
 * within A733_POWER_PCK_BOUND_US of its command: the bound is that, plus two ticks, from the first time this task saw
 * verification ended. The wait itself is capped by the open's deadline. */
#define VD_TICK_US (1000000u/configTICK_RATE_HZ)
static uint64_t row_bound(uint64_t *supply,uint64_t *verified)
{
    const uint64_t t=owner_us();
    const unsigned w=a733_power_authority_load(&providers.shared.rails.authority);
    if(w&(A733_POWER_SUPPLY_DONE|A733_POWER_DCDC2_VERIFIED)){
        if(!*verified)*verified=t+A733_POWER_PCK_BOUND_US+2u*VD_TICK_US;
        return *verified;
    }
    if(!*supply){
        const uint64_t anchor=providers.domains.pmic->config.startup_deadline;
        *supply=(anchor>t?anchor:t)+A733_POWER_SUPPLY_SETTLE_US+A733_POWER_SUPPLY_BUDGET_US+VD_TICK_US;
    }
    return *supply;
}

/* --- the binding's native callbacks ----------------------------------------------------------------------------- */
static int op_now(void *cookie,uint64_t *out){(void)cookie;*out=lease_now();return *out?1:-1;}
/* The PLL lock-wait gaps: this core's counter, read without the shared clock's compare-and-swap. The task is pinned
 * to one core, so the reads are monotonic. */
static int op_spin_now(void *cookie,uint64_t *out){(void)cookie;*out=now_us();return 1;}
static int op_enter(void *cookie,uint64_t d)
{(void)cookie;return a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d)==1?1:0;}
static int op_leave(void *cookie){(void)cookie;a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;}

#ifdef SUNSTONE_VIDEO_DECODE_MEASURE
static const uintptr_t words[VDEC_CHECK_WORDS]={
    [VDEC_CHECK_CCU_PLL_REF]=CCU,[VDEC_CHECK_RTC_OSC]=RTC+0x160,[VDEC_CHECK_CCU_AHB]=CCU+0x500,
    [VDEC_CHECK_CCU_NSI]=CCU+0x580,[VDEC_CHECK_CCU_MBUS]=CCU+0x588,[VDEC_CHECK_CCU_PLL_VE0]=CCU+0x220,
    [VDEC_CHECK_CCU_PLL_VE0_P0]=CCU+0x228,[VDEC_CHECK_CCU_PLL_VE0_P1]=CCU+0x22c,
    [VDEC_CHECK_CCU_GATES]=CCU+0x1918,[VDEC_CHECK_CCU_STATUS]=CCU+0x1998,[VDEC_CHECK_CCU_AUTO]=CCU+0x1b10,
    [VDEC_CHECK_CCU_MODULE]=CCU+0xa88,[VDEC_CHECK_CCU_BGR]=CCU+0xa8c,[VDEC_CHECK_CCU_AHB_GATES]=CCU+0x5c0,
    [VDEC_CHECK_CCU_MBUS_GATES]=CCU+0x5e0,[VDEC_CHECK_CCU_MCLK_GATES]=CCU+0x5e4,
    [VDEC_CHECK_RCCU_PCK]=R_CCU+0x1ac,[VDEC_CHECK_GICD_ENABLE]=GICD+0x100+4*(VD_INTID/32),
    [VDEC_CHECK_GICD_PENDING]=GICD+0x200+4*(VD_INTID/32),[VDEC_CHECK_GICD_ACTIVE]=GICD+0x300+4*(VD_INTID/32)};
/* The recorded words, read under the register lock through the binding's read allowlist; read only. */
static int read_words(uint32_t *out)
{
    if(!timely()||op_enter(NULL,vd.limit)!=1)return 0;
    unsigned ok=1;
    for(unsigned i=0;i<VDEC_CHECK_WORDS&&ok;++i)ok=a733_video_platform_native_read(NULL,words[i],&out[i])==1;
    (void)op_leave(NULL);
    return ok;
}

#endif
/* The PCK600 provider client: one the session's board, NPU and GPU clients do not use. G2D takes the highest such
 * client, so the decoder takes the lowest. */
#define VD_GPU_CLIENT 7u
static unsigned pck_client(void)
{
    const struct os_session *s=os_session_admitted();
    for(unsigned c=0;c<A733_GPU_RESOURCE_CLIENTS;++c)
        if(s&&c!=s->board.pck_client&&c!=VD_GPU_CLIENT&&!(os_granted(OS_GRANT_NPU)&&c==s->npu.pck_client))return c;
    return A733_GPU_RESOURCE_CLIENTS;
}
static int tokens(void)
{
    while(!vd.rail.id){
        enum a733_rail_result r=a733_rail_acquire(&providers.shared.rails,VD_OWNER,A733_RAIL_BIT(A733_RAIL_DCDC2),0,
            session_deadline(),&vd.rail);
        if(r==A733_RAIL_BUSY&&timely()){pause_us(50);continue;}
        if(r!=A733_RAIL_READY&&r!=A733_RAIL_PENDING)return refuse(VD_STEP_RAIL,r);
    }
    for(;;){
        enum a733_rail_result r=a733_rail_poll(&providers.shared.rails,&vd.rail);
        if(r==A733_RAIL_READY)break;
        if((r!=A733_RAIL_BUSY&&r!=A733_RAIL_PENDING)||!timely())return refuse(VD_STEP_RAIL,r);
        pause_us(50);
    }
    const struct os_session *s=os_session_admitted();
    if(!s)return refuse(VD_STEP_CONFIG,0);
    struct a733_video_platform_config *c=&vd.config;
    if(!c->owner){
        *c=(struct a733_video_platform_config){.clocks=&providers.shared.clocks,.power=&providers.shared.gpu,
            .owner=VD_OWNER,.handoff=s->board.generation,.module=VD_MODULE,.module_hz=VD_MODULE_HZ,
            .start=A733_VIDEO_COLD_NS_ROM,.row=&vd.row};
        memcpy(c->fabric,s->board.fabric,sizeof(c->fabric));memcpy(c->fabric_hz,s->board.fabric_hz,sizeof(c->fabric_hz));
    }
    a733_clock_mask_t scopes;
    if(!a733_video_platform_scopes(c,&scopes))return refuse(VD_STEP_CONFIG,1);
    while(!c->clock.id){
        enum a733_clock_result r=a733_clock_acquire(c->clocks,VD_OWNER,scopes,a733_video_platform_exclusive(c),
            session_deadline(),&c->clock);
        if(r==A733_CLOCK_BUSY&&timely()){pause_us(50);continue;}
        if(r!=A733_CLOCK_OK)return refuse(VD_STEP_CLOCK,r);
    }
    while(!vd.pck_token){
        vd.pck_client=pck_client();
        if(vd.pck_client>=A733_GPU_RESOURCE_CLIENTS)return refuse(VD_STEP_PCK,0xff);
        struct a733_gpu_power_resources ready={0};
        enum a733_gpu_resource_result r=a733_gpu_resources_acquire(c->power,vd.pck_client,
            A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,session_deadline(),&vd.pck_token,&ready);
        if(r==A733_GPU_RESOURCE_BUSY&&timely()){vd.pck_token=0;pause_us(50);continue;}
        if(r==A733_GPU_RESOURCE_FAULT)return fail(VD_STEP_PCK,r|vd.pck_client<<8);
        if(r!=A733_GPU_RESOURCE_OK){vd.pck_token=0;return refuse(VD_STEP_PCK,r|vd.pck_client<<8);}
    }
    c->power_client=vd.pck_client;
    c->pck=(struct a733_gpu_resource_lease){vd.pck_token,A733_GPU_RESOURCE_OSC|A733_GPU_RESOURCE_PCK,
        A733_GPU_RESOURCE_STABLE};
    if(!vd.initialized){
        /* The decoder's authority is the session's grant of the device to this owner. The supply is the power-domain
         * owner's: it answers the domain-2 row ON only with DCDC2 verified at 800 mV and this task's rail token
         * pinned. The decoder masters touch no memory in this stage. */
        static const struct a733_video_unit_ops ops={op_now,op_enter,op_leave,a733_video_platform_native_read,
            a733_video_platform_native_write,op_spin_now};
        /* The binding's init checks the pages once, outside every hold; a page that fails the check refuses the
         * service here, before any access. */
        if(!a733_video_unit_native_map())return refuse(VD_STEP_CONFIG,3);
        if(!a733_video_decoder_init(&vd.decoder,c,&ops,NULL))return refuse(VD_STEP_CONFIG,2);
        vd.initialized=1;
    }
    return 1;
}
/* The stage-1 record: the binding's cold record and identity, and domain 2's transition as the power-domain owner
 * published it with the row's latest answer (struct a733_power_domain_row, transition). */
static void copy_record(void)
{
#ifdef SUNSTONE_VIDEO_DECODE_MEASURE
    const struct a733_video_decoder *p=&vd.decoder;
    struct vdec_check_record *i=&vd.info;
    i->cold=p->cold;
    i->feature=p->unit.identity[0];i->identity_high=p->unit.identity[1];i->identity_low=p->unit.identity[2];
    i->version_high=(uint32_t)(p->version>>32);i->version_low=(uint32_t)p->version;
    i->cold_phase=p->cold_step;
    i->failed=p->unit.failed|p->unit.locked<<1|p->unit.uncertain<<2|p->unit.late<<3;
    i->fault_step=vd.fault_step;i->fault_value=vd.fault_value;
    i->measured=providers.domains.measure_transitions;
    if(a733_power_domain_state(&vd.row)!=A733_POWER_ROW_PENDING)i->transition=vd.row.transition;
    if(a733_power_authority_load(&providers.shared.rails.authority)&A733_POWER_DCDC2_VERIFIED){
        i->owner_started_us=providers.domains.started_at;i->owner_verified_us=providers.domains.verified_at;
    }
#endif
}
/* A fault after the first write (docs/video-decode-os.md, "Faults after the first write"): the service enters FAULT
 * with the binding's failing access in the record, keeps its leases and gates, and starts nothing new. Its close, from
 * the application or the quiesce, then runs once (do_close). */
static int fault(unsigned step,uint32_t value)
{
    RECORD(const struct a733_video_unit *u=&vd.decoder.unit;
        vd.info.refused_address=(uint32_t)u->last_address;vd.info.refused_word=u->last_word;)
    copy_record();
    return fail(step,value);
}
static int do_open(void)
{
    atomic_store(&vd.phase,OPENING);
    atomic_store(&vd.close_started,0);
    RECORD(memset(&vd.info,0,sizeof(vd.info));vd.info.magic=VDEC_CHECK_INFO_MAGIC;vd.info.bytes=sizeof(vd.info);
        vd.info.requested_ticks=ss_counter_ticks();)
    vd.fault_step=0;vd.fault_value=0;
    vd.limit=lease_now()+VD_CALL_US;
    if(vd.limit>session_deadline())vd.limit=session_deadline();
    if(tokens()<0)goto refused;
    RECORD(if(!read_words(vd.info.entry)){refuse(VD_STEP_ENTRY_WORDS,0);goto refused;})
    /* The binding's entry hold, its row request, then hold A, the lock wait and hold B in one call after the ON
     * answer, then the identity. The owner answers on the row's wake, which notifies this task; the wait blocks until
     * that or the bound. At the binding's deadline, vd.limit, its next call fails the open. */
    RECORD(vd.info.row_request_us=owner_us();)
    uint64_t supply_bound=0,verified_bound=0;
    for(;;){
        const int r=a733_video_decoder_open(&vd.decoder,vd.limit);
        const struct a733_video_decoder *p=&vd.decoder;
        const enum a733_power_row_state s=a733_power_domain_state(&vd.row);
        RECORD(if(s!=A733_POWER_ROW_IDLE&&s!=A733_POWER_ROW_PENDING&&!vd.info.row_state){
            vd.info.row_state=s;vd.info.row_on_us=owner_us();
            vd.info.authority=a733_power_authority_load(&providers.shared.rails.authority);
        })
        if(r==1)break;
        if(r==A733_VIDEO_OPEN_REFUSED){
            /* Nothing written: the decoder is as it was. A refusal at the binding's deadline leaves it not failed;
             * with the row still pending, that is the row's timeout, as at the service's own bound below. */
            const unsigned step=s==A733_POWER_ROW_IDLE?VD_STEP_ENTRY:
                s==A733_POWER_ROW_PENDING&&!p->unit.failed?VD_STEP_ROW_TIMEOUT:VD_STEP_ROW;
            copy_record();refuse(step,p->cold_step|s<<8);goto refused;
        }
        if(r<0)return fault(VD_STEP_COLD,p->cold_step|s<<8);
        if(s==A733_POWER_ROW_PENDING){
            const uint64_t bound=row_bound(&supply_bound,&verified_bound),t=owner_us();
            /* The row is requested only after the entry hold, which writes nothing, and the decoder writes only
             * after the ON answer: an unanswered row leaves nothing to close. */
            if(t>=bound){
                copy_record();refuse(VD_STEP_ROW_TIMEOUT,p->cold_step|s<<8);
                /* Nothing written: the binding drops the open's deadline, so a later open starts with its own. */
                if(a733_video_decoder_cancel(&vd.decoder)!=A733_VIDEO_OPEN_REFUSED)return fault(VD_STEP_COLD,p->cold_step|s<<8);
                goto refused;
            }
            /* The row's wake, which the owner also calls when verification ends, notifies this task. */
            const uint64_t at=lease_now();uint64_t wait=bound-t;
            if(at>=vd.limit)wait=0;else if(vd.limit-at<wait)wait=vd.limit-at;
            (void)sunstone_deadline_wait_until(now_us()+wait);
        }else{
            /* A busy registry or register lock after the binding's own bounded retry: block, then call again. */
            pause_us(VD_BUSY_US);
        }
    }
    RECORD(vd.info.started_ticks=ss_counter_ticks();
        if(!read_words(vd.info.started))return fault(VD_STEP_STARTED_WORDS,0);)
    copy_record();
    atomic_store(&vd.phase,OPEN);
    return 1;
refused:
    atomic_store(&vd.phase,CLOSED);
    return -1;
}
/* The close of an open service, and the one close of a FAULT service
 * (docs/designs/failure-return-audit.md, "A faulted device service closes before it reports"): the binding's vendor
 * order, module gate, bus gate, reset, then PLL_VE0, each write read back. The close writes only CCU words, so no
 * decoder register is touched whatever state the fault left. A FAULT service's close keeps the first fault in the
 * record and, when every step reads back, ends CLOSED; a step that fails leaves it FAULT. */
static int do_close(void)
{
    const unsigned faulted=atomic_load(&vd.phase)==FAULT;
    atomic_store(&vd.close_started,1);
    atomic_store(&vd.phase,CLOSING);
    vd.limit=lease_now()+VD_CALL_US;
    if(vd.limit>session_deadline())vd.limit=session_deadline();
    RECORD(const uint32_t at_fault=vd.info.failed;)
    for(;;){
        const int r=a733_video_decoder_close(&vd.decoder,vd.limit,faulted);
        if(r==1)break;
        if(r<0||!timely()){
            copy_record();RECORD(if(faulted)vd.info.failed=at_fault;)
            return fail(VD_STEP_CLOSE,(uint32_t)r);
        }
        if(faulted)pause_us(VD_BUSY_US);
    }
    RECORD(vd.info.fault_closed=faulted;
        if(!read_words(vd.info.closed)){copy_record();return fail(VD_STEP_CLOSED_WORDS,0);}
        vd.info.closed_ticks=ss_counter_ticks();)
    copy_record();
    RECORD(if(faulted)vd.info.failed=at_fault;)
    vd.closed=1;
    atomic_store(&vd.phase,CLOSED);
    return faulted?-1:1;
}
/* An open service, or a FAULT one whose close has not started. */
static int closable(unsigned phase){return phase==OPEN||(phase==FAULT&&!atomic_load(&vd.close_started));}
static void serve(void)
{
    if(atomic_load_explicit(&vd.request,memory_order_acquire)!=POSTED)return;
    const unsigned phase=atomic_load(&vd.phase);
    int r=-1;
    if(vd.op==OPEN_OP)r=phase==CLOSED&&!vd.closed?do_open():phase==OPEN?1:-1;
    else if(vd.op==CLOSE_OP)r=closable(phase)?do_close():phase==CLOSED?1:-1;
    vd.result=r;
    atomic_store_explicit(&vd.request,DONE,memory_order_release);
    os_application_wake_task(vd.waiter);
}
static void os_video_decode_task(void *unused)
{
    (void)unused;
    if(sunstone_deadline_register()!=1)for(;;)vTaskSuspend(NULL);
    for(;;){
        serve();
        if(atomic_load(&vd.quiesce_wanted)&&atomic_load(&vd.request)!=POSTED){
            if(closable(atomic_load(&vd.phase)))(void)do_close();
            if(atomic_load(&vd.phase)==CLOSED)atomic_store(&vd.quiesced,1);
        }
        /* Every input notifies this task: an application request, the quiesce and the row's wake. */
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    }
}

/* The handler never runs in this stage: INTID 81 stays disabled. */
void os_video_decode_irq(void){}

int os_video_decode_task_privileged(void (*entry)(void *),void *context){return !context&&entry==os_video_decode_task;}
int os_video_decode_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
/* A FAULT service first gets its close (docs/designs/failure-return-audit.md, "A faulted device service closes before
 * it reports"): DONE once it is CLOSED, FAULT (site 0x941) when a close step failed or the failure was uncertain.
 *
 * This step runs on the supervisor's core while the owner task closes, so it decides from the one word the owner
 * publishes, reported, and from quiesced. Deciding from phase and close_started together is racy in either load
 * order: two loads can fall on either side of the close's stores, and the step then reports 0x941 while a close that
 * will succeed is running; the sticky report ends the session at 0x742. */
static enum os_quiesce_result quiesce_step(struct os_quiesce *q)
{
    if(atomic_load_explicit(&vd.reported,memory_order_acquire)){
        (void)os_quiesce_fault(q,SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|0x941,
            vd.fault_step<<24|(vd.fault_value&0xffffff),SUNSTONE_CRUMB_HERE());
        return OS_QUIESCE_FAULT;
    }
    if(atomic_load(&vd.quiesced))return OS_QUIESCE_DONE;
    atomic_store(&vd.quiesce_wanted,1);
    wake_owner(NULL);
    return OS_QUIESCE_PENDING;
}
static struct os_quiesce video_decode_quiesce={.owner=OS_QUIESCE_VIDEO_DECODE,.order=OS_QUIESCE_DEVICES,.step=quiesce_step};
int os_video_decode_start(void)
{
    const struct ss_grant *grant=ss_grant();
    if(!grant||!(grant->devices&SS_DEVICE_VIDEO_DECODE))return 1;
    if(!os_session_admitted()||!os_quiesce_register(&video_decode_quiesce))return 0;
    /* The row is registered before the power service first runs; the rail token is acquired by the owner task before
     * the first request. */
    if(!a733_power_domain_row_init(&vd.row,&providers.domains,2,VD_OWNER,&vd.rail))return 0;
    vd.row.wake=wake_owner;
#ifdef SUNSTONE_VIDEO_DECODE_MEASURE
    /* Stage 1 only (docs/power-domains.md, "A transition record sizes the 50 us spin"): the owner reads status back to
     * back from each command to its target or the 10,000 us bound. */
    if(!a733_power_domains_measure_transitions(&providers.domains))return 0;
#endif
    owner=xTaskCreateStaticAffinitySet(os_video_decode_task,"os-video-decode",sizeof(owner_stack)/sizeof(owner_stack[0]),
        NULL,VD_PRIORITY,owner_stack,&owner_tcb,1u<<VD_CPU);
    return owner!=NULL;
}

/* --- application calls ---------------------------------------------------------------------------------------------- */
static int granted(void){const struct ss_grant *g=ss_grant();return g&&(g->devices&SS_DEVICE_VIDEO_DECODE)&&owner;}
static int call(unsigned op)
{
    if(!granted()||atomic_load(&vd.quiesce_wanted))return -1;
    const unsigned state=atomic_load_explicit(&vd.request,memory_order_acquire);
    if(state==DONE){
        if(vd.op!=op)return -1;
        const int r=vd.result;
        atomic_store_explicit(&vd.request,0,memory_order_release);
        return r;
    }
    if(state==POSTED)return vd.op==op?0:-1;
    vd.op=op;vd.waiter=xTaskGetCurrentTaskHandle();
    atomic_store_explicit(&vd.request,POSTED,memory_order_release);
    xTaskNotifyGive(owner);
    return 0;
}
int ss_video_decode_open(void){return call(OPEN_OP);}
int ss_video_decode_close(void){return call(CLOSE_OP);}
#ifdef SUNSTONE_VIDEO_DECODE_MEASURE
int vdec_check_record(struct vdec_check_record *out)
{
    if(!granted()||!out)return -1;
    if(atomic_load(&vd.request)==POSTED||vd.info.magic!=VDEC_CHECK_INFO_MAGIC)return 0;
    *out=vd.info;return 1;
}
#endif
int ss_video_decode_fault(uint32_t *step,uint32_t *value)
{
    if(!granted())return -1;
    if(step)*step=vd.fault_step;
    if(value)*value=vd.fault_value;
    return vd.fault_step?1:0;
}
static int ready(void)
{const unsigned r=atomic_load(&vd.request);return r==DONE||(r!=POSTED&&atomic_load(&vd.phase)==FAULT);}
void ss_video_decode_wait(uint32_t wait_us)
{
    const uint64_t until=now_us()+(wait_us?wait_us:1);
    if(os_application_wait_unless(until,ready)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x7b1);
}
