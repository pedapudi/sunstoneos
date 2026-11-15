/* Video decoder service (SS_DEVICE_VIDEO_DECODE, include/sunstone_video_decode.h, docs/video-decode-os.md). One owner
 * task, priority 3 pinned to CPU 4, does every register access through the decoder platform binding
 * (drivers/video/a733_video_platform.c) and its JPEG executor (drivers/video/a733_video_jpeg_run.c). Open is the
 * binding's a733_video_decoder_open: it requests PCK600 domain 2 through the shared power-domain owner, and its cold
 * start opens the decoder's MBUS clock gate, releases reset, opens the bus gate, programs PLL_VE0 to 624 MHz and opens
 * the module gate; it then reads the decoder identity. A decode runs one JPEG picture through the executor: the owner's
 * guarded buffers mapped for masters 6 and 7, completion on INTID 81, the drain, then the copy of the active pixels.
 * Close masks INTID 81, detaches the masters and runs the binding's a733_video_decoder_close: module gate, bus gate,
 * reset, then PLL_VE0 disabled; domain 2 stays ON. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "a733_gpu_resources.h"
#include "a733_platform_locks.h"
#include "a733_power_domains.h"
#include "a733_video_platform.h"
#include "device-fault.h"
#include "dma_address_width.h"
#include "gicv3_spi.h"
#include "iommu.h"
#include "owner-memory.h"
#include "app_large.h"
#include "a733_storage_digest.h"
#include "quiesce.h"
#include "sunstone_crumb.h"
#include "sunstone_deadline.h"
#include "sunstone_notify.h"
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
#define VE UINT32_C(0x01c0e000)
/* The bound on the completion interrupt after the trigger, inside the decode's own VD_CALL_US. */
#define VD_JOB_US 100000u
#define VD_GUARD 4096u     /* an unmapped page on each side of every decoder buffer, in memory and in device addresses */
#define VD_POISON 0xa5u
#define VD_TOKEN VD_OWNER  /* the binding's admission token: the session's grant of the device to this owner */
enum op {OPEN_OP=1,CLOSE_OP,DECODE_OP};
enum {POSTED=1,DONE=2};
/* Fault steps (ss_video_decode_fault). Steps below VD_STEP_WRITES write nothing, so a refusal there leaves the decoder
 * as it was. */
enum {
    VD_STEP_NONE, VD_STEP_RAIL, VD_STEP_CLOCK, VD_STEP_PCK, VD_STEP_SUPPLY, VD_STEP_CONFIG, VD_STEP_ENTRY_WORDS,
    VD_STEP_ENTRY, VD_STEP_ROW, VD_STEP_ROW_TIMEOUT,
    /* A decode refused before the executor starts: the decoder and the IOMMU are as the open left them. */
    VD_STEP_DECODE, VD_STEP_DECODE_INPUT, VD_STEP_DECODE_OUTPUT, VD_STEP_DECODE_MEMORY, VD_STEP_DECODE_IRQ,
    VD_STEP_WRITES=16, VD_STEP_COLD, VD_STEP_STARTED_WORDS,
    /* A decode that failed once the executor started (value: its state << 16 | index, or the word named). */
    VD_STEP_DECODE_RUN, VD_STEP_DECODE_GIC, VD_STEP_DECODE_TIMEOUT, VD_STEP_DECODE_STATUS, VD_STEP_DECODE_DRAIN,
    VD_STEP_DECODE_IOMMU_FAULT, VD_STEP_DECODE_GUARD,
    VD_STEP_CLOSE=32, VD_STEP_CLOSED_WORDS, VD_STEP_CLOSE_GIC, VD_STEP_CLOSE_DETACH
};
/* One decoder buffer: an allocation of a guard page, the mapped pages and a guard page, and its device addresses,
 * likewise reserved with an unmapped page on each side. */
struct vd_buffer {uint8_t *cpu;uint64_t iova;uint32_t mapped,used;};
const struct large_slot *os_large_slot(unsigned index);
static TaskHandle_t owner;
static struct {
    struct a733_power_domain_row row;
    struct a733_rail_token rail;
    struct a733_video_platform_config config;
    struct a733_video_platform platform;
    struct a733_video_jpeg_run run;
    struct ss_video_decode_jpeg job;
    struct ss_video_decode_done done;
    struct vd_buffer buffer[3]; /* input, luma, chroma */
    struct gicv3_spi gic;
    uint32_t width,height,stride,rows,writes,drain_step,drain_value;
    unsigned decoded,attached;
    atomic_uint armed,irq_seen,irq_count,stray;
    uint64_t irq_ticks;
    uint64_t pck_token;
    unsigned pck_client,initialized,closed; /* closed: the decoder is closed for this power lifetime */
    RECORD(struct vdec_check_record info;)
    /* The phase and the fault report (apps/os/device-fault.h). The fault must be reported once a close step failed,
     * or when the unit's failure was uncertain, which no close settles. */
    struct os_device_fault dev;
    atomic_uint request;
    unsigned op;int result;
    uint64_t limit;
    TaskHandle_t waiter;
} vd={.dev={.site=0x941,.owner=&owner},.gic={GICD,VD_INTID}};
static StaticTask_t owner_tcb;
static StackType_t owner_stack[2048];

