/* SPDX-License-Identifier: MIT
 * The vendor-order variant of the GPU power diagnostic. It compiles main.c
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

/* One ON of the power-only sequence through the reset release, with the
 * CORE6 status poll kept only as an observation; then the vendor's secure
 * configuration and a read of the GPU core identity (docs/gpu-power.md,
 * "The vendor-order probe makes the vendor's secure configuration before the
 * core read"). The accesses are the rows of vendor_steps, in order: four
 * entry reads, the two writes of sunxi_secure_config (sunxi_platform.c:
 * 964-979) in its order, each read back, the core identity read that the
 * vendor makes first (rgxbvnc.c:119): its OSReadHWReg64 is two 32-bit reads,
 * +0x24 then +0x20 in source order (osfunc.h:1086-1095), and the restore of
 * both written words to their entry values, each read back. The progress record between
 * the result header and the log holds each access's number before it is
 * issued and each value after it; a log line names each access too. An
 * access that never completes stops the CPU; then nothing returns and only
 * a power removal recovers the board. */
#define VENDOR_ON_US 200000u
#define VENDOR_RESERVE_US 300000u
#define VENDOR_MAGIC UINT64_C(0x3130444e56555047) /* "GPUVND01" */
#define WRAPPER_PAGE UINT32_C(0x01880000)
#define SMC0_PAGE UINT32_C(0x0a000000)
enum vendor_kind { READ32, WRITE32, RESTORE32 };
/* RESTORE32 writes the value read by the row named in value. */
struct vendor_step { uint32_t address, value, kind; };
const struct vendor_step vendor_steps[14] = {
    {0x01880014, 0, READ32},        /* drm_ctrl */
    {0x01880010, 0, READ32},        /* irq_ctrl */
    {0x01880018, 0, READ32},        /* mult_core_cnt */
    {0x0a0000b0, 0, READ32},        /* SMC0 +0xb0, SMC_DRM_GPU_HW_RST */
    {0x0a0000b0, 2, WRITE32},       /* sunxi_platform.c:969-970 */
    {0x0a0000b0, 0, READ32},
    {0x01880014, 0, WRITE32},       /* sunxi_platform.c:973-975 */
    {0x01880014, 0, READ32},
    {0x01800024, 0, READ32},        /* RGX_CR_CORE_ID__PBVNC, high word */
    {0x01800020, 0, READ32},        /* RGX_CR_CORE_ID__PBVNC, low word */
    {0x01880014, 0, RESTORE32},     /* drm_ctrl as read at row 0 */
    {0x01880014, 0, READ32},
    {0x0a0000b0, 3, RESTORE32},     /* SMC0 +0xb0 as read at row 3 */
    {0x0a0000b0, 0, READ32}};
#define VENDOR_STEPS (sizeof(vendor_steps) / sizeof(*vendor_steps))
#define IDENTITY_HIGH 8u
#define IDENTITY_LOW 9u
/* value[n] holds the value of row n. */
struct vendor_progress { uint64_t magic, stage, count, stamp; uint32_t value[VENDOR_STEPS]; };
#define VENDOR_PROGRESS ((volatile struct vendor_progress *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct boot_result) == 128 && 128 + sizeof(struct vendor_progress) <= LOG_OFFSET,
               "the progress record lies between the result header and the log");
static unsigned vendor_done, vendor_next;
static uint32_t vendor_value[VENDOR_STEPS];

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

