/* SPDX-License-Identifier: MIT
 * The wrapper-write variant of the GPU power diagnostic. It compiles main.c
 * as the power-only variant with its client and entry renamed, so main.c and
 * the power-only image stay unchanged, and adds its own client and entry.
 * The build links power-abandon.c and platform-abandon.c in place of the
 * shared controller and platform sources (abandon.h).
 */
#define GPU_POWER_ONLY 1
#define kernel_main gpu_power_only_kernel_main
#define gpu_power_id_client gpu_power_only_client
#include "main.c"
#undef kernel_main
#undef gpu_power_id_client
#include "abandon.h"
#include "boot.h"

void kernel_main(void);
void gpu_power_id_client(void *unused);

/* One ON of the power-only sequence, then, while the controller still polls
 * CORE6 status at its last step, three stages of GPU wrapper writes
 * (docs/gpu-power.md, "The wrapper-write probe sets the core-0 controls one
 * at a time"). The wrapper registers are those of the vendor
 * sunxi_dvfs_ctrl.c:113-150; the vendor writes neither. The accesses are the
 * rows of wrapper_steps, in order: the two entry reads, each stage's write
 * and readback, and the two restore writes and readbacks. A stage that
 * brings CORE6 to status 8 skips the remaining stages. Every path through
 * the stages ends with the restore, OFF and provider park. The progress
 * record between the result header and the log, and a log line, name each
 * access before it is issued. An access that never completes stops the CPU;
 * then nothing returns and only a power removal recovers the board. */
#define WRAPPER_ON_US 200000u
#define WRAPPER_STAGE_US 20000u
#define WRAPPER_RESERVE_US 300000u
#define WRAPPER_STAGES 3u
#define WRAPPER_MAGIC UINT64_C(0x3130575257555047) /* "GPUWRW01" */
#define WRAPPER_PAGE UINT32_C(0x01880000)
struct wrapper_step { uint32_t address, value, write; };
const struct wrapper_step wrapper_steps[12] = {
    {0x01880104, 0, 0}, {0x01880100, 0, 0},                 /* entry: both 0 */
    {0x01880100, 0x00640064, 1}, {0x01880100, 0, 0},        /* stage 1: delays 100 up, 100 down */
    {0x01880104, 0x100, 1}, {0x01880104, 0, 0},             /* stage 2: core-0 clock enable */
    {0x01880104, 0x107, 1}, {0x01880104, 0, 0},             /* stage 3: software control, active, accept */
    {0x01880104, 0, 1}, {0x01880104, 0, 0},                 /* restore */
    {0x01880100, 0, 1}, {0x01880100, 0, 0}};
#define RESTORE_STEP 8u
struct wrapper_progress { uint64_t magic, stage, count, stamp; };
#define WRAPPER_PROGRESS ((volatile struct wrapper_progress *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct boot_result) == 128 && 128 + sizeof(struct wrapper_progress) <= LOG_OFFSET,
               "the progress record lies between the result header and the log");
static unsigned wrapper_done, wrapper_next;

static void print_power(void)
{
    const struct a733_gpu_power *p = &gpu.power;
    struct bb_line line;
    bb_line_start(&line, "GPU power");
    bb_line_dec(&line, "phase", p->phase); bb_line_dec(&line, "error", p->error);
    bb_line_dec(&line, "step", p->index); bb_line_hex(&line, "last", p->last_value);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU resources");
    bb_line_dec(&line, "result", gpu.resource_result); bb_line_dec(&line, "error", bb_power.gpu.error);
    bb_line_dec(&line, "stage", bb_power.gpu.stage); bb_line_hex(&line, "last", bb_power.gpu.last_value);
    bb_line_end(&line); bb_power_print(&line);
    core_report();
}

/* Access i of wrapper_steps; i only moves forward. A read returns its value,
 * a write its value after DSB SY. */
