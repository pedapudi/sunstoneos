/* The GPU runtime of the integrated OS image's linear-layer option
 * (docs/recognition-dispatch.md, docs/gpu-mimus-service.md). It composes the
 * GPU service from the board's providers: the GPU platform on the shared GPU
 * resource manager, PMIC and locks, the firmware instance, the mimus session
 * as the service's component and the native backend. start runs in the
 * gpu-linear start entry before the scheduler; the recognizer calls open
 * initially, reconciles thermal state between calls, and begins and finishes
 * each job; INTID 95 calls irq. */
#include "os.h"
#include "../board/application.h"
#include "a733_fault.h"
#include "a733_gpu_backend.h"
#include "a733_gpu_mimus.h"
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
extern const struct os_placement os_placement_table[];
extern const struct os_placement_tensor os_placement_tensors[];
extern const uint32_t os_placement_rows, os_placement_gpu_rows;

/* Worker priority, above the recognizer's 3 on the same core. */
#define OS_GPU_PRIORITY 4u
/* OPEN covers power-up, firmware start and the session's publication. The
 * hang bound runs from each kick; board timing per layer shape tightens it. */
#define OS_GPU_OPEN_US UINT64_C(1000000)
#define OS_GPU_MAX_JOB_US UINT64_C(1000000)
#define OS_GPU_REPLY_US (2 * OS_GPU_MAX_JOB_US)
#define OS_GPU_TENSORS 640u
/* The recognizer's finite waits; they record no service progress. */
#define OS_GPU_WAIT_TICKS 10u
_Static_assert(configTICK_RATE_HZ == 1000, "GPU caller wait budgets use millisecond ticks");
/* Offline preparation binds these actual task/deadline constants to the ELF. */
__attribute__((used, retain)) const uint32_t os_gpu_configuration[8] = {
    1, OS_GPU_REPLY_US, OS_GPU_OPEN_US, OS_GPU_WAIT_TICKS, OS_GPU_PRIORITY,
    OS_RECOGNITION_CORE,
#if defined(SUNSTONE_A733_OS_PERF) && OS_PERF_BOOT_MODE == 2
    A733_GPU_LOW_HZ, A733_GPU_LOW_UV};
#else
    A733_GPU_CORE_HZ, A733_GPU_SUPPLY_UV};
#endif
struct gpu_wait {
    uint64_t end, last;
    TickType_t started, ticks;
    unsigned observations;
};

_Static_assert(OS_DISPATCH_ARENA == PVR_MIMUS_ARENA_GPU && OS_DISPATCH_CODE_BYTES == PVR_MIMUS_ARENA_BYTES &&
    OS_DISPATCH_OUTPUT == PVR_MIMUS_OUTPUT_GPU && OS_DISPATCH_OUTPUT_BYTES == PVR_MIMUS_OUTPUT_BYTES &&
    OS_DISPATCH_TAG_BYTES == PVR_MIMUS_TAG_BYTES && OS_DISPATCH_MODEL_BYTES == PVR_MIMUS_MODEL_BYTES,
    "session layout");