/* Row i of vendor_steps; i only moves forward. */
static uint32_t vendor_access(unsigned i)
{
    if (i >= VENDOR_STEPS || i < vendor_next) bb_power_fail("GPU vendor order");
    const struct vendor_step *s = &vendor_steps[i];
    const int write = s->kind == WRITE32 || s->kind == RESTORE32;
    const uint32_t store = s->kind == RESTORE32 ? vendor_value[s->value] : s->value;
    before();
    VENDOR_PROGRESS->stage = i + 1;
    VENDOR_PROGRESS->stamp = counter();
    __asm__ volatile("dsb sy" ::: "memory");
    struct bb_line line;
    bb_line_start(&line, write ? "GPU VENDOR writing" : "GPU VENDOR reading");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", s->address);
    if (write) bb_line_hex(&line, "v", store);
    bb_line_end(&line); bb_power_print(&line);
    uint32_t value = store;
    if (write) a733_mmio_write32_dsb_sy_both(s->address, store);
    else value = a733_mmio_read32_dsb_sy_both(s->address);
    vendor_value[i] = value;
    VENDOR_PROGRESS->value[i] = value;
    vendor_next = i + 1;
    VENDOR_PROGRESS->count = ++vendor_done;
    __asm__ volatile("dsb sy" ::: "memory");
    before();
    if (!write) {
        bb_line_start(&line, "GPU VENDOR");
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
    VENDOR_PROGRESS->magic = VENDOR_MAGIC;
    VENDOR_PROGRESS->stage = VENDOR_PROGRESS->count = 0;
    for (unsigned i = 0; i < VENDOR_STEPS; ++i) VENDOR_PROGRESS->value[i] = 0;
    VENDOR_PROGRESS->stamp = counter();
    snapshot(0);
    cycle = 1;
    before();
    /* The controller keeps the client deadline. The client's 200 ms bound
     * ends the CORE6 status observation, and the reserve covers the
     * accesses, OFF and park. */
    const uint64_t start = last_now, on_deadline = last_now + VENDOR_ON_US;
    if (on_deadline + VENDOR_RESERVE_US > bb_power.deadline) bb_power_fail("GPU vendor order time budget");
    if (!a733_gpu_platform_begin(&gpu, bb_power.deadline)) gpu_fail("GPU ON begin");
    int result = wait_on(on_deadline);
    before();
    snapshot(1);
    print_power();
    struct bb_line line;
    bb_line_start(&line, "GPU VENDOR core6");
    bb_line_dec(&line, "on", result == 1); bb_line_hex(&line, "status", gpu.power.last_value);
    bb_line_dec(&line, "waited_us", last_now - start);
    bb_line_end(&line); bb_power_print(&line);
    if (result < 0) gpu_fail("GPU ON poll");
    /* The MISR read after the reset release precedes only the final status
     * poll: the clocks are open and the reset released, as in the vendor's
     * sunxi_platform_init before its first core access. */
    if (result == 0 && (gpu.phase != A733_GPU_PLATFORM_STARTING || gpu.power.phase != A733_GPU_POWER_STARTING ||
                        !(gpu.power.core_seen & (UINT32_C(8) << A733_GPU_NOTE_MISR_LATE))))
        gpu_fail("GPU vendor order refused before clock and reset progress");
    static const struct a733_mapping_rule read_write = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 1, 1};
    static const struct a733_mapping_rule read_only = {A733_MAPPING_READ, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    before();
    if (!a733_mapping_check(aarch64_device_translate, WRAPPER_PAGE, WRAPPER_PAGE, &read_write) ||
        !a733_mapping_check(aarch64_device_translate, SMC0_PAGE, SMC0_PAGE, &read_write) ||
        !a733_mapping_check(aarch64_device_translate, GPU_BASE, GPU_BASE, &read_only))
        gpu_fail("GPU vendor order mapping");
    for (unsigned i = 0; i < VENDOR_STEPS; ++i) {
        (void)vendor_access(i);
        if (i == IDENTITY_LOW) {
            const uint64_t value = (uint64_t)vendor_value[IDENTITY_HIGH] << 32 | vendor_value[IDENTITY_LOW];
            bb_line_start(&line, "GPU VENDOR identity");
            bb_line_hex(&line, "raw", value);
            bb_line_dec(&line, "b", value >> 48); bb_line_dec(&line, "v", (value >> 32) & 0xffff);
            bb_line_dec(&line, "n", (value >> 16) & 0xffff); bb_line_dec(&line, "c", value & 0xffff);
            bb_line_dec(&line, "matches", value == EXPECTED_BVNC);
            bb_line_end(&line); bb_power_print(&line);
        }
    }
    /* One more observation of CORE6 status after the accesses. */
    if (result == 0 && (result = a733_gpu_platform_poll(&gpu)) < 0) gpu_fail("GPU ON poll");
    const int on = result == 1,
        matches = ((uint64_t)vendor_value[IDENTITY_HIGH] << 32 | vendor_value[IDENTITY_LOW]) == EXPECTED_BVNC;
    bb_line_start(&line, "GPU VENDOR result");
    bb_line_dec(&line, "core6_on", on); bb_line_dec(&line, "identity_matches", matches);
    bb_line_dec(&line, "accesses", vendor_done);
    bb_line_end(&line); bb_power_print(&line);
    before();
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
    if (!matches) bb_power_fail("GPU vendor order: identity not 36.56.104.183, OFF and provider parked");
    platform_puts("COMPLETE GPU vendor order: identity 36.56.104.183, OFF and provider parked\n");
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
        platform_panic("GPU vendor-order initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
