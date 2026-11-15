/* Video decoder service (SS_DEVICE_VIDEO_DECODE, include/sunstone_video_decode.h, docs/video-decode-os.md). One owner
 * task, priority 3 pinned to CPU 4, does every register access through the decoder platform binding
 * (drivers/video/a733_video_platform.c). Open requests PCK600 domain 2 through the shared power-domain owner, then the
 * binding's cold start opens the decoder's MBUS clock gate, releases reset, opens the bus gate, programs PLL_VE0 to
 * 624 MHz and opens the module gate; open then reads the decoder identity. Close is the binding's quiesce: module gate,
 * bus gate, reset, then PLL_VE0 disabled; domain 2 stays ON. Stage 1 runs no job, so INTID 81 stays disabled and its
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

#define CCU UINT32_C(0x02002000)
#define R_CCU UINT32_C(0x07010000)
#define RTC UINT32_C(0x07090000)
#define GICD UINT32_C(0x03400000)
#define VD_INTID 81u
#define VD_OWNER UINT64_C(0x5645440000000001) /* "VED", lease owner of the rail, clocks and domain row */
#define VD_TOKEN UINT64_C(0x5645440000000002) /* the binding's admission token */
#define VD_MODULE UINT32_C(0x81000000)         /* CCU 0xa88: gate, mux 1 (PLL_VE0), divider 1 */
#define VD_MODULE_HZ UINT32_C(624000000)       /* the vendor point, A733_VIDEO_VENDOR_HZ */
#define VD_CPU 4u                              /* an A55 core: not core 0 (tick, SPIs, PMIC task), not 6 or 7 */
#define VD_PRIORITY 3u                         /* scheduler band 1 */
/* Bound of one open or close, inside the session deadline: the supply verification (5,160 + 100,000 us after provider
 * start), the domain transition (10,000 us), the PLL lock wait (about 420 us) and the register sequence, with margin.
 * The binding's deadline is fixed for the whole cold start. */
#define VD_CALL_US 500000u
enum phase {CLOSED,OPENING,OPEN,CLOSING,FAULT};
enum op {NONE,OPEN_OP,CLOSE_OP};
enum {POSTED=1,DONE=2};
/* Fault steps (ss_video_decode_fault). Steps below VD_STEP_WRITES write nothing, so a refusal there leaves the decoder
 * as it was. */
enum {
    VD_STEP_NONE, VD_STEP_RAIL, VD_STEP_CLOCK, VD_STEP_PCK, VD_STEP_SUPPLY, VD_STEP_CONFIG, VD_STEP_ENTRY_WORDS,
    VD_STEP_ENTRY, VD_STEP_ROW, VD_STEP_ROW_TIMEOUT, VD_STEP_WRITES=16, VD_STEP_COLD, VD_STEP_STARTED_WORDS,
    VD_STEP_IDENTITY, VD_STEP_CLOSE=32, VD_STEP_CLOSED_WORDS
};
static struct {
    struct a733_power_domain_row row;
    struct a733_rail_token rail;
    struct a733_video_platform_config config;
    struct a733_video_platform platform;
    uint64_t pck_token,token;
    unsigned pck_client,initialized,closed; /* closed: the decoder is closed for this power lifetime */
    struct ss_video_decode_info info;
    atomic_uint phase,request,quiesce_wanted,quiesced;
    unsigned op;int result;
    uint32_t fault_step,fault_value;
    uint64_t limit;
    TaskHandle_t waiter;
} vd;
static TaskHandle_t owner;
static StaticTask_t owner_tcb;
static StackType_t owner_stack[2048];
static const struct a733_video_jpeg_job no_job;

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
    atomic_store(&vd.phase,FAULT);return -1;
}
static void wake_owner(void *unused){(void)unused;if(owner)xTaskNotifyGive(owner);}

/* --- the binding's native callbacks ----------------------------------------------------------------------------- */
static int op_now(void *cookie,uint64_t *out){(void)cookie;*out=lease_now();return *out?1:-1;}
/* The cold start's authority: this task holds the DCDC2 token, the owner has verified DCDC2 at 800 mV, and the
 * session granted the device. The decoder masters touch no memory in stage 1, so no memory authority is given. */