static uint64_t now_us(void){return sunstone_deadline_now_us();}
static uint64_t lease_now(void){uint64_t t=0;return a733_counter_clock_now(&providers.shared.counter,&t)==1?t:0;}
static uint64_t owner_us(void){uint64_t t=0;return a733_pmic_now(&providers.shared.pmic,&t)==1?t:0;}
static uint64_t session_deadline(void){return board.session.deadline_us-10000;}
static int timely(void){uint64_t t=lease_now();return t&&t<vd.limit&&board_live(NULL);}
static void pause_us(uint32_t us){const uint64_t t=now_us()+us;while(now_us()<t)(void)sunstone_deadline_wait_until(t);}
static int refuse(unsigned step,uint32_t value){return os_device_refuse(&vd.dev,step,value);}
static int fail(unsigned step,uint32_t value){return os_device_fail(&vd.dev,step,value,vd.platform.decoder.unit.uncertain);}
/* The power row's wake ends the owner's deadline waits (SS_NOTIFY_DEADLINE); the owner's index 0 is for its inputs. */
static void wake_owner(void *unused){(void)unused;if(owner)xTaskNotifyGiveIndexed(owner,SS_NOTIFY_DEADLINE);}
/* The bound on the owner's answer to the row (include/a733_power_domains.h): the supply deadline until verification
 * ends with DCDC2 verified or the done bit, then the row deadline from the first time this task saw verification
 * ended. Each is taken once, when its wait begins, and clipped to the open's deadline. */
