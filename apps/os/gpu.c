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

extern const unsigned char gpu_firmware[], gpu_firmware_end[];
extern const struct os_placement os_placement_table[];
extern const struct os_placement_tensor os_placement_tensors[];
extern const uint32_t os_placement_rows, os_placement_gpu_rows;

/* Fixed identities: the GPU resource manager's client, the PMIC consumer and
 * pool owner of the firmware, and the pool owner and context owner of the
 * session. */
#define OS_GPU_CLIENT 7u
#define OS_GPU_FIRMWARE_OWNER UINT64_C(0x4f53475055464d31)
#define OS_GPU_SESSION_OWNER UINT32_C(0x4f534731)
/* Worker priority, above the recognizer's 3 on the same core. */
#define OS_GPU_PRIORITY 4u
/* OPEN covers power-up, firmware start and the session's publication. The
 * hang bound runs from each kick; board timing per layer shape tightens it. */
#define OS_GPU_OPEN_US UINT64_C(1000000)
#define OS_GPU_MAX_JOB_US UINT64_C(1000000)
#define OS_GPU_TENSORS 640u
/* The recognizer's finite waits; they record no service progress. */
#define OS_GPU_WAIT_TICKS 10u

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
    const struct a733_board_session *s = &board.session;
    const uint64_t owners[] = {s->pmic_owner, s->usb0_owner, s->typec_owner, s->usb2_owner, s->latch_owner,
                               s->latch_m_owner};
    for (unsigned i = 0; i < sizeof(owners) / sizeof(*owners); ++i)
        if (owners[i] == OS_GPU_FIRMWARE_OWNER || owners[i] == OS_GPU_SESSION_OWNER) return 0;
    return s->pck_client != OS_GPU_CLIENT;
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
        !a733_gpu_reserved_prepare(&gpu.firmware, OS_GPU_FIRMWARE_OWNER, gpu_firmware,
                                   (size_t)(gpu_firmware_end - gpu_firmware), 400000000) ||
        !(gpu.component = a733_gpu_mimus_prepare(&gpu.session, &gpu.firmware, &gpu.service, OS_GPU_SESSION_OWNER,
                                                 &layout)) ||
        !a733_gpu_backend_bind(&gpu.backend, &gpu.service, &gpu.firmware.config, &gpu.firmware.memory, &gpu.power,
                               &external) ||
        !pvr_gpu_service_init(&gpu.service, &gpu.firmware.config, OS_GPU_PRIORITY, (UBaseType_t)1 << core) ||
        !pvr_gpu_service_bind(&gpu.service, 0, os_recognizer)) {
        /* A worker that exists but has no client never runs a step. */
        if (gpu.service.worker) vTaskSuspend(gpu.service.worker);
        gpu.component = NULL;
        return 1;
    }
    gpu.started = 1;
    return 1;
}

static int runtime_window(uint64_t *end)
{
    if (!board_live(NULL)) return 0;
    uint64_t limit = atomic_load_explicit(&board.live_until, memory_order_acquire);
    if (board.session.deadline_us < limit) limit = board.session.deadline_us;
    const uint64_t now = a733_smp_counter() / 24;
    if (limit <= 10000 || now >= limit - 10000) return 0;
    limit -= 10000;
    *end = limit - now > OS_GPU_OPEN_US ? now + OS_GPU_OPEN_US : limit;
    return 1;
}
static int runtime_live(uint64_t end)
{ return board_live(NULL) && a733_smp_counter() / 24 < end; }
static int runtime_request(enum pvr_gpu_service_operation operation, uint64_t end,
                           struct pvr_gpu_service_reply *reply)
{
    if (!runtime_live(end)) return 0;
    const uint64_t now = a733_smp_counter() / 24;
    if (now >= end) return 0;
    const struct pvr_gpu_service_request request = {operation,
        operation == PVR_GPU_SERVICE_STATUS ? 0 : end - now};
    uint64_t ticket;
    if (!pvr_gpu_service_send_epoch(&gpu.service, gpu.epoch, &request, OS_GPU_WAIT_TICKS, &ticket)) return 0;
    while (!pvr_gpu_service_collect(&gpu.service, ticket, reply, OS_GPU_WAIT_TICKS))
        if (!runtime_live(end)) return 0;
    return runtime_live(end) && reply->result == PVR_GPU_SERVICE_OK;
}
int a733_gpu_mimus_open(void)
{
    uint64_t end;
    if (!gpu.started || gpu.failed || gpu.retired || gpu.opened || !runtime_window(&end)) return 0;
    struct pvr_gpu_service_reply reply;
    if (!runtime_request(PVR_GPU_SERVICE_OPEN, end, &reply) || reply.epoch != gpu.epoch + 1) return 0;
    gpu.epoch = reply.epoch;
    struct pvr_mimus_record record;
    while (!pvr_mimus_service_receive(gpu.component, &record, OS_GPU_WAIT_TICKS))
        if (!runtime_live(end)) return 0;
    if (!runtime_live(end) || record.result != PVR_MIMUS_OK || record.sequence) return 0;
    gpu.opened = 1; gpu.parked = 0;
    return 1;
}
static int runtime_close(void)
{
    uint64_t end;
    struct pvr_gpu_service_reply reply;
    if (!gpu.opened || !runtime_window(&end) || !runtime_request(PVR_GPU_SERVICE_CLOSE, end, &reply)) return 0;
    while (reply.phase != PVR_GPU_SERVICE_STOPPED) {
        /* The worker owns firmware, drain and platform waits. Observation
         * renews neither service progress nor this absolute bound. */
        vTaskDelay(1);
        if (!runtime_request(PVR_GPU_SERVICE_STATUS, end, &reply)) return 0;
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
    int ok = throttled ? runtime_close() : gpu.parked && a733_gpu_mimus_open();
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
int a733_gpu_mimus_begin(const struct pvr_mimus_request *job)
{ return gpu.failed ? -1 : gpu.retired ? 0 : pvr_mimus_service_submit(gpu.component, job); }
void a733_gpu_mimus_finish(struct pvr_mimus_record *record)
{ while (!pvr_mimus_service_receive(gpu.component, record, OS_GPU_WAIT_TICKS)) {} }
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
