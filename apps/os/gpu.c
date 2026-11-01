/* OS GPU device ownership, thermal transitions and returning-image proof.
 * Application programs and data leases enter through gpu_queue.h. */
#include "os.h"
#include "sunstone_crumb.h"


#include "../board/application.h"
#include "a733_fault.h"
#include "a733_gpu_backend.h"
#include "a733_gpu_queue.h"
#include "application.h"
#include "irq_table.h"
#include "a733_smp_placement.h"
#include "gpu-runtime.h"
#include "a733_gpu_platform_native.h"
#include "pvr_gpu_service.h"
#include "boot.h"
#include <string.h>
#ifdef SUNSTONE_A733_OS_PERF
#include "perf.h"
__attribute__((used,retain)) const uint32_t os_gpu_performance_configuration[5]={
    1,A733_GPU_CORE_HZ,A733_GPU_SUPPLY_UV,A733_GPU_LOW_HZ,A733_GPU_LOW_UV};
#endif

extern const unsigned char gpu_firmware[], gpu_firmware_end[];
_Static_assert(configTICK_RATE_HZ == 1000, "GPU caller wait budgets use millisecond ticks");
struct gpu_wait {
    uint64_t end, last;
    TickType_t started, ticks;
    unsigned observations;
};

static struct {
    struct a733_gpu_platform_native native;
    struct a733_gpu_platform power;
    struct a733_gpu_reserved firmware;
    struct a733_gpu_queue session;
    struct a733_gpu_backend backend;
    struct pvr_gpu_service service;
    struct pvr_compute_queue *component;
    /* Set once in start; read by the application after the scheduler starts. */
    unsigned started;
#ifdef SUNSTONE_A733_OS_CPU_RETURN
    /* Written only by a refused pre-scheduler constructor, never by runtime. */
    unsigned constructor_refusal, constructor_worker_suspended;
#endif
    /* Application task lifecycle; job admission belongs to the queue. */
    unsigned opened, parked, failed, retired;
    atomic_uint waiting;
    uint64_t epoch;
    /* Caller ownership survives timeout, including a late reply already
     * consumed by the service API. Only the application task mutates these. */
    uint64_t accepted_ticket;

#ifdef SUNSTONE_A733_OS_PERF
    /* Thermal-task private action state, independent of application fields. */
    struct {uint64_t ticket,end,outer,last;unsigned action;} performance;
#endif
} gpu;

/* A GPU loss recorded by the previous boot's supervisor exit
 * (board_resident_stop_detail): OPEN stays refused for this boot, which
 * clears the slots, so the boot after it opens the GPU again. */
static int previous_gpu_loss(void)
{
    const struct a733_fault_report *r = &a733_fault_previous;
    if ((uint32_t)r->header != A733_FAULT_REPORT_MAGIC) return 0;
    for (unsigned core = 0; core < A733_FAULT_CORES; ++core) {
        const struct a733_fault_record *slot = &r->slot[core];
        if (((r->header >> (32 + core)) & 1) && (slot->info & 0xff) == A733_FAULT_KIND_EXIT &&
            slot->status == 5 && slot->detail == OS_STOP_DETAIL_GPU_LOSS) return 1;
    }
    return 0;
}
static int session_owners_distinct(void)
{
    const struct os_session *os = os_session_admitted();
    if (!os) return 0;
    const struct a733_board_session *s = &board.session;
    const uint64_t owners[] = {s->pmic_owner, s->usb0_owner, s->typec_owner, s->usb2_owner, s->latch_owner,
                               s->latch_m_owner, os->thermal_owner,
#ifdef SUNSTONE_A733_OS_NPU
                               os->npu.owner,
#endif
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
                               os->cpu_clock.owner,
#endif
    };
    for (unsigned i = 0; i < sizeof(owners) / sizeof(*owners); ++i)
        if (owners[i] == OS_GPU_FIRMWARE_OWNER || owners[i] == OS_GPU_SESSION_OWNER) return 0;
#ifdef SUNSTONE_A733_OS_NPU
    if (os->npu.pck_client == OS_GPU_CLIENT) return 0;
#endif
    return s->pck_client != OS_GPU_CLIENT;
}