static int op_admit(void *cookie,const struct a733_video_platform_config *c,const struct a733_video_jpeg_job *j,
    uint64_t d,uint64_t *token)
{
    (void)cookie;(void)j;(void)d;
    *token=VD_TOKEN;
    return c==&vd.platform.decoder.config&&c->owner==VD_OWNER&&
        a733_rail_status(&providers.shared.rails,&vd.rail)==A733_RAIL_READY&&
        (a733_power_authority_load(&providers.shared.rails.authority)&A733_POWER_DCDC2_VERIFIED)?1:-1;
}
static int op_memory(void *cookie,uint64_t t,const struct a733_video_jpeg_job *j,const struct a733_video_jpeg *p,
    uint32_t stride,uint32_t rows,uint64_t d)
{(void)cookie;(void)t;(void)j;(void)p;(void)stride;(void)rows;(void)d;return -1;}
static int op_drain(void *cookie,uint64_t t,uint64_t d){(void)cookie;(void)t;(void)d;return -1;}
static int op_enter(void *cookie,uint64_t d)
{(void)cookie;return a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d)==1?1:0;}
static int op_leave(void *cookie){(void)cookie;a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);return 1;}

static const uintptr_t words[SS_VIDEO_DECODE_WORDS]={
    [SS_VIDEO_DECODE_CCU_PLL_REF]=CCU,[SS_VIDEO_DECODE_RTC_OSC]=RTC+0x160,[SS_VIDEO_DECODE_CCU_AHB]=CCU+0x500,
    [SS_VIDEO_DECODE_CCU_NSI]=CCU+0x580,[SS_VIDEO_DECODE_CCU_MBUS]=CCU+0x588,[SS_VIDEO_DECODE_CCU_PLL_VE0]=CCU+0x220,
    [SS_VIDEO_DECODE_CCU_PLL_VE0_P0]=CCU+0x228,[SS_VIDEO_DECODE_CCU_PLL_VE0_P1]=CCU+0x22c,
    [SS_VIDEO_DECODE_CCU_GATES]=CCU+0x1918,[SS_VIDEO_DECODE_CCU_STATUS]=CCU+0x1998,[SS_VIDEO_DECODE_CCU_AUTO]=CCU+0x1b10,
    [SS_VIDEO_DECODE_CCU_MODULE]=CCU+0xa88,[SS_VIDEO_DECODE_CCU_BGR]=CCU+0xa8c,[SS_VIDEO_DECODE_CCU_AHB_GATES]=CCU+0x5c0,
    [SS_VIDEO_DECODE_CCU_MBUS_GATES]=CCU+0x5e0,[SS_VIDEO_DECODE_CCU_MCLK_GATES]=CCU+0x5e4,
    [SS_VIDEO_DECODE_RCCU_PCK]=R_CCU+0x1ac,[SS_VIDEO_DECODE_GICD_ENABLE]=GICD+0x100+4*(VD_INTID/32),
    [SS_VIDEO_DECODE_GICD_PENDING]=GICD+0x200+4*(VD_INTID/32),[SS_VIDEO_DECODE_GICD_ACTIVE]=GICD+0x300+4*(VD_INTID/32)};