struct tskTaskControlBlock *os_recognizer;
static struct {
    struct a733_gpu_platform_native native;
    struct a733_gpu_platform power;
    struct a733_gpu_reserved firmware;
    struct a733_gpu_mimus session;
    struct a733_gpu_backend backend;
    struct pvr_gpu_service service;
    struct pvr_mimus_service *component;
    struct pvr_mimus_tensor tensors[OS_GPU_TENSORS];
    /* N of each admitted tensor, which the hook compares with the call's. */
    uint16_t channels[OS_GPU_TENSORS];
    /* Set once in start; read by the recognizer after the scheduler starts. */
    unsigned started;
    /* Recognizer-owned lifecycle, separate from permanent dispatch admission. */
    unsigned opened, parked, failed, retired;
    uint64_t epoch;
    /* Caller ownership survives timeout, including a late reply already
     * consumed by the service API. Only this recognizer mutates these. */
    uint64_t accepted_ticket;
    unsigned job_pending;
    struct gpu_wait job_wait;
#ifdef SUNSTONE_A733_OS_PERF
    /* Thermal-task private action state, independent of recognizer fields. */
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
/* Every placed row's tensor, once: the session's own bound must cover the
 * table's job_rows, so every job the hook issues passes the worker's check. */
static int admit_tensors(void)
{
    if (!os_placement_rows || os_placement_rows > OS_GPU_TENSORS) return 0;
    for (uint32_t i = 0; i < os_placement_gpu_rows; ++i) {
        const struct os_placement_tensor *t = &os_placement_tensors[i];
        if (t->ordinal >= os_placement_rows || t->region >= PVR_MIMUS_MODELS) return 0;
        const struct os_placement p = os_placement_table[t->ordinal];
        struct pvr_mimus_tensor *admitted = &gpu.tensors[t->ordinal];
        if (!p.n_gpu || admitted->job_rows || p.n_gpu > t->n ||
            !pvr_mimus_tensor_open(admitted, t->region, t->offset, t->stride, t->k, p.n_gpu) ||
            p.job_rows > admitted->job_rows) return 0;
        gpu.channels[t->ordinal] = t->n;
    }
    return 1;
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
int a733_gpu_mimus_start(unsigned core)
{
    if (core >= configNUMBER_OF_CORES || gpu.started || !os_recognizer ||
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) return 0;
    /* Each refusal below leaves OPEN refused and the boot on the CPU; pages
     * acquired before it stay retained. */
    struct a733_pmic_lifetime lifetime;
    static const struct a733_gpu_backend_external external = {.revoked = &board.fatal, .live = board_live};
    const struct a733_gpu_mimus_layout layout = {{OS_DISPATCH_MODELS, OS_DISPATCH_MODELS + OS_DISPATCH_MODEL_BYTES},
        OS_DISPATCH_ARENA, OS_DISPATCH_OUTPUT, gpu.tensors, os_placement_rows, OS_GPU_MAX_JOB_US};
    if (previous_gpu_loss() || !session_owners_distinct() || !admit_tensors() ||
        !a733_gpu_platform_native_init(&gpu.native, &providers.shared.locks.callbacks) ||
        !a733_gpu_platform_init(&gpu.power, &providers.shared.gpu, OS_GPU_CLIENT, &gpu.native.operations, &gpu.native) ||
        a733_pmic_identity(&providers.shared.pmic, &lifetime) != A733_PMIC_READY ||
        !a733_gpu_platform_bind_pmic(&gpu.power, &providers.shared.gpu_native, &providers.shared.pmic, &lifetime,
                                     OS_GPU_FIRMWARE_OWNER) ||
#ifdef SUNSTONE_A733_OS_PERF
        !a733_gpu_platform_performance_enable(&gpu.power,
            OS_PERF_BOOT_MODE==A733_PERF_LOW ? A733_GPU_LOW_HZ : A733_GPU_CORE_HZ) ||
#endif
        !a733_gpu_reserved_prepare(&gpu.firmware, OS_GPU_FIRMWARE_OWNER, gpu_firmware,
                                   (size_t)(gpu_firmware_end - gpu_firmware), 400000000) ||
        !(gpu.component = a733_gpu_mimus_prepare(&gpu.session, &gpu.firmware, &gpu.service, OS_GPU_SESSION_OWNER,
                                                 &layout)) ||
        !a733_gpu_backend_bind(&gpu.backend, &gpu.service, &gpu.firmware.config, &gpu.firmware.memory, &gpu.power,
                               &external) ||
        !service_init(core) ||
        !pvr_gpu_service_bind(&gpu.service, 0, os_recognizer)
#ifdef SUNSTONE_A733_OS_PERF
        || !pvr_gpu_service_bind(&gpu.service,1,os_thermal_task_handle())
#endif
        ) {
        /* A worker that exists but has no client never runs a step. */
        if (gpu.service.worker) vTaskSuspend(gpu.service.worker);
        gpu.component = NULL;
        return 1;
    }
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
static int runtime_record(struct gpu_wait *wait, struct pvr_mimus_record *record)
{
    for (;;) {
        int status = runtime_observe(wait);
        if (status != 1) return status;
        struct pvr_mimus_record observed;
        int received = pvr_mimus_service_receive(gpu.component, &observed, OS_GPU_WAIT_TICKS);
        status = runtime_live(wait);
        if (status != 1) return status;
        if (received) {
            if (observed.result > PVR_MIMUS_LOST) return runtime_fault(OS_GPU_CALL_LOST);
            *record = observed; return 1;
        }
    }
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
int a733_gpu_mimus_open(void)
{
    struct gpu_wait wait;
    if (xTaskGetCurrentTaskHandle() != os_recognizer) return OS_GPU_CALL_LOST;
    if (gpu.failed) return -(int)gpu.failed;
    if (!gpu.started || gpu.retired || gpu.opened) return 0;
    if (gpu.job_pending) return runtime_fault(OS_GPU_CALL_LOST);
    int status = runtime_window(&wait, OS_GPU_OPEN_US);
    if (status != 1) return status;
    struct pvr_gpu_service_reply reply;
    status = runtime_request(PVR_GPU_SERVICE_OPEN, &wait, &reply);
    if (status != 1) return status;
    if (reply.epoch != gpu.epoch + 1) return runtime_fault(OS_GPU_CALL_LOST);
    gpu.epoch = reply.epoch;
    struct pvr_mimus_record record;
    status = runtime_record(&wait, &record);
    if (status != 1) return status;
    if (record.result != PVR_MIMUS_OK || record.sequence) return runtime_fault(OS_GPU_CALL_LOST);
    gpu.opened = 1; gpu.parked = 0;
    return 1;
}
static int runtime_close(void)
{
    struct gpu_wait wait;
    struct pvr_gpu_service_reply reply;
    if (!gpu.opened || gpu.job_pending || runtime_window(&wait, OS_GPU_OPEN_US) != 1 ||
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
int a733_gpu_mimus_thermal(void)
{
    if (!gpu.started || gpu.failed || xTaskGetCurrentTaskHandle() != os_recognizer) return -1;
    if (gpu.retired) return 0;
    if (!board_live(NULL)) { gpu.failed = 1; return -1; }
    int throttled = os_thermal_throttled();
    if ((!throttled && gpu.opened) || (throttled && gpu.parked)) return !throttled;
    os_dispatch_waiting(1);
    int ok = throttled ? runtime_close() : gpu.parked && a733_gpu_mimus_open() == 1;
    /* At most OPEN then CLOSE: a changed thermal level never admits a job
     * during the reopen. CRITICAL is permanent board revocation. */
    if (ok && !throttled && os_thermal_throttled()) {
        throttled = 1;
        ok = runtime_close();
    }
    if (!ok || !board_live(NULL)) { gpu.failed = 1; return -1; }
    os_dispatch_waiting(0);
    return !throttled;
}
int a733_gpu_mimus_retire(void)
{
    if (!gpu.started || gpu.failed || xTaskGetCurrentTaskHandle() != os_recognizer) return 0;
    if (gpu.retired) return gpu.parked && !gpu.opened;
    /* The recognizer has consumed the record but rejected the output. Close
     * admission before shutdown; no result from this job was lent to the
     * engine. Only acknowledged STOPPED permits continued CPU inference. */
    gpu.retired = 1;
    os_dispatch_waiting(1);
    if (!runtime_close() || !board_live(NULL)) { gpu.failed = 1; return 0; }
    os_dispatch_waiting(0);
    return 1;
}
#ifdef SUNSTONE_A733_OS_CPU_RETURN
#include "cpu-return.h"
__attribute__((used,retain)) uint64_t os_gpu_return_report[8];
static atomic_uint return_state;
int os_gpu_return_ready(void)
{return atomic_load_explicit(&return_state,memory_order_acquire)==2;}
int os_gpu_return_started(void)
{return atomic_load_explicit(&return_state,memory_order_acquire)!=0;}
static int return_park(void *context,uint64_t deadline)
{
    if(context!=&gpu.backend || xTaskGetCurrentTaskHandle()!=gpu.service.worker ||
       gpu.service.phase!=PVR_GPU_SERVICE_PARKING ||
       !a733_gpu_backend_stopped(&gpu.backend,&gpu.service))return -1;
    return a733_gpu_platform_park(&gpu.power,deadline);
}
int a733_gpu_mimus_return(uint64_t completed)
{
    if(!completed || xTaskGetCurrentTaskHandle()!=os_recognizer || !gpu.started ||
       gpu.failed || gpu.job_pending || !gpu.epoch || os_gpu_return_started() ||
       a733_smp_counter()>=os_cpu_return_cutoff())return 0;
    atomic_store_explicit(&return_state,1,memory_order_release);
    if(gpu.opened) { if(!runtime_close())return 0; }
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
       atomic_load(&gpu.component->slot_state)!=PVR_MIMUS_SLOT_CLOSED ||
       gpu.service.clients[0].pending || uxQueueMessagesWaiting(gpu.service.requests) ||
       uxQueueMessagesWaiting(gpu.component->completions) || gpu.service.irq_wake_pending ||
       gpu.power.pmic_observed || gpu.power.pmic.request.id ||
       !a733_gpu_backend_stopped(&gpu.backend,&gpu.service) || runtime_live(&wait)!=1)return 0;
    /* The priority-4 worker has blocked on this same core before priority 3
     * can collect the final reply. No other owner can resume it. */
    vTaskSuspend(gpu.service.worker);
    gpu.retired=1;gpu.accepted_ticket=0;
    const uint64_t report[8]={UINT64_C(0x3152475033333741),1,sizeof(report),
        board.session.epoch,board.session.generation,completed,63,A733_GPU_SUPPLY_UV};
    memcpy(os_gpu_return_report,report,sizeof(report));
    atomic_store_explicit(&return_state,2,memory_order_release);
    return 1;
}
#endif
int a733_gpu_mimus_begin(const struct pvr_mimus_request *job)
{
    if (xTaskGetCurrentTaskHandle() != os_recognizer) return OS_GPU_CALL_LOST;
    if (gpu.failed) return -(int)gpu.failed;
    if (gpu.retired) return 0;
    if (!gpu.opened || gpu.job_pending)
        return runtime_fault(OS_GPU_CALL_LOST);
    int status = runtime_window(&gpu.job_wait, OS_GPU_REPLY_US);
    if (status != 1) return status;
    status = pvr_mimus_service_submit(gpu.component, job);
#ifdef SUNSTONE_A733_OS_PERF
    while(status==PVR_MIMUS_SUBMIT_HELD) {
        int live=runtime_observe(&gpu.job_wait);if(live!=1)return live;
        vTaskDelay(1);
        live=runtime_live(&gpu.job_wait);if(live!=1)return live;
        status=pvr_mimus_service_submit(gpu.component,job);
    }
#endif
    if (status == 1) gpu.job_pending = 1;
    if (status < 0) return runtime_fault(OS_GPU_CALL_LOST);
    return status;
}
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
int a733_gpu_mimus_finish(struct pvr_mimus_record *record)
{
    if (xTaskGetCurrentTaskHandle() != os_recognizer) return OS_GPU_CALL_LOST;
    if (gpu.failed) return -(int)gpu.failed;
    if (!gpu.job_pending)
        return runtime_fault(OS_GPU_CALL_LOST);
    int status = runtime_record(&gpu.job_wait, record);
    if (status != 1) return status;
    if (record->result != PVR_MIMUS_LOST) gpu.job_pending = 0;
    return 1;
}
int a733_gpu_mimus_tensor(uint32_t ordinal, struct os_gpu_tensor *out)
{
    if (ordinal >= os_placement_rows || !gpu.tensors[ordinal].job_rows) return 0;
    const struct pvr_mimus_tensor *t = &gpu.tensors[ordinal];
    *out = (struct os_gpu_tensor){t->k, gpu.channels[ordinal], t->stride,
        (const int8_t *)(uintptr_t)(OS_DISPATCH_MODELS + (t->weights - PVR_MIMUS_MODEL_GPU))};
    return 1;
}
void a733_gpu_mimus_irq(void)
{
    BaseType_t woken = pdFALSE;
    /* A mask that cannot hold leaves delivery enabled: end the boot. */
    if (!gpu.started || !pvr_gpu_service_interrupt(&gpu.service, &woken)) board_fail(NULL);
    portYIELD_FROM_ISR(woken);
}

/* The gpu-linear start entry (apps/os/start.c): the service worker on the
 * recognition core, created with EL1 before the scheduler starts. */
int os_gpu_linear_task_privileged(void (*entry)(void *), void *context)
{ return pvr_gpu_service_task_privileged(entry, context) == pdTRUE; }
int os_gpu_linear_pins(struct a733_pin_handoff *pins) { return pins != NULL; }
/* Without the engine's exact-sum hook no call can reach the GPU and the
 * recognizer never opens the session (apps/os/gpu_hook.cc), so nothing is
 * prepared: no pool page, firmware check or worker. */
int os_gpu_linear_start(void)
{
#if __has_include("mimus/asr/exact_sum_hook.h")
    return a733_gpu_mimus_start(OS_RECOGNITION_CORE) == 1;
#else
    return 1;
#endif
}