#ifdef SUNSTONE_A733_OS_CPU_RETURN
static int return_park(void *context,uint64_t deadline);
#endif
static int service_init(unsigned core)
{
#ifdef SUNSTONE_A733_OS_CPU_RETURN
    gpu.firmware.config.park=return_park;
#endif
    return pvr_gpu_service_init(&gpu.service,&gpu.firmware.config,OS_GPU_PRIORITY,(UBaseType_t)1<<core);
}
static int start_refused(unsigned reason)
{
    if (gpu.service.worker) {
        vTaskSuspend(gpu.service.worker);
#ifdef SUNSTONE_A733_OS_CPU_RETURN
        gpu.constructor_worker_suspended=1;
#endif
    }
    gpu.component=NULL;
#ifdef SUNSTONE_A733_OS_CPU_RETURN
    gpu.constructor_refusal=reason;
#else
    (void)reason;
#endif
    return 1;
}
static TaskHandle_t os_gpu_client_task;
static int gpu_start_body(unsigned core);
int os_gpu_queue_start(unsigned core)
{
    sunstone_crumb(SUNSTONE_CRUMB_GPU_CONSTRUCT, (uint32_t)(core));
    int r = gpu_start_body(core);
    sunstone_crumb(SUNSTONE_CRUMB_GPU_CONSTRUCTED, (uint32_t)(gpu.started));
    return r;
}
static int gpu_start_body(unsigned core)
{
    if (core >= configNUMBER_OF_CORES || gpu.started || !os_gpu_client_task ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
#ifdef SUNSTONE_A733_OS_CPU_RETURN
    if(gpu.constructor_refusal)return 0;
#endif
    /* Each refusal below leaves OPEN refused and the boot on the CPU; pages
     * acquired before it stay retained. */
    struct a733_pmic_lifetime lifetime;
    static const struct a733_gpu_backend_external external = {.revoked = &board.fatal, .live = board_live};
    if(previous_gpu_loss())return start_refused(1);
    if(!session_owners_distinct())return start_refused(2);
    if(!a733_gpu_platform_native_init(&gpu.native,&providers.shared.locks.callbacks))return start_refused(4);
    if(!a733_gpu_platform_init(&gpu.power,&providers.shared.gpu,OS_GPU_CLIENT,&gpu.native.operations,&gpu.native))return start_refused(5);
    if(a733_pmic_identity(&providers.shared.pmic,&lifetime)!=A733_PMIC_READY)return start_refused(6);
    if(!a733_gpu_platform_bind_pmic(&gpu.power,&providers.shared.gpu_native,&providers.shared.pmic,&lifetime,
                                  OS_GPU_FIRMWARE_OWNER))return start_refused(7);
#ifdef SUNSTONE_A733_OS_PERF
    if(!a733_gpu_platform_performance_enable(&gpu.power,
        OS_PERF_BOOT_MODE==A733_PERF_LOW ? A733_GPU_LOW_HZ : A733_GPU_CORE_HZ))return start_refused(8);
#endif
    if(!a733_gpu_reserved_prepare(&gpu.firmware,OS_GPU_FIRMWARE_OWNER,gpu_firmware,
        (size_t)(gpu_firmware_end-gpu_firmware),400000000))return start_refused(9);
    if(!(gpu.component=a733_gpu_queue_prepare(&gpu.session,&gpu.firmware,&gpu.service,OS_GPU_SESSION_OWNER,
                                            ss_application.manifest->regions,ss_application.manifest->region_count)))return start_refused(10);
    if(!a733_gpu_backend_bind(&gpu.backend,&gpu.service,&gpu.firmware.config,&gpu.firmware.memory,&gpu.power,
                             &external))return start_refused(11);
    if(!service_init(core))return start_refused(12);
    if(!pvr_gpu_service_bind(&gpu.service,0,os_gpu_client_task))return start_refused(13);
#ifdef SUNSTONE_A733_OS_PERF
    if(!pvr_gpu_service_bind(&gpu.service,1,os_thermal_task_handle()))return start_refused(14);
#endif
    if(!pvr_compute_queue_client_bind(gpu.component))return start_refused(3);
    gpu.started = 1;
    return 1;
}

static int runtime_fault(int result)
{ gpu.failed = (unsigned)-result; return result; }
static int runtime_window(struct gpu_wait *wait, uint64_t budget)
{
    if (!board_live(NULL)) return runtime_fault(OS_GPU_CALL_LOST);
    uint64_t limit = atomic_load_explicit(&board.live_until, memory_order_acquire);
    if (board.session.deadline_us < limit) limit = board.session.deadline_us;
    const uint64_t now = a733_smp_counter() / 24;
    if (limit <= 10000 || now >= limit - 10000) return runtime_fault(OS_GPU_CALL_TIMEOUT);
    limit -= 10000;
    /* Subtract before adding, so even an invalid near-UINT64_MAX lifetime
     * cannot wrap this deadline. The RTOS budget is independent of the counter. */
    uint64_t span = limit - now < budget ? limit - now : budget;
    *wait = (struct gpu_wait){.end = now + span, .last = now,
        .started = xTaskGetTickCount(), .ticks = (TickType_t)((span + 999) / 1000),
        .observations = (unsigned)(2 * (span / 1000 + 1) + 2)};
    return 1;
}
static int runtime_live(struct gpu_wait *wait)
{
    if (!board_live(NULL)) return runtime_fault(OS_GPU_CALL_LOST);
    const uint64_t now = a733_smp_counter() / 24;
    if (now < wait->last) return runtime_fault(OS_GPU_CALL_LOST);
    if (now >= wait->end || (TickType_t)(xTaskGetTickCount() - wait->started) >= wait->ticks)
        return runtime_fault(OS_GPU_CALL_TIMEOUT);
    wait->last = now;
    return 1;
}
static int runtime_observe(struct gpu_wait *wait)
{
    int live = runtime_live(wait);
    if (live != 1) return live;
    /* Also finite if both clocks stop while mocked/broken queues return. */
    if (!wait->observations) return runtime_fault(OS_GPU_CALL_TIMEOUT);
    --wait->observations;
    return 1;
}
static int runtime_request(enum pvr_gpu_service_operation operation, struct gpu_wait *wait,
                           struct pvr_gpu_service_reply *reply)
{
#ifdef SUNSTONE_A733_OS_PERF
retry:
#endif
    ;
    int status = runtime_observe(wait);
    if (status != 1) return status;
    const struct pvr_gpu_service_request request = {operation,
        operation == PVR_GPU_SERVICE_STATUS ? 0 : wait->end - wait->last};
    uint64_t ticket;
    int sent = pvr_gpu_service_send_epoch(&gpu.service, gpu.epoch, &request, OS_GPU_WAIT_TICKS, &ticket);
    if (sent) gpu.accepted_ticket = ticket;
    status = runtime_live(wait);
    if (status != 1) return status;
    if (!sent) return 0;
    for (;;) {
        status = runtime_observe(wait);
        if (status != 1) return status;
        int received = pvr_gpu_service_collect(&gpu.service, ticket, reply, OS_GPU_WAIT_TICKS);
        status = runtime_live(wait);
        if (status != 1) return status;
        if (received) {
#ifdef SUNSTONE_A733_OS_PERF
            if(operation==PVR_GPU_SERVICE_CLOSE && reply->result==PVR_GPU_SERVICE_BUSY) {
                vTaskDelay(1);goto retry;
            }
#endif
            return reply->result == PVR_GPU_SERVICE_OK ? 1 : runtime_fault(OS_GPU_CALL_LOST);
        }
    }
}
/* Context creation owns the first OPEN; the runtime owns healthy reopens. */
void pvr_compute_queue_runtime_opened(uint64_t epoch)
{ gpu.epoch=epoch;gpu.opened=1;gpu.parked=0; }
void pvr_compute_queue_runtime_closed(void)
{ gpu.opened=0;gpu.parked=1;gpu.retired=1; }
static int runtime_reopen(void)
{
    struct gpu_wait wait;struct pvr_gpu_service_reply reply;
    if(gpu.failed||gpu.retired||!gpu.parked||runtime_window(&wait,OS_GPU_OPEN_US)!=1)return 0;
    if(runtime_request(PVR_GPU_SERVICE_OPEN,&wait,&reply)!=1)return 0;
    uint64_t epoch=reply.epoch;
    while(reply.phase!=PVR_GPU_SERVICE_READY){
        vTaskDelay(1);
        if(runtime_request(PVR_GPU_SERVICE_STATUS,&wait,&reply)!=1)return 0;
    }
    if(epoch!=gpu.epoch+1||reply.epoch!=epoch)return 0;
    pvr_compute_queue_runtime_opened(epoch);return 1;
}
static int runtime_close(void)
{
    struct gpu_wait wait;
    struct pvr_gpu_service_reply reply;
    if (!gpu.opened || gpu.component->owed || runtime_window(&wait, OS_GPU_OPEN_US) != 1 ||
        runtime_request(PVR_GPU_SERVICE_CLOSE, &wait, &reply) != 1) return 0;
    while (reply.phase != PVR_GPU_SERVICE_STOPPED) {
        /* The worker owns firmware, drain and platform waits. Observation
         * renews neither service progress nor this absolute bound. */
        vTaskDelay(1);
        if (runtime_request(PVR_GPU_SERVICE_STATUS, &wait, &reply) != 1) return 0;
    }
    if (reply.epoch != gpu.epoch) return 0;
    gpu.opened = 0; gpu.parked = 1;
    return 1;
}
static int gpu_thermal_body(void);
int os_gpu_queue_thermal(void)
{
    sunstone_crumb(SUNSTONE_CRUMB_GPU_THERMAL, (uint32_t)(0xffff));
    int r = gpu_thermal_body();
    sunstone_crumb(SUNSTONE_CRUMB_GPU_THERMAL, (uint32_t)(r));
    return r;
}
static int gpu_thermal_body(void)
{
    if (!gpu.started || gpu.failed || xTaskGetCurrentTaskHandle() != os_gpu_client_task) return -1;
    if (gpu.retired) return 0;
    if (!board_live(NULL)) { gpu.failed = 1; return -1; }
    int throttled = os_thermal_throttled();
    if ((!throttled && gpu.opened) || (throttled && gpu.parked)) return !throttled;
    atomic_store(&gpu.waiting,1);
    int ok = throttled ? runtime_close() : gpu.parked && runtime_reopen() == 1;
    /* At most OPEN then CLOSE: a changed thermal level never admits a job
     * during the reopen. CRITICAL is permanent board revocation. */
    if (ok && !throttled && os_thermal_throttled()) {
        throttled = 1;
        ok = runtime_close();
    }
    if (!ok || !board_live(NULL)) { gpu.failed = 1; return -1; }
    atomic_store(&gpu.waiting,0);
    return !throttled;
}
static int gpu_retire_body(void);
int os_gpu_queue_retire(void)
{
    sunstone_crumb(SUNSTONE_CRUMB_GPU_RETIRE, (uint32_t)(0));
    int r = gpu_retire_body();
    sunstone_crumb(SUNSTONE_CRUMB_GPU_RETIRED, (uint32_t)(r));
    return r;
}
static int gpu_retire_body(void)
{
    if (!gpu.started || gpu.failed || xTaskGetCurrentTaskHandle() != os_gpu_client_task) return 0;
    if (gpu.retired) return gpu.parked && !gpu.opened;
    /* The application has consumed the record but rejected the output. Close
     * admission before shutdown; no result from this job was lent to the
     * engine. Only acknowledged STOPPED permits continued CPU inference. */
    gpu.retired = 1;
    atomic_store(&gpu.waiting,1);
    if (!runtime_close() || !board_live(NULL)) { gpu.failed = 1; return 0; }
    atomic_store(&gpu.waiting,0);
    return 1;
}
#ifdef SUNSTONE_A733_OS_CPU_RETURN
#include "cpu-return.h"
__attribute__((used,retain)) const uint32_t os_gpu_untouched_return_version=2;
__attribute__((used,retain)) uint64_t os_gpu_return_report[8];
static atomic_uint return_state;
int os_gpu_return_ready(void)
{unsigned state=atomic_load_explicit(&return_state,memory_order_acquire);return state==2||state==3;}
int os_gpu_return_started(void)
{return atomic_load_explicit(&return_state,memory_order_acquire)!=0;}
static int zero_bytes(const void *address,size_t bytes)
{
    const unsigned char *p=address;
    for(size_t i=0;i<bytes;++i)if(p[i])return 0;
    return 1;
}
/* An unconstructed or cold service has accepted no OPEN. Allocated RAM stays
 * owned until the host restores its backup. */
static int gpu_return_untouched_body(void);
int os_gpu_queue_return_untouched(void)
{
    sunstone_crumb(SUNSTONE_CRUMB_GPU_PARKED, (uint32_t)(0xfffe));
    int r = gpu_return_untouched_body();
    sunstone_crumb(SUNSTONE_CRUMB_GPU_PARKED, (uint32_t)(r));
    return r;
}
static int gpu_return_untouched_body(void)
{
    const struct a733_gpu_platform *p=&gpu.power;
    const struct a733_gpu_backend *b=&gpu.backend;
    const struct pvr_gpu_service *s=&gpu.service;
    const struct pvr_compute_queue *m=&gpu.session.component;
    const int unconstructed=zero_bytes(&gpu,sizeof(gpu));
    const int cold=!unconstructed&&!gpu.constructor_refusal;
    const TaskHandle_t client=unconstructed?(TaskHandle_t)ss_application.task:os_gpu_client_task;
    if(!client || xTaskGetCurrentTaskHandle()!=client ||
       (unconstructed&&(os_gpu_client_task||!ss_grant())) || gpu.constructor_refusal>14 ||
       (cold ? (!gpu.started || gpu.component!=m || !p->initialized || !b->initialized ||
                !s->initialized || !s->worker) : (gpu.started || gpu.component)) ||
       gpu.opened || gpu.parked || gpu.failed || gpu.retired || gpu.epoch || gpu.accepted_ticket ||
       os_gpu_return_started() || a733_smp_counter()>=os_cpu_return_cutoff())return 0;
    if((!p->initialized&&!zero_bytes(p,sizeof(*p))) ||
       p->phase!=A733_GPU_PLATFORM_NEW || p->token || p->delegated || p->deadline || p->last_now ||
       p->power.phase!=A733_GPU_POWER_NEW || p->power.error || p->power.deadline || p->power.last_now ||
       p->power.lease_attempted || p->power.lease_held || p->power.requested ||
       p->power.row.counted || atomic_load(&p->power.row.request) || atomic_load(&p->power.row.state) ||
       p->pmic_observed || p->supply_token.id || p->supply_request.id || p->supply_stage ||
       p->supply_written || p->supply_settled || p->pmic.rail.id || p->pmic.request.id ||
       p->pmic.stage || p->pmic.failed || p->pmic.armed || p->pmic.deadline || p->pmic.last_now ||
       p->pmic.armed_deadline || (!cold&&p->perf.enabled) ||
       p->perf.parent.id || p->perf.deadline || p->perf.settled || p->perf.last_now ||
       p->perf.current_hz || p->perf.current_uv || p->perf.value || p->perf.action ||
       p->perf.stage || p->perf.startup || p->perf.off_normalized)return 0;
    if((!b->initialized&&!zero_bytes(b,sizeof(*b))) ||
       (b->initialized&&(b->phase!=A733_GPU_BACKEND_BOUND || b->service!=s || b->power!=p ||
                         b->memory!=&gpu.firmware.memory)) ||
       atomic_load(&b->error) || b->deadline || (!cold&&b->last_now) || b->identity || b->identity_step ||
       !zero_bytes(&b->mode,sizeof(b->mode)))return 0;
    const unsigned creating=atomic_load(&m->create_pending), created=atomic_load(&m->create_done);
    const int refused_create=cold && created==2 && (creating==1 || creating==2);
    if(s->phase!=PVR_GPU_SERVICE_COLD || s->error || s->epoch || s->deadline ||
       (cold ? (b->last_now!=s->last_now || s->owner!=PVR_GPU_SERVICE_CLIENTS || !s->irq_masked ||
                s->clients[0].task!=os_gpu_client_task || !m->completions || m->epoch || m->error) : s->last_now) ||
       atomic_load(&s->published_epoch) || s->irq_fault || s->irq_wake_pending || s->irq_deferred ||
       m->sequence || m->owed || m->crumb_pending || m->generation || m->create_ticket ||
       (!refused_create&&(creating||created)) || m->close_ticket || atomic_load(&m->client_owed) || atomic_load(&m->lost))return 0;
    if(m->owner) {
        if(m->owner!=&gpu.session || m->service!=s ||
           (m->phase!=PVR_COMPUTE_QUEUE_BOUND &&
            (m->phase!=PVR_COMPUTE_QUEUE_FAILED || (cold&&!refused_create))) ||
           atomic_load(&m->slot_state)!=PVR_COMPUTE_QUEUE_CLOSED)return 0;
    } else if(!zero_bytes(m,sizeof(*m)))return 0;
    if(s->worker) {
        if(!s->initialized)return 0;
        const eTaskState worker=eTaskGetState(s->worker);
        if((cold ? (gpu.constructor_worker_suspended ||
            (worker!=eBlocked&&worker!=eReady)) : (!gpu.constructor_worker_suspended ||
            worker!=eSuspended)) ||
           !s->requests || uxQueueMessagesWaiting(s->requests))return 0;
        for(unsigned i=0;i<PVR_GPU_SERVICE_CLIENTS;++i)
            if(s->clients[i].pending || (!cold&&s->clients[i].next_ticket) || !s->clients[i].replies ||
               uxQueueMessagesWaiting(s->clients[i].replies))return 0;
    } else if(s->initialized || gpu.constructor_worker_suspended || !zero_bytes(s,sizeof(*s)))return 0;
    if(m->completions&&uxQueueMessagesWaiting(m->completions))return 0;
    /* The worker shares the application's core. With no accepted request it
     * is blocked or ready, and cannot advance while this caller seals it. */
    if(cold)vTaskSuspend(s->worker);
    uint64_t version=2,value=gpu.constructor_refusal;
#if defined(SUNSTONE_A733_OS_GPU_BISECT) && OS_GPU_BISECT_STAGE == 1
    if(cold){version=OS_GPU_RETURN_VERSION;value=OS_GPU_RETURN_VALUE;}
#endif
    const uint64_t report[8]={UINT64_C(0x3152475033333741),version,sizeof(report),
        board.session.epoch,board.session.generation,0,0,value};
    memcpy(os_gpu_return_report,report,sizeof(report));gpu.retired=1;
    atomic_store_explicit(&return_state,3,memory_order_release);
    return 1;
}
static int return_park(void *context,uint64_t deadline)
{
    if(context!=&gpu.backend || xTaskGetCurrentTaskHandle()!=gpu.service.worker ||
       gpu.service.phase!=PVR_GPU_SERVICE_PARKING ||
       !a733_gpu_backend_stopped(&gpu.backend,&gpu.service))return -1;
    return a733_gpu_platform_park(&gpu.power,deadline);
}
static int gpu_return_body(uint64_t completed);
int os_gpu_queue_return(uint64_t completed)
{
    sunstone_crumb(SUNSTONE_CRUMB_GPU_PARKED, (uint32_t)(0xffff));
    int r = gpu_return_body(completed);
    sunstone_crumb(SUNSTONE_CRUMB_GPU_PARKED, (uint32_t)(r));
    return r;
}
static int gpu_return_body(uint64_t completed)
{
    if(xTaskGetCurrentTaskHandle()!=os_gpu_client_task || !gpu.started || !gpu.component ||
       gpu.failed || gpu.component->owed || atomic_load(&gpu.component->client_owed) || !gpu.epoch || os_gpu_return_started() ||
       a733_smp_counter()>=os_cpu_return_cutoff())return 0;
    atomic_store_explicit(&return_state,1,memory_order_release);
    if(gpu.opened) { if(!os_gpu_queue_retire())return 0; }
    if(!gpu.parked || gpu.opened || !a733_gpu_backend_stopped(&gpu.backend,&gpu.service))return 0;
    struct gpu_wait wait;
    struct pvr_gpu_service_reply reply;
    if(runtime_window(&wait,OS_GPU_OPEN_US)!=1 ||
       runtime_request(PVR_GPU_SERVICE_PARK,&wait,&reply)!=1)return 0;
    while(reply.phase!=PVR_GPU_SERVICE_PARKED) {
        vTaskDelay(1);
        if(runtime_request(PVR_GPU_SERVICE_STATUS,&wait,&reply)!=1)return 0;
    }
    if(reply.epoch!=gpu.epoch || gpu.component->sequence!=completed || gpu.component->owed ||
       atomic_load(&gpu.component->slot_state)!=PVR_COMPUTE_QUEUE_CLOSED ||
       gpu.service.clients[0].pending || uxQueueMessagesWaiting(gpu.service.requests) ||
       uxQueueMessagesWaiting(gpu.component->completions) || gpu.service.irq_wake_pending ||
       gpu.power.pmic_observed || gpu.power.pmic.request.id ||
       !a733_gpu_backend_stopped(&gpu.backend,&gpu.service) || runtime_live(&wait)!=1)return 0;
    /* The priority-4 worker has blocked on this same core before priority 3
     * can collect the final reply. No other owner can resume it. */
    vTaskSuspend(gpu.service.worker);
    gpu.retired=1;gpu.accepted_ticket=0;
    const uint64_t report[8]={UINT64_C(0x3152475033333741),OS_GPU_RETURN_VERSION,sizeof(report),
        board.session.epoch,board.session.generation,completed,63,OS_GPU_RETURN_VALUE};
    memcpy(os_gpu_return_report,report,sizeof(report));
    atomic_store_explicit(&return_state,2,memory_order_release);
    return 1;
}
#endif
#ifdef SUNSTONE_A733_OS_PERF
int os_gpu_perf_owner(void)
{return gpu.started && (pvr_gpu_service_performance_state(&gpu.service)&PVR_GPU_PERF_OWNER)!=0;}
int os_gpu_perf_ready(void)
{return gpu.started && pvr_gpu_service_performance_state(&gpu.service)==(PVR_GPU_PERF_OWNER|PVR_GPU_PERF_READY);}
enum a733_perf_status os_gpu_perf_action(enum a733_perf_action action,uint32_t value,
    uint64_t deadline,int start,struct a733_perf_point *point)
{
    if(xTaskGetCurrentTaskHandle()!=os_thermal_task_handle())return A733_PERF_FAILED;
    if(!gpu.started)return A733_PERF_REFUSED;
    if(!board_live(NULL) || (pvr_gpu_service_performance_state(&gpu.service)&PVR_GPU_PERF_FAILED))return A733_PERF_FAILED;
    uint64_t now=a733_smp_counter()/24;
    if(now<gpu.performance.last)return A733_PERF_FAILED;
    gpu.performance.last=now;
    if(start) {
        if(gpu.performance.ticket)return A733_PERF_FAILED;
        uint64_t end=atomic_load_explicit(&board.live_until,memory_order_acquire);
        if(board.session.deadline_us<end)end=board.session.deadline_us;
        if(end<=10000 || now>=end-10000)return A733_PERF_EXPIRED;
        end-=10000;
        gpu.performance.end=deadline<end ? deadline : end;gpu.performance.action=action;
        if(action==A733_PERF_HOLD) {
            if(gpu.performance.end>UINT64_MAX-3*A733_PERF_ACCEL_US)return A733_PERF_REFUSED;
            gpu.performance.outer=gpu.performance.end+3*A733_PERF_ACCEL_US;
            if(end<gpu.performance.outer)gpu.performance.outer=end;
        } else if(action!=A733_PERF_READ && gpu.performance.outer && gpu.performance.outer<gpu.performance.end)
            gpu.performance.end=gpu.performance.outer;
    }
    if(gpu.performance.action!=(unsigned)action)return A733_PERF_FAILED;
    if(now>=gpu.performance.end)return gpu.performance.ticket ? A733_PERF_FAILED : A733_PERF_EXPIRED;
    if(!gpu.performance.ticket) {
        const uint64_t limit=action==A733_PERF_READ || !gpu.performance.outer ? gpu.performance.end : gpu.performance.outer;
        if(!pvr_gpu_service_performance(&gpu.service,action,value,gpu.performance.end,limit,&gpu.performance.ticket))
            return A733_PERF_PENDING;
    }
    struct pvr_gpu_service_reply reply;
    if(!pvr_gpu_service_collect(&gpu.service,gpu.performance.ticket,&reply,0))return A733_PERF_PENDING;
    /* A received late reply no longer occupies the queue, but the accepted
     * ticket remains retained here. It cannot authorize another action. */
    now=a733_smp_counter()/24;
    if(now<gpu.performance.last || now>=gpu.performance.end || !board_live(NULL))return A733_PERF_FAILED;
    gpu.performance.last=now;
    if(reply.result==PVR_GPU_SERVICE_FAULT || reply.performance_status>A733_PERF_FAILED ||
        (reply.result==PVR_GPU_SERVICE_OK && reply.performance_status==A733_PERF_PENDING))return A733_PERF_FAILED;
    enum a733_perf_status result=reply.result==PVR_GPU_SERVICE_OK ?
        (enum a733_perf_status)reply.performance_status : A733_PERF_REFUSED;
    if(result==A733_PERF_FAILED)return result;
    gpu.performance.ticket=0;
    if(result==A733_PERF_DONE && point)*point=reply.performance_point;
    if(action==A733_PERF_RELEASE && result==A733_PERF_DONE)gpu.performance.outer=0;
    return result;
}
#endif
void os_gpu_queue_irq(void)
{
    BaseType_t woken = pdFALSE;
    /* A mask that cannot hold leaves delivery enabled: end the boot. */
    if (!gpu.started || !pvr_gpu_service_interrupt(&gpu.service, &woken)) board_fail(NULL);
    portYIELD_FROM_ISR(woken);
}

int os_gpu_queue_task_privileged(void (*entry)(void *),void *context)
{ return pvr_gpu_service_task_privileged(entry, context) == pdTRUE; }
int os_gpu_queue_pins(struct a733_pin_handoff *pins) { return pins != NULL; }
int os_gpu_queue_start_entry(void)
{
    const struct ss_grant *grant=ss_grant();
    if(!grant||!(grant->devices&SS_DEVICE_GPU))return 1;
    if(!ss_application.manifest||!ss_application.task)return 0;
    os_gpu_client_task=(TaskHandle_t)ss_application.task;
    for(unsigned i=0;i<a733_irq_route_count;++i)
        if(a733_irq_routes[i].intid==A733_GPU_INTID)
            return a733_irq_routes[i].core==grant->main_core&&os_gpu_queue_start(grant->main_core)==1;
    return 0;
}
int pvr_compute_queue_runtime_sync(void)
{
    if(!gpu.started)return 0;
    if(!gpu.opened&&!gpu.parked)return gpu.failed?-1:1;
    return os_gpu_queue_thermal();
}
#ifdef SUNSTONE_A733_OS_CPU_RETURN
int os_gpu_retire(void)
{
    if(os_gpu_return_ready())return 1;
    if(gpu.constructor_refusal || (!gpu.epoch && gpu.service.phase==PVR_GPU_SERVICE_COLD))
        return os_gpu_queue_return_untouched();
    return gpu.component&&os_gpu_queue_return(gpu.component->sequence);
}
#endif

unsigned os_gpu_queue_state(void)
{
    if(gpu.failed||(gpu.component&&atomic_load(&gpu.component->lost)))return 2;
    return atomic_load(&gpu.waiting)||(gpu.component&&
        (atomic_load(&gpu.component->client_owed)||
         (atomic_load(&gpu.component->create_pending)&&!atomic_load(&gpu.component->create_done))))?1:0;
}

#if defined(SUNSTONE_A733_OS_CPU_RETURN) && defined(SUNSTONE_A733_OS_GPU_BISECT)
__attribute__((used,retain)) const uint32_t os_gpu_bisect_configuration[3]={
    1,OS_GPU_BISECT_STAGE,OS_GPU_BISECT_LIMIT};
#endif