/* The recorded words, read under the register lock through the binding's read allowlist; read only. */
static int read_words(uint32_t *out)
{
    if(!timely()||op_enter(NULL,vd.limit)!=1)return 0;
    unsigned ok=1;
    for(unsigned i=0;i<SS_VIDEO_DECODE_WORDS&&ok;++i)ok=a733_video_platform_native_read(NULL,words[i],&out[i])==1;
    (void)op_leave(NULL);
    return ok;
}

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
    /* The power-domain owner verifies DCDC2 enabled at 800 mV once per boot and wakes every registered row when it
     * publishes the result. The service reads only the verified bit and writes no PMIC register. */
    const uint64_t until=now_us()+VD_CALL_US;
    for(;;){
        const unsigned w=a733_power_authority_load(&providers.shared.rails.authority);
        if(w&A733_POWER_DCDC2_VERIFIED)break;
        if((w&A733_POWER_SUPPLY_DONE)||now_us()>=until||!timely())return refuse(VD_STEP_SUPPLY,w);
        (void)sunstone_deadline_wait_until(until);
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
        static const struct a733_video_platform_ops ops={op_now,op_admit,op_memory,op_drain,op_enter,op_leave,
            a733_video_platform_native_read,a733_video_platform_native_write};
        if(!a733_video_platform_init(&vd.platform,c,&ops,NULL))return refuse(VD_STEP_CONFIG,2);
        vd.initialized=1;
    }
    return 1;
}
static void copy_record(void)
{
    const struct a733_video_decoder *p=&vd.platform.decoder;
    const struct a733_video_cold_record *r=&p->cold;
    struct ss_video_decode_info *i=&vd.info;
    for(unsigned k=0;k<3;++k){i->gates_entry[k]=r->entry_gates[k];i->gates_a1[k]=r->gates[k];}
    i->mclk_written=r->mclk_written;i->mclk_readback=r->mclk_readback;i->lock_reads=r->lock_reads;
    i->entry_at=r->entry_at;i->entered_at=r->entered_at;i->on_at=r->on_at;i->hold_a_at=r->hold_a_at;
    i->locked_at=r->locked_at;i->hold_b_at=r->hold_b_at;
    i->version_high=(uint32_t)(p->version>>32);i->version_low=(uint32_t)p->version;
    i->cold_phase=p->cold_step;i->failed=p->unit.failed|p->unit.locked<<1;
    i->measured=providers.domains.measure_transitions;
    for(unsigned k=0;k<SS_VIDEO_DECODE_DOMAINS&&k<A733_POWER_DOMAINS;++k){
        const struct a733_power_transition *t=&providers.domains.transition[k];
        i->transition[k]=(struct ss_video_decode_transition){providers.domains.domain[k].id,t->policy_before,
            t->status_before,t->policy_written,t->first_word,t->pending_word,t->reached_word,t->all_and,t->all_or,
            t->reads,t->spin_reads,t->commands,t->command_at,t->first_at,t->pending_at,t->reached_at};
    }
}
static int do_open(void)
{
    atomic_store(&vd.phase,OPENING);
    memset(&vd.info,0,sizeof(vd.info));
    vd.info.magic=SS_VIDEO_DECODE_INFO_MAGIC;vd.info.bytes=sizeof(vd.info);
    vd.info.requested_ticks=ss_counter_ticks();
    vd.fault_step=0;vd.fault_value=0;
    vd.limit=lease_now()+VD_CALL_US;
    if(vd.limit>session_deadline())vd.limit=session_deadline();
    if(tokens()<0)goto refused;
    if(!read_words(vd.info.entry)){refuse(VD_STEP_ENTRY_WORDS,0);goto refused;}
    /* The binding's entry hold, its row request, then hold A, the lock wait and hold B in one call after the ON
     * answer. The owner answers on the row's wake, which notifies this task; the wait blocks until that or the bound. */
    const struct a733_video_jpeg_ops *ops=a733_video_platform_jpeg_ops();
    vd.info.row_request_us=owner_us();
    uint64_t row_limit=0;
    for(;;){
        const int r=ops->acquire(&vd.platform,&no_job,vd.limit,&vd.token);
        const struct a733_video_decoder *p=&vd.platform.decoder;
        const enum a733_power_row_state s=a733_power_domain_state(&vd.row);
        if(p->requested&&s!=A733_POWER_ROW_PENDING&&!vd.info.row_state){
            vd.info.row_state=s;vd.info.row_on_us=owner_us();
            vd.info.authority=a733_power_authority_load(&providers.shared.rails.authority);
        }
        if(r==1)break;
        if(r<0){
            copy_record();
            /* Nothing written before hold A: a refusal there leaves the decoder as it was. */
            if(p->cold_step<2u){refuse(p->requested?VD_STEP_ROW:VD_STEP_ENTRY,p->cold_step|s<<8);goto refused;}
            return fail(VD_STEP_COLD,p->cold_step|s<<8);
        }
        if(!timely()){copy_record();return fail(VD_STEP_ROW_TIMEOUT,p->cold_step|s<<8);}
        if(s==A733_POWER_ROW_PENDING&&p->requested){
            if(!row_limit)row_limit=now_us()+A733_POWER_PCK_BOUND_US+4000;
            if(now_us()>=row_limit){copy_record();return fail(VD_STEP_ROW_TIMEOUT,p->cold_step|s<<8);}
            (void)sunstone_deadline_wait_until(row_limit);
        }
    }
    vd.info.started_ticks=ss_counter_ticks();
    if(!read_words(vd.info.started)){copy_record();return fail(VD_STEP_STARTED_WORDS,0);}
    /* The identity words through the binding's read call, then its identity admission (prepare's first call). */
    if(ops->read(&vd.platform,vd.token,0xf0,&vd.info.feature,vd.limit)!=1||
       ops->read(&vd.platform,vd.token,0xe0,&vd.info.identity_high,vd.limit)!=1||
       ops->read(&vd.platform,vd.token,0xe4,&vd.info.identity_low,vd.limit)!=1){copy_record();return fail(VD_STEP_IDENTITY,1);}
    if(ops->prepare(&vd.platform,vd.token,&no_job,NULL,0,0,vd.limit)!=0||!vd.platform.decoder.version){
        copy_record();return fail(VD_STEP_IDENTITY,2);
    }
    copy_record();
    atomic_store(&vd.phase,OPEN);
    return 1;
refused:
    atomic_store(&vd.phase,CLOSED);
    return -1;
}
static int do_close(void)
{
    atomic_store(&vd.phase,CLOSING);
    vd.limit=lease_now()+VD_CALL_US;
    if(vd.limit>session_deadline())vd.limit=session_deadline();
    for(;;){
        const int r=a733_video_decoder_quiesce(&vd.platform.decoder,vd.limit);
        if(r==1)break;
        if(r<0||!timely()){copy_record();return fail(VD_STEP_CLOSE,(uint32_t)r);}
    }
    if(!read_words(vd.info.closed)){copy_record();return fail(VD_STEP_CLOSED_WORDS,0);}
    vd.info.closed_ticks=ss_counter_ticks();
    copy_record();
    vd.closed=1;
    atomic_store(&vd.phase,CLOSED);
    return 1;
}
static void serve(void)
{
    if(atomic_load_explicit(&vd.request,memory_order_acquire)!=POSTED)return;
    const unsigned phase=atomic_load(&vd.phase);
    int r=-1;
    if(vd.op==OPEN_OP)r=phase==CLOSED&&!vd.closed?do_open():phase==OPEN?1:-1;
    else if(vd.op==CLOSE_OP)r=phase==OPEN?do_close():phase==CLOSED?1:-1;
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
            if(atomic_load(&vd.phase)==OPEN)(void)do_close();
            if(atomic_load(&vd.phase)==CLOSED)atomic_store(&vd.quiesced,1);
        }
        (void)sunstone_deadline_wait_until(now_us()+10000000);
    }
}