static uint32_t wrapper_access(unsigned i)
{
    if (i >= sizeof(wrapper_steps) / sizeof(*wrapper_steps) || i < wrapper_next)
        bb_power_fail("GPU wrapper write order");
    const struct wrapper_step *s = &wrapper_steps[i];
    before();
    WRAPPER_PROGRESS->stage = i + 1;
    WRAPPER_PROGRESS->stamp = counter();
    __asm__ volatile("dsb sy" ::: "memory");
    struct bb_line line;
    bb_line_start(&line, s->write ? "GPU WRAPPER writing" : "GPU WRAPPER reading");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", s->address);
    if (s->write) bb_line_hex(&line, "v", s->value);
    bb_line_end(&line); bb_power_print(&line);
    uint32_t value = s->value;
    if (s->write) a733_mmio_write32_dsb_sy_both(s->address, s->value);
    else value = a733_mmio_read32_dsb_sy_both(s->address);
    wrapper_next = i + 1;
    WRAPPER_PROGRESS->count = ++wrapper_done;
    before();
    if (!s->write) {
        bb_line_start(&line, "GPU WRAPPER");
        bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", s->address); bb_line_hex(&line, "v", value);
        bb_line_end(&line); bb_power_print(&line);
    }
    return value;
}

/* Polls the controller until CORE6 reads status 8 (1), the controller fails
 * (-1) or the bound passes with the poll still pending (0). */
static int wait_on(uint64_t until)
{
    int result;
    while ((result = a733_gpu_platform_poll(&gpu)) == 0) {
        if (bb_power_now() >= until) return 0;
        vTaskDelay(1);
    }
    return result;
}