static uint64_t row_bound(uint64_t *supply,uint64_t *verified)
{
    const unsigned w=a733_power_authority_load(&providers.shared.rails.authority);
    if(w&(A733_POWER_SUPPLY_DONE|A733_POWER_DCDC2_VERIFIED)){
        if(!*verified)*verified=a733_power_row_deadline(&providers.domains,owner_us(),vd.limit);
        return *verified;
    }
    if(!*supply)*supply=a733_power_supply_deadline(&providers.domains,vd.limit);
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

/* --- the binding's decode callbacks (include/a733_video_platform.h) ------------------------------------------------ */
static int admit(void *cookie,const struct a733_video_platform_config *c,const struct a733_video_jpeg_job *j,
    uint64_t d,uint64_t *token)
{
    (void)cookie;(void)j;(void)d;
    if(!token||(*token&&*token!=VD_TOKEN)||c->owner!=VD_OWNER||!os_session_admitted()||!board_live(NULL))return -1;
    *token=VD_TOKEN;
    return 1;
}
/* Both masters attached with the three buffers, input read-only and the planes write-only, once the executor has
 * parsed the picture the service sized them from. */
static int memory(void *cookie,uint64_t token,const struct a733_video_jpeg_job *j,const struct a733_video_jpeg *pic,
    uint32_t stride,uint32_t rows,uint64_t d)
{
    (void)cookie;(void)d;
    const struct vd_buffer *b=vd.buffer;
    if(token!=VD_TOKEN||pic->width!=vd.width||pic->height!=vd.height||stride!=vd.stride||rows!=vd.rows||
       j->input_iova!=b[0].iova||j->luma_iova!=b[1].iova||j->chroma_iova!=b[2].iova)return -1;
    if(vd.attached)return 1;
    const struct a733_iommu_span spans[3]={
        {b[0].iova,(uintptr_t)b[0].cpu,b[0].mapped,A733_IOMMU_READ},
        {b[1].iova,(uintptr_t)b[1].cpu,b[1].mapped,A733_IOMMU_WRITE},
        {b[2].iova,(uintptr_t)b[2].cpu,b[2].mapped,A733_IOMMU_WRITE}};
    if(os_iommu_attach(A733_VIDEO_DEC_MASTERS,spans,3)!=1)return -1;
    vd.attached=1;
    return 1;
}
/* The executor's finish, after the completion interrupt, the status word with completion, the interrupt enables
 * cleared and completion acknowledged. Stopped: the status word reads the idle value the executor requires before a
 * trigger (no completion, error or busy bit, 0x001c3f0f clear). Drained: the vendor driver ends a job at this
 * interrupt (cedar_ve.c, IOCTL_WAIT_VE_DE) and its library reads the frame with no further step; neither master has
 * an IOMMU fault. The planes are Normal non-cacheable, so no line needs invalidating; a full barrier orders the copy's
 * reads after these checks. */
static int drain(void *cookie,uint64_t token,uint64_t d)
{
    (void)cookie;(void)d;
    if(token!=VD_TOKEN)return -1;
    const int e=op_enter(NULL,vd.limit);
    if(e!=1)return e==0?0:-1;
    uint32_t v=0;
    const int r=a733_video_platform_native_read(NULL,VE+0x11c,&v);
    (void)op_leave(NULL);
    vd.drain_value=v;
    if(r!=1||(v&0x001c3f0fu)){vd.drain_step=VD_STEP_DECODE_DRAIN;return -1;}
    struct a733_iommu_owner_fault f[2];
    int scan[2];
    for(unsigned m=0;m<2;++m)scan[m]=os_iommu_scan(6+m,&f[m]);
    RECORD(vd.info.decode.scan[0]=(uint32_t)scan[0];vd.info.decode.scan[1]=(uint32_t)scan[1];
        _Static_assert(sizeof(f)==sizeof(vd.info.decode.iommu_fault),"the fault words fit the record");
        memcpy(vd.info.decode.iommu_fault,f,sizeof(f));)
    if(scan[0]!=1||scan[1]!=1){
        vd.drain_step=scan[0]==0||scan[1]==0?VD_STEP_DECODE_IOMMU_FAULT:VD_STEP_DECODE_DRAIN;
        vd.drain_value=f[scan[0]==1?1:0].micro_status&0xffffffu;
        return -1;
    }
    __asm__ volatile("dsb sy":::"memory");
    return 1;
}
/* The executor's write, logged: every decoder write of the picture, in order. */
static int logged_write(void *v,uint64_t token,uint32_t off,unsigned width,uint32_t value,uint64_t d)
{
    const int r=a733_video_platform_jpeg_ops()->write(v,token,off,width,value,d);
    if(r==1){
        RECORD(if(vd.writes<VDEC_CHECK_LOG){vd.info.decode.log[vd.writes][0]=off|(uint32_t)width<<24;
            vd.info.decode.log[vd.writes][1]=value;})
        ++vd.writes;
    }
    return r;
}

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
        /* The decoder's authority is the session's grant of the device to this owner (admit). The supply is the
         * power-domain owner's: it answers the domain-2 row ON only with DCDC2 verified at 800 mV and this task's
         * rail token pinned. The masters reach only the decode's three buffers (memory). */
        static const struct a733_video_platform_ops platform_ops={op_now,admit,memory,drain,op_enter,op_leave,
            a733_video_platform_native_read,a733_video_platform_native_write,op_spin_now};
        /* The binding's init checks the pages once, outside every hold; a page that fails the check refuses the
         * service here, before any access. */
        if(!a733_video_unit_native_map())return refuse(VD_STEP_CONFIG,3);
        if(!a733_video_platform_init(&vd.platform,c,&platform_ops,NULL))return refuse(VD_STEP_CONFIG,2);
        vd.initialized=1;
    }
    return 1;
}
/* The stage-1 record: the binding's cold record and identity, and domain 2's transition as the power-domain owner
 * published it with the row's latest answer (struct a733_power_domain_row, transition). */