/* The handler never runs in this stage: INTID 81 stays disabled. */
void os_video_decode_irq(void){}

int os_video_decode_task_privileged(void (*entry)(void *),void *context){return !context&&entry==os_video_decode_task;}
int os_video_decode_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
static enum os_quiesce_result quiesce_step(struct os_quiesce *q)
{
    if(atomic_load(&vd.phase)==FAULT){
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
static int with_info(int r,struct ss_video_decode_info *info)
{
    if(r!=0&&info&&vd.info.magic==SS_VIDEO_DECODE_INFO_MAGIC)*info=vd.info;
    return r;
}
int ss_video_decode_open(struct ss_video_decode_info *info){return with_info(call(OPEN_OP),info);}
int ss_video_decode_close(struct ss_video_decode_info *info){return with_info(call(CLOSE_OP),info);}
int ss_video_decode_fault(uint32_t *step,uint32_t *value)
{
    if(!granted())return -1;
    if(step)*step=vd.fault_step;
    if(value)*value=vd.fault_value;
    return vd.fault_step?1:0;
}
static int ready(void){return atomic_load(&vd.request)==DONE||atomic_load(&vd.phase)==FAULT;}
void ss_video_decode_wait(uint32_t wait_us)
{
    const uint64_t until=now_us()+(wait_us?wait_us:1);
    if(os_application_wait_unless(until,ready)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x7b1);
}