void gpu_power_id_client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    WRAPPER_PROGRESS->magic = WRAPPER_MAGIC;
    WRAPPER_PROGRESS->stage = WRAPPER_PROGRESS->count = 0;
    WRAPPER_PROGRESS->stamp = counter();
    snapshot(0);
    cycle = 1;
    before();
    /* The controller keeps the client deadline; the client's own bounds end
     * each wait, and the reserve covers the restore, OFF and park. */
    const uint64_t on_deadline = last_now + WRAPPER_ON_US;
    if (on_deadline + WRAPPER_STAGES * WRAPPER_STAGE_US + WRAPPER_RESERVE_US > bb_power.deadline)
        bb_power_fail("GPU wrapper write time budget");
    if (!a733_gpu_platform_begin(&gpu, bb_power.deadline)) gpu_fail("GPU ON begin");
    int result = wait_on(on_deadline);
    snapshot(1);
    print_power();
    if (result < 0) gpu_fail("GPU ON poll");
    unsigned stage = 0, refused = 0, late = 0;
    if (result == 0) {
        /* The MISR read after the reset release precedes only the final
         * status poll, so the controller is polling CORE6 status. */
        if (gpu.phase != A733_GPU_PLATFORM_STARTING || gpu.power.phase != A733_GPU_POWER_STARTING ||
            !(gpu.power.core_seen & (UINT32_C(8) << A733_GPU_NOTE_MISR_LATE)))
            gpu_fail("GPU wrapper write refused before the final CORE6 poll");
        static const struct a733_mapping_rule device = {A733_MAPPING_READ_WRITE, 1,
            {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 1, 1};
        before();
        if (!a733_mapping_check(aarch64_device_translate, WRAPPER_PAGE, WRAPPER_PAGE, &device))
            gpu_fail("GPU wrapper mapping");
        const uint32_t qchan = wrapper_access(0), delay = wrapper_access(1);
        refused = qchan || delay;
        for (unsigned k = 1; !refused && k <= WRAPPER_STAGES && result == 0; ++k) {
            (void)wrapper_access(2 * k);
            const uint32_t readback = wrapper_access(2 * k + 1);
            struct bb_line line;
            bb_line_start(&line, "GPU WRAPPER stage");
            bb_line_dec(&line, "k", k); bb_line_hex(&line, "readback", readback);
            bb_line_hex(&line, "status", a733_mmio_read32_dsb_sy_both(0x07066008));
            bb_line_hex(&line, "disr", a733_mmio_read32_dsb_sy_both(0x07066010));
            bb_line_hex(&line, "misr", a733_mmio_read32_dsb_sy_both(0x07066014));
            bb_line_end(&line); bb_power_print(&line);
            before();
            const uint64_t start = last_now;
            result = wait_on(start + WRAPPER_STAGE_US);
            before();
            bb_line_start(&line, "GPU WRAPPER stage");
            bb_line_dec(&line, "k", k); bb_line_dec(&line, "on", result == 1);
            bb_line_hex(&line, "core_status", gpu.power.last_value);
            bb_line_dec(&line, "waited_us", last_now - start);
            bb_line_end(&line); bb_power_print(&line);
            if (result < 0) gpu_fail("GPU ON poll");
            if (result == 1) stage = k;
        }
        if (!refused) {
            for (unsigned i = RESTORE_STEP; i < sizeof(wrapper_steps) / sizeof(*wrapper_steps); ++i)
                (void)wrapper_access(i);
            /* One more poll: status 8 that arrives after the last stage's
             * bound belongs to no stage and is reported as late. */
            if (result == 0 && (result = a733_gpu_platform_poll(&gpu)) < 0) gpu_fail("GPU ON poll");
            late = !stage && result == 1;
            /* CORE6 after the restore, which removes any forced accept. */
            struct bb_line line;
            bb_line_start(&line, "GPU WRAPPER restored");
            bb_line_hex(&line, "status", a733_mmio_read32_dsb_sy_both(0x07066008));
            bb_line_hex(&line, "disr", a733_mmio_read32_dsb_sy_both(0x07066010));
            bb_line_hex(&line, "misr", a733_mmio_read32_dsb_sy_both(0x07066014));
            bb_line_end(&line); bb_power_print(&line);
        }
    }
    const int on = result == 1;
    struct bb_line line;
    bb_line_start(&line, "GPU WRAPPER result");
    bb_line_dec(&line, "stage", stage); bb_line_dec(&line, "on", on); bb_line_dec(&line, "late", late);
    bb_line_dec(&line, "accesses", wrapper_done);
    bb_line_end(&line); bb_power_print(&line);
    before();
    /* After an abandoned ON, status 0 does not show that CORE6's transition
     * ended; snapshot 2 records CORE6 and TOP5 after every OFF attempt. */
    if (on ? !a733_gpu_platform_off_begin(&gpu, bb_power.deadline) :
             !a733_gpu_platform_abandon(&gpu, bb_power.deadline)) {
        snapshot(2);
        gpu_fail("GPU OFF begin");
    }
    while ((result = a733_gpu_platform_off_poll(&gpu)) == 0) {
        if (bb_power_now() >= bb_power.deadline) { result = -2; break; }
        vTaskDelay(1);
    }
    snapshot(2);
    if (result == -2) gpu_fail("GPU OFF deadline");
    if (result != 1) gpu_fail("GPU OFF poll");
    off_proof();
    bb_power_park();
    off_proof();
    if (!atomic_load_explicit(&bb_power.parked, memory_order_acquire) ||
        !a733_power_service_idle(&bb_power.domains)) bb_power_fail("GPU provider park proof");
    for (unsigned i = 0; i < A733_PMIC_REQUESTS; ++i)
        if (bb_power.pmic.slots[i].state || bb_power.pmic.slots[i].borrow.id)
            bb_power_fail("GPU provider request retained");
    before();
    if (refused) bb_power_fail("GPU wrapper write refused: wrapper words not 0, OFF and provider parked");
    if (!on)
        bb_power_fail("GPU wrapper write: CORE6 not ON after three stages, OFF and provider parked");
    platform_puts("COMPLETE GPU wrapper write: CORE status 8, OFF and provider parked\n");
    platform_exit(0);
}

void kernel_main(void)
{
    platform_init();
    struct a733_pmic_lifetime life;
    if (!bb_power_snapshot_init(gpu_power_id_client, "gpu-id") ||
        !a733_gpu_platform_native_init(&native, &bb_power.locks.callbacks) ||
        !a733_gpu_platform_init(&gpu, &bb_power.gpu, GPU_CLIENT, &native.operations, &native) ||
        a733_pmic_identity(&bb_power.pmic, &life) != A733_PMIC_READY ||
        !a733_gpu_platform_bind_pmic(&gpu, &bb_power.gpu_native, &bb_power.pmic, &life, BB_POWER_CLIENT_OWNER))
        platform_panic("GPU wrapper initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