static void copy_record(void)
{
#ifdef SUNSTONE_VIDEO_DECODE_MEASURE
    const struct a733_video_decoder *p=&vd.platform.decoder;
    struct vdec_check_record *i=&vd.info;
    i->cold=p->cold;
    i->feature=p->unit.identity[0];i->identity_high=p->unit.identity[1];i->identity_low=p->unit.identity[2];
    i->version_high=(uint32_t)(p->version>>32);i->version_low=(uint32_t)p->version;
    i->cold_phase=p->cold_step;
    i->failed=p->unit.failed|p->unit.locked<<1|p->unit.uncertain<<2|p->unit.late<<3;
    i->fault_step=vd.dev.fault_step;i->fault_value=vd.dev.fault_value;
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
    RECORD(const struct a733_video_unit *u=&vd.platform.decoder.unit;
        vd.info.refused_address=(uint32_t)u->last_address;vd.info.refused_word=u->last_word;)
    copy_record();
    return fail(step,value);
}
static int do_open(void)
{
    os_device_open_begin(&vd.dev);
    RECORD(memset(&vd.info,0,sizeof(vd.info));vd.info.magic=VDEC_CHECK_INFO_MAGIC;vd.info.bytes=sizeof(vd.info);
        vd.info.requested_ticks=ss_counter_ticks();)
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
        const int r=a733_video_decoder_open(&vd.platform.decoder,vd.limit);
        const struct a733_video_decoder *p=&vd.platform.decoder;
        const enum a733_power_row_state s=a733_power_domain_state(&vd.row);
        RECORD(if(s!=A733_POWER_ROW_IDLE&&s!=A733_POWER_ROW_PENDING&&!vd.info.row_state){
            vd.info.row_state=s;vd.info.row_seen_us=owner_us();
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
                if(a733_video_decoder_cancel(&vd.platform.decoder)!=A733_VIDEO_OPEN_REFUSED)return fault(VD_STEP_COLD,p->cold_step|s<<8);
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
    atomic_store(&vd.dev.phase,OS_DEVICE_OPEN);
    return 1;
refused:
    atomic_store(&vd.dev.phase,OS_DEVICE_CLOSED);
    return -1;
}
/* --- the decode --------------------------------------------------------------------------------------------------- */
#define VD_INTID_BIT (1u<<(VD_INTID%32u))
/* An application large region holding bytes at offset, writable for the output. */
static const struct large_slot *region(uint32_t index,uint64_t offset,uint32_t bytes,int write)
{
    const struct large_slot *l=os_large_slot(index);
    if(!l||__atomic_load_n(&l->state,__ATOMIC_ACQUIRE)!=SS_LARGE_READY||(write&&!(l->flags&SS_LARGE_WRITE))||!bytes)
        return NULL;
    return offset<=l->bytes&&bytes<=l->bytes-offset?l:NULL;
}
/* Sized from the picture: used bytes in whole pages, between two poisoned guard pages that no master maps. */
static int buffer(struct vd_buffer *b,uint32_t used)
{
    const uint32_t mapped=(used+4095u)&~4095u;
    const uint64_t bytes=(uint64_t)mapped+2u*VD_GUARD;
    uint8_t *a=os_owner_memory_below(bytes,4096,OS_MEMORY_DEVICE,A733_IOMMU_PHYSICAL_LIMIT);
    uint64_t iova=0;
    if(!a||os_iommu_iova(bytes,4096,&iova)!=1)return 0;
    memset(a,VD_POISON,(size_t)bytes);
    *b=(struct vd_buffer){a+VD_GUARD,iova+VD_GUARD,mapped,used};
    return 1;
}
/* 0 when every guard byte, and every plane byte past the padded plane, still holds the poison; otherwise the buffer
 * (1 input, 2 luma, 3 chroma) of the first change, with 0x10 for a plane's tail. */
static uint32_t guards(void)
{
    for(unsigned i=0;i<3;++i){
        const struct vd_buffer *b=&vd.buffer[i];
        const volatile uint8_t *g=b->cpu-VD_GUARD;
        for(uint32_t k=0;k<VD_GUARD;++k)if(g[k]!=VD_POISON||g[VD_GUARD+b->mapped+k]!=VD_POISON)return 1u+i;
        if(i)for(uint32_t k=b->used;k<b->mapped;++k)if(((const volatile uint8_t *)b->cpu)[k]!=VD_POISON)return 0x11u+i;
    }
    return 0;
}
/* INTID 81's enable, pending and active words, read under the register lock. */
static int gic_words(uint32_t w[3])
{
    if(op_enter(NULL,vd.limit)!=1)return 0;
    unsigned ok=1;
    for(unsigned i=0;i<3&&ok;++i)ok=a733_video_platform_native_read(NULL,GICD+0x100u*(i+1u)+4u*(VD_INTID/32u),&w[i])==1;
    (void)op_leave(NULL);
    return ok;
}
static uint32_t decoder_status(void)
{
    uint32_t v=0;
    if(op_enter(NULL,vd.limit)==1){(void)a733_video_platform_native_read(NULL,VE+0x11c,&v);(void)op_leave(NULL);}
    return v;
}
static void decode_record(void)
{
#ifdef SUNSTONE_VIDEO_DECODE_MEASURE
    struct vdec_check_decode *i=&vd.info.decode;
    const struct a733_video_jpeg_run *r=&vd.run;
    i->state=r->state;i->index=r->index;i->status=r->status;i->format_old=r->format_old;
    i->status_after=vd.drain_value;i->writes=vd.writes;
    i->width=vd.width;i->height=vd.height;i->stride=vd.stride;i->rows=vd.rows;i->file_bytes=vd.job.input_bytes;
    for(unsigned k=0;k<3;++k){i->iova[k]=(uint32_t)vd.buffer[k].iova;i->mapped[k]=vd.buffer[k].mapped;}
    i->irq_count=atomic_load(&vd.irq_count);i->stray=atomic_load(&vd.stray);i->seen=atomic_load(&vd.irq_seen);
    i->irq_ticks=vd.irq_ticks;
#endif
}
/* A decode that failed once the executor started: the service enters FAULT, and its close stops the decoder. */
static int decode_fault(unsigned step,uint32_t value)
{
    decode_record();
    return fault(step,value&0xffffffu);
}
static uint32_t at(const struct a733_video_jpeg_run *r){return (uint32_t)r->state<<16|r->index;}
/* Where the executor and the binding's preparation stand: a poll in PREPARE advances the binding's mapping and its
 * reset and default steps without changing the executor's state or index. */
static uint64_t progress(void)
{
    const struct a733_video_jpeg_run *r=&vd.run;const struct a733_video_decoder *p=&vd.platform.decoder;
    return (uint64_t)r->state<<48|(uint64_t)r->index<<24|(uint64_t)p->step<<4|p->mapped<<1|p->prepared;
}
/* Poll the executor until it reaches state and index, or ends: 1 reached, -1 failed (FAULT, or past the deadline). A
 * poll that changes nothing (a busy register lock) blocks 50 us. */
static int advance_to(enum a733_video_jpeg_state state,unsigned index)
{
    struct a733_video_jpeg_run *r=&vd.run;
    while(r->state!=state||r->index!=index){
        const uint64_t before=progress();
        if(r->state==A733_VIDEO_JPEG_DONE||a733_video_jpeg_poll(r)<0)return -1;
        if(progress()==before){if(!timely())return -1;pause_us(50);}
    }
    return 1;
}
/* One JPEG picture (include/sunstone_video_decode.h): the input copied into the owner's guarded buffer and parsed, the
 * planes sized from it, then the executor: identity, preparation (the masters attached by memory), programming up to
 * the trigger, INTID 81 enabled, the trigger, the completion interrupt, the status read, the interrupt enables
 * cleared, completion acknowledged and the drain. Then the guards, the copy of the active pixels and their hash. */
static int do_decode(void)
{
    struct ss_video_decode_done *o=&vd.done;
    memset(o,0,sizeof(*o));
    o->magic=SS_VIDEO_DECODE_DONE_MAGIC;o->bytes=sizeof(*o);
    os_device_clear_fault(&vd.dev);
    vd.limit=lease_now()+VD_CALL_US;
    if(vd.limit>session_deadline())vd.limit=session_deadline();
    RECORD(vd.info.decode.begin_ticks=ss_counter_ticks();)
    const struct ss_video_decode_jpeg *j=&vd.job;
    if(vd.decoded)return refuse(VD_STEP_DECODE,1);
    const struct large_slot *in=region(j->input_region,j->input_offset,j->input_bytes,0);
    const struct large_slot *out=region(j->output_region,j->output_offset,j->output_bytes,1);
    if(!in||j->input_bytes>UINT32_C(0x1000000))return refuse(VD_STEP_DECODE_INPUT,0);
    if(!out)return refuse(VD_STEP_DECODE_OUTPUT,0);
    const uint8_t *src=(const uint8_t *)(uintptr_t)(in->base+j->input_offset);
    uint8_t *dst=(uint8_t *)(uintptr_t)(out->base+j->output_offset);
    if((uintptr_t)src<(uintptr_t)dst+j->output_bytes&&(uintptr_t)dst<(uintptr_t)src+j->input_bytes)
        return refuse(VD_STEP_DECODE_OUTPUT,1);
    /* The open's one decode: its buffers and device addresses stay reserved for the session. */
    vd.decoded=1;
    struct vd_buffer *b=vd.buffer;
    if(!buffer(&b[0],j->input_bytes))return refuse(VD_STEP_DECODE_MEMORY,0);
    memset(b[0].cpu,0,b[0].mapped);memcpy(b[0].cpu,src,j->input_bytes);
    /* The owner's copy is what the executor parses and the decoder reads. */
    struct a733_video_jpeg pic;
    if(!a733_video_jpeg_parse(b[0].cpu,j->input_bytes,&pic)||!pic.width||!pic.height||pic.width>1920||pic.height>1080)
        return refuse(VD_STEP_DECODE_INPUT,1);
    vd.width=pic.width;vd.height=pic.height;vd.stride=(vd.width+31u)&~31u;vd.rows=(vd.height+31u)&~31u;
    const uint32_t cw=(vd.width+1u)/2u,ch=(vd.height+1u)/2u,active=vd.width*vd.height+2u*cw*ch;
    if(j->output_bytes<active)return refuse(VD_STEP_DECODE_OUTPUT,2);
    if(!buffer(&b[1],vd.stride*vd.rows)||!buffer(&b[2],vd.stride*vd.rows/2u))return refuse(VD_STEP_DECODE_MEMORY,1);
    uint32_t g[3];
    if(!gic_words(g))return refuse(VD_STEP_DECODE_IRQ,0);
    if((g[0]|g[1]|g[2])&VD_INTID_BIT||atomic_load(&vd.irq_count))
        return refuse(VD_STEP_DECODE_IRQ,(g[0]&VD_INTID_BIT?4u:0)|(g[1]&VD_INTID_BIT?1u:0)|(g[2]&VD_INTID_BIT?2u:0)|
            atomic_load(&vd.irq_count)<<8);
    static struct a733_video_jpeg_ops logged_ops;
    logged_ops=*a733_video_platform_jpeg_ops();logged_ops.write=logged_write;
    const struct a733_video_jpeg_job job={b[0].cpu,j->input_bytes,b[0].iova,b[0].mapped,b[1].iova,b[1].mapped,
        b[2].iova,b[2].mapped};
    memset(&vd.run,0,sizeof(vd.run));vd.writes=0;vd.drain_step=0;vd.drain_value=0;
    if(!a733_video_jpeg_begin(&vd.run,&logged_ops,&vd.platform,&job,vd.limit))return refuse(VD_STEP_DECODE,2);
    struct a733_video_jpeg_run *r=&vd.run;
    /* Everything up to the trigger, the last programming write. */
    if(advance_to(A733_VIDEO_JPEG_PROGRAM,658)!=1)return decode_fault(VD_STEP_DECODE_RUN,at(r));
    /* INTID 81 enabled for the trigger only: the handler masks it again on its first interrupt. */
    atomic_store(&vd.irq_seen,0);
    atomic_store_explicit(&vd.armed,1,memory_order_release);
    {
        int e=0;
        for(unsigned i=0;i<100&&!e;++i){e=gicv3_spi_rearm(&vd.gic);if(!e)pause_us(10);}
        if(e!=1){atomic_store(&vd.armed,0);return decode_fault(VD_STEP_DECODE_GIC,(uint32_t)e);}
    }
    RECORD(vd.info.decode.armed_ticks=ss_counter_ticks();)
    const uint64_t trigger=ss_counter_ticks();
    if(a733_video_jpeg_poll(r)<0||r->state!=A733_VIDEO_JPEG_WAIT)return decode_fault(VD_STEP_DECODE_RUN,at(r));
    o->trigger_ticks=trigger;RECORD(vd.info.decode.trigger_ticks=trigger;)
    /* Nothing polls the decoder: the owner blocks until the handler's notification or the bound. */
    const uint64_t until=now_us()+VD_JOB_US;
    while(!atomic_load_explicit(&vd.irq_seen,memory_order_acquire)){
        if(now_us()>=until){
            /* Disarmed first: a handler that already took the interrupt sets irq_seen, which is read once more. */
            if(atomic_exchange(&vd.armed,0)&&!atomic_load(&vd.irq_seen))
                return decode_fault(VD_STEP_DECODE_TIMEOUT,decoder_status());
            while(!atomic_load_explicit(&vd.irq_seen,memory_order_acquire))pause_us(1);
            break;
        }
        (void)sunstone_deadline_wait_until(until);
    }
    o->irq_ticks=vd.irq_ticks;
    /* The first status read after the interrupt shows completion. */
    if(a733_video_jpeg_poll(r)<0||r->state!=A733_VIDEO_JPEG_DISABLE)
        return decode_fault(VD_STEP_DECODE_STATUS,r->status);
    if(advance_to(A733_VIDEO_JPEG_DONE,r->index)!=1)
        return decode_fault(vd.drain_step?vd.drain_step:VD_STEP_DECODE_RUN,vd.drain_step?vd.drain_value:at(r));
    RECORD(vd.info.decode.finish_ticks=ss_counter_ticks();
        if(!gic_words(vd.info.decode.gic))return decode_fault(VD_STEP_DECODE_GIC,0x100);)
    const uint32_t guard=guards();
    RECORD(vd.info.decode.guard=guard;)
    if(guard)return decode_fault(VD_STEP_DECODE_GUARD,guard);
    /* The active pixels: Y rows, then Cb and Cr rows from the chroma buffer (Cb then Cr, stride / 2). */
    const uint32_t w=vd.width,h=vd.height,cb=vd.stride*vd.rows/4u;
    for(uint32_t y=0;y<h;++y)memcpy(dst+(size_t)y*w,b[1].cpu+(size_t)y*vd.stride,w);
    for(uint32_t plane=0;plane<2;++plane)
        for(uint32_t y=0;y<ch;++y)
            memcpy(dst+(size_t)w*h+(size_t)plane*cw*ch+(size_t)y*cw,b[2].cpu+(size_t)plane*cb+(size_t)y*(vd.stride/2u),cw);
    storage_digest_bytes(dst,active,o->sha256);
    o->width=w;o->height=h;o->output_bytes=active;o->status=r->status;o->done_ticks=ss_counter_ticks();
    decode_record();
    RECORD(vd.info.decode.output_bytes=active;vd.info.decode.done_ticks=o->done_ticks;
        memcpy(vd.info.decode.output_sha256,o->sha256,32);)
    return 1;
}
/* The close of an open service, and the one close of a FAULT service
 * (docs/designs/failure-return-audit.md, "A faulted device service closes before it reports"): INTID 81 masked and
 * observed masked, then the binding's vendor order, module gate, bus gate, reset, then PLL_VE0, each write read back.
 * The masters are detached, which returns them to bypass, only while the decoder cannot run: before the binding's
 * close after a decode that drained, after it (reset asserted) when a fault may have left the decoder running. The
 * close writes only CCU, GIC and IOMMU words, so no decoder register is touched whatever state the fault left. A
 * FAULT service's close keeps the first fault in the record and, when every step reads back, ends CLOSED; a step
 * that fails leaves it FAULT. */
static int do_close(void)
{
    const unsigned faulted=os_device_close_begin(&vd.dev);
    vd.limit=lease_now()+VD_CALL_US;
    if(vd.limit>session_deadline())vd.limit=session_deadline();
    RECORD(const uint32_t at_fault=vd.info.failed;)
    {
        atomic_store(&vd.armed,0);
        int r=gicv3_spi_mask(&vd.gic);
        for(unsigned i=0;r==1&&i<200;++i){r=gicv3_spi_masked(&vd.gic);if(r==1)break;if(r==0)pause_us(10);}
        if(r!=1){copy_record();return fail(VD_STEP_CLOSE_GIC,(uint32_t)r&0xffffffu);}
    }
    const unsigned drained=!faulted&&vd.run.state==A733_VIDEO_JPEG_DONE;
    if(vd.attached&&drained){
        if(os_iommu_detach(A733_VIDEO_DEC_MASTERS)!=1){copy_record();return fail(VD_STEP_CLOSE_DETACH,0);}
        vd.attached=0;
    }
    for(;;){
        const int r=a733_video_decoder_close(&vd.platform.decoder,vd.limit,faulted);
        if(r==1)break;
        if(r<0||!timely()){
            copy_record();RECORD(if(faulted)vd.info.failed=at_fault;)
            return fail(VD_STEP_CLOSE,(uint32_t)r);
        }
        if(faulted)pause_us(VD_BUSY_US);
    }
    if(vd.attached){
        if(os_iommu_detach(A733_VIDEO_DEC_MASTERS)!=1){copy_record();RECORD(if(faulted)vd.info.failed=at_fault;)
            return fail(VD_STEP_CLOSE_DETACH,1);}
        vd.attached=0;
    }
    RECORD(vd.info.fault_closed=faulted;
        if(!read_words(vd.info.closed)){copy_record();return fail(VD_STEP_CLOSED_WORDS,0);}
        vd.info.closed_ticks=ss_counter_ticks();)
    copy_record();
    RECORD(if(faulted)vd.info.failed=at_fault;)
    vd.closed=1;
    atomic_store(&vd.dev.phase,OS_DEVICE_CLOSED);
    return faulted?-1:1;
}
/* An open service, or a FAULT one whose close has not started. */
static int closable(void){return os_device_closable(&vd.dev,1);}
static void serve(void)
{
    if(atomic_load_explicit(&vd.request,memory_order_acquire)!=POSTED)return;
    const unsigned phase=atomic_load(&vd.dev.phase);
    int r=-1;
    if(vd.op==OPEN_OP)r=phase==OS_DEVICE_CLOSED&&!vd.closed?do_open():phase==OS_DEVICE_OPEN?1:-1;
    else if(vd.op==CLOSE_OP)r=closable()?do_close():phase==OS_DEVICE_CLOSED?1:-1;
    /* A decode on a service that is not OPEN is refused with nothing recorded (apps/os/device-fault.h). */
    else if(vd.op==DECODE_OP)r=phase==OS_DEVICE_OPEN?do_decode():-1;
    vd.result=r;
    atomic_store_explicit(&vd.request,DONE,memory_order_release);
    os_application_wake_task(vd.waiter);
}
static void os_video_decode_task(void *unused)
{
    (void)unused;
    /* Every deadline wait of this task is on SS_NOTIFY_DEADLINE, which INTID 81 and the row's wake give; index 0 is
     * given only by the application's request and the quiesce, and only the block below takes it, so an input that
     * arrives during a close or a decode stays pending until the block. */
    if(sunstone_deadline_register_index(SS_NOTIFY_DEADLINE)!=1)for(;;)vTaskSuspend(NULL);
    for(;;){
        serve();
        if(atomic_load(&vd.dev.quiesce_wanted)&&atomic_load(&vd.request)!=POSTED){
            if(closable())(void)do_close();
            if(atomic_load(&vd.dev.phase)==OS_DEVICE_CLOSED)atomic_store(&vd.dev.quiesced,1);
        }
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    }
}

/* INTID 81 (VideoEngineInterupt, cedar_ve.c, and ve_irq_work, ve_plat_sun60iw2.c): the vendor handler clears the
 * MPEG interrupt enables (0x114 bits 6:2) and wakes the waiting task. This handler touches no decoder register, which
 * the owner accesses only under the platform register lock: it masks the line at the distributor and notifies the
 * owner, whose executor then reads the status, clears the same enables and acknowledges completion. An interrupt with
 * no decode armed is counted apart. */
void os_video_decode_irq(void)
{
    BaseType_t woken=pdFALSE;
    (void)gicv3_spi_mask(&vd.gic);
    atomic_fetch_add_explicit(&vd.irq_count,1,memory_order_relaxed);
    if(atomic_exchange_explicit(&vd.armed,0,memory_order_acq_rel)){
        vd.irq_ticks=ss_counter_ticks();
        atomic_store_explicit(&vd.irq_seen,1,memory_order_release);
    } else atomic_fetch_add_explicit(&vd.stray,1,memory_order_relaxed);
    if(owner)vTaskNotifyGiveIndexedFromISR(owner,SS_NOTIFY_DEADLINE,&woken);
    portYIELD_FROM_ISR(woken);
}

int os_video_decode_task_privileged(void (*entry)(void *),void *context){return !context&&entry==os_video_decode_task;}
int os_video_decode_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
/* A FAULT service first gets its close: DONE once it is CLOSED, FAULT (site 0x941) when a close step failed or the
 * failure was uncertain (apps/os/device-fault.h). */
static enum os_quiesce_result quiesce_step(struct os_quiesce *q){return os_device_quiesce_step(&vd.dev,q);}
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
static int call(unsigned op,const struct ss_video_decode_jpeg *job)
{
    if(!granted()||atomic_load(&vd.dev.quiesce_wanted))return -1;
    const unsigned state=atomic_load_explicit(&vd.request,memory_order_acquire);
    if(state==DONE){
        if(vd.op!=op)return -1;
        const int r=vd.result;
        atomic_store_explicit(&vd.request,0,memory_order_release);
        return r;
    }
    if(state==POSTED)return vd.op==op?0:-1;
    if(job)vd.job=*job;
    vd.op=op;vd.waiter=xTaskGetCurrentTaskHandle();
    atomic_store_explicit(&vd.request,POSTED,memory_order_release);
    xTaskNotifyGive(owner);
    return 0;
}
int ss_video_decode_open(void){return call(OPEN_OP,NULL);}
int ss_video_decode_close(void){return call(CLOSE_OP,NULL);}
int ss_video_decode_jpeg(const struct ss_video_decode_jpeg *job){return job?call(DECODE_OP,job):-1;}
int ss_video_decode_done(struct ss_video_decode_done *out)
{
    if(!granted()||!out)return -1;
    if(atomic_load(&vd.request)==POSTED||vd.done.magic!=SS_VIDEO_DECODE_DONE_MAGIC)return 0;
    *out=vd.done;return 1;
}
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
    if(step)*step=vd.dev.fault_step;
    if(value)*value=vd.dev.fault_value;
    return vd.dev.fault_step?1:0;
}
static int ready(void)
{const unsigned r=atomic_load(&vd.request);return r==DONE||(r!=POSTED&&atomic_load(&vd.dev.phase)==OS_DEVICE_FAULT);}
void ss_video_decode_wait(uint32_t wait_us)
{
    const uint64_t until=now_us()+(wait_us?wait_us:1);
    if(os_application_wait_unless(until,ready)==SUNSTONE_DEADLINE_ERROR)ss_fail(0x7b1);
}
