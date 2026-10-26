/* SPDX-License-Identifier: MIT
 * The probe-read variant of the GPU power diagnostic: the vendor's
 * probe-time read of the GPU core identity, then the vendor's secure
 * configuration and the same read again. It compiles main.c as the
 * power-only variant with its client and entry renamed, so main.c and the
 * power-only image stay unchanged, and adds its own client and entry.
 */
#define GPU_POWER_ONLY 1
#define kernel_main gpu_power_only_kernel_main
#define gpu_power_id_client gpu_power_only_client
#include "main.c"
#undef kernel_main
#undef gpu_power_id_client
#include "boot.h"

void kernel_main(void);
void gpu_power_id_client(void *unused);

/* One complete ON of the default GPU power path, which must reach CORE6
 * status 8 within 200 ms before any GPU-window access (docs/gpu-power.md,
 * "The probe-read probe makes the vendor's probe-time read, then the secure
 * configuration"; docs/designs/gpu-firmware-start.md). The accesses are the
 * rows of probe_steps, in order:
 * - reads of drm_ctrl, irq_ctrl and the SMC0 word;
 * - the core identity as the vendor reads it at probe time
 *   (rgxbvnc.c:119): OSReadHWReg64 is two 32-bit reads, +0x24 then +0x20
 *   in source order (osfunc.h:1086-1095);
 * - the two writes of sunxi_secure_config (sunxi_platform.c:964-979) in its
 *   order, each read back, and the identity read again;
 * - drm_ctrl and the SMC0 word written back to their entry values, each
 *   read back.
 * From the first write to the last restore readback the client masks
 * D, A, I and F, makes no deadline check and has no failure path, and
 * VBAR_EL1 points at probe_restore_vectors, whose every slot writes both
 * words back to their entry values before it enters the image's own slot.
 * Those slots cover a synchronous exception; an SError stays masked until
 * both words are restored. The monitor's 2 s recovery timer is a FIQ taken
 * at EL3 through the monitor's own vectors, which the EL1 FIQ mask does not
 * stop and the restore slots never see; the client therefore starts the
 * window only while at least 500 ms remain before that deadline after a
 * 10 ms window bound. A recovery inside the window, or an access that never
 * completes, would leave both words written until the power removal. The progress
 * record between the result header and the log holds the row in flight
 * and each completed value. After the restore the client powers the GPU
 * off, ends its supply sample and parks the provider, as the power-only
 * client does, so the lifetime can continue to the firmware start. */
#define PROBE_ON_US 200000u
/* The monitor returns to the ROM 2 s after its entry (start_counter). The
 * window must end at least 500 ms before that. The wrapper-read run on port
 * 3-1 finished its last read, two log lines and its exit within 15 us, so
 * 10 ms bounds the window's ten accesses and their log lines. A refusal
 * returns status 7, which no other path of this image uses. */
#define MONITOR_DEADLINE_US 2000000u
#define WINDOW_BOUND_US 10000u
#define WINDOW_SLACK_US 500000u
#define PROBE_WINDOW_REFUSED 7u
#define PROBE_RESERVE_US 100000u
#define PROBE_MAGIC UINT64_C(0x3130445250555047) /* "GPUPRD01" */
#define WRAPPER_PAGE UINT32_C(0x01880000)
#define SMC0_PAGE UINT32_C(0x0a000000)
#define DRM_CTRL UINT32_C(0x01880014)
#define SMC0_GPU UINT32_C(0x0a0000b0)
enum probe_kind { READ32, WRITE32, RESTORE32 };
/* RESTORE32 writes the value read by the row named in value. */
struct probe_step { uint32_t address, value, kind; };
const struct probe_step probe_steps[15] = {
    {DRM_CTRL, 0, READ32},          /* drm_ctrl */
    {0x01880010, 0, READ32},        /* irq_ctrl */
    {SMC0_GPU, 0, READ32},          /* SMC0 +0xb0, SMC_DRM_GPU_HW_RST */
    {0x01800024, 0, READ32},        /* RGX_CR_CORE_ID__PBVNC, high word */
    {0x01800020, 0, READ32},        /* RGX_CR_CORE_ID__PBVNC, low word */
    {SMC0_GPU, 2, WRITE32},         /* sunxi_platform.c:969-970 */
    {SMC0_GPU, 0, READ32},
    {DRM_CTRL, 0, WRITE32},         /* sunxi_platform.c:973-975 */
    {DRM_CTRL, 0, READ32},
    {0x01800024, 0, READ32},
    {0x01800020, 0, READ32},
    {DRM_CTRL, 0, RESTORE32},       /* drm_ctrl as read at row 0 */
    {DRM_CTRL, 0, READ32},
    {SMC0_GPU, 2, RESTORE32},       /* SMC0 +0xb0 as read at row 2 */
    {SMC0_GPU, 0, READ32}};
#define PROBE_STEPS (sizeof(probe_steps) / sizeof(*probe_steps))
#define FIRST_WRITE 5u
/* value[n] holds the value of row n. */
struct probe_progress { uint64_t magic, stage, count, stamp; uint32_t value[PROBE_STEPS]; };
#define PROBE_PROGRESS ((volatile struct probe_progress *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct boot_result) == 128 && 128 + sizeof(struct probe_progress) <= LOG_OFFSET,
               "the progress record lies between the result header and the log");
static unsigned probe_done;
static uint32_t probe_value[PROBE_STEPS];
/* The entry values that probe_restore_vectors writes back: drm_ctrl, then
 * the SMC0 word, the reverse of the vendor's write order. */
struct { uint32_t drm_ctrl, smc0; } probe_restore;

/* Sixteen 128-byte slots, each of which writes both words back from
 * probe_restore and then enters the same slot of the image's table. */
__asm__(
    ".pushsection .text.probe_vectors, \"ax\"\n"
    ".balign 2048\n"
    ".global probe_restore_vectors\n"
    "probe_restore_vectors:\n"
    ".irp n, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15\n"
    ".balign 128\n"
    "    stp x15, x16, [sp, #-32]!\n"
    "    str x17, [sp, #16]\n"
    "    adrp x15, probe_restore\n"
    "    add x15, x15, :lo12:probe_restore\n"
    "    ldr w16, [x15]\n"
    "    movz x17, #0x0014\n"
    "    movk x17, #0x0188, lsl #16\n"
    "    str w16, [x17]\n"
    "    dsb sy\n"
    "    ldr w16, [x15, #4]\n"
    "    movz x17, #0x00b0\n"
    "    movk x17, #0x0a00, lsl #16\n"
    "    str w16, [x17]\n"
    "    dsb sy\n"
    "    ldr x17, [sp, #16]\n"
    "    ldp x15, x16, [sp], #32\n"
    "    b _freertos_vector_table + \\n * 128\n"
    ".endr\n"
    ".size probe_restore_vectors, . - probe_restore_vectors\n"
    ".popsection\n");
extern const char probe_restore_vectors[], _freertos_vector_table[];

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

/* Row i of probe_steps, in table order. Outside the write window the
 * deadline and clock are checked around the access; inside it nothing can
 * fail. */
static uint32_t probe_access(unsigned i, int checked)
{
    const struct probe_step *s = &probe_steps[i];
    const int write = s->kind == WRITE32 || s->kind == RESTORE32;
    const uint32_t store = s->kind == RESTORE32 ? probe_value[s->value] : s->value;
    if (checked) before();
    PROBE_PROGRESS->stage = i + 1;
    PROBE_PROGRESS->stamp = counter();
    __asm__ volatile("dsb sy" ::: "memory");
    struct bb_line line;
    bb_line_start(&line, write ? "GPU PROBE writing" : "GPU PROBE reading");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", s->address);
    if (write) bb_line_hex(&line, "v", store);
    bb_line_end(&line); bb_power_print(&line);
    uint32_t value = store;
    if (write) a733_mmio_write32_dsb_sy_both(s->address, store);
    else value = a733_mmio_read32_dsb_sy_both(s->address);
    probe_value[i] = value;
    PROBE_PROGRESS->value[i] = value;
    PROBE_PROGRESS->count = ++probe_done;
    __asm__ volatile("dsb sy" ::: "memory");
    if (checked) before();
    if (!write) {
        bb_line_start(&line, "GPU PROBE");
        bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", s->address); bb_line_hex(&line, "v", value);
        bb_line_end(&line); bb_power_print(&line);
    }
    return value;
}

static uint64_t identity(unsigned high)
{
    return (uint64_t)probe_value[high] << 32 | probe_value[high + 1];
}

/* prefix names the read: before or after the secure configuration. */
static void print_identity(const char *prefix, uint64_t value)
{
    struct bb_line line;
    bb_line_start(&line, prefix);
    bb_line_hex(&line, "raw", value);
    bb_line_dec(&line, "b", value >> 48); bb_line_dec(&line, "v", (value >> 32) & 0xffff);
    bb_line_dec(&line, "n", (value >> 16) & 0xffff); bb_line_dec(&line, "c", value & 0xffff);
    bb_line_dec(&line, "matches", value == EXPECTED_BVNC);
    bb_line_end(&line); bb_power_print(&line);
}

void gpu_power_id_client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    PROBE_PROGRESS->magic = PROBE_MAGIC;
    PROBE_PROGRESS->stage = PROBE_PROGRESS->count = 0;
    for (unsigned i = 0; i < PROBE_STEPS; ++i) PROBE_PROGRESS->value[i] = 0;
    PROBE_PROGRESS->stamp = counter();
    snapshot(0);
    cycle = 1;
    before();
    /* The controller keeps the client deadline. The client's 200 ms bound
     * ends the wait for CORE6 status 8; the reserve covers the accesses. */
    const uint64_t start = last_now, on_deadline = last_now + PROBE_ON_US;
    if (on_deadline + PROBE_RESERVE_US > bb_power.deadline) bb_power_fail("GPU probe read time budget");
    if (!a733_gpu_platform_begin(&gpu, bb_power.deadline)) gpu_fail("GPU ON begin");
    int result;
    while ((result = a733_gpu_platform_poll(&gpu)) == 0) {
        if (bb_power_now() >= on_deadline) break;
        vTaskDelay(1);
    }
    before();
    snapshot(1);
    print_power();
    struct bb_line line;
    bb_line_start(&line, "GPU PROBE core6");
    bb_line_dec(&line, "on", result == 1); bb_line_hex(&line, "status", gpu.power.last_value);
    bb_line_dec(&line, "waited_us", last_now - start);
    bb_line_end(&line); bb_power_print(&line);
    if (result < 0) gpu_fail("GPU ON poll");
    /* No GPU-window access without CORE6 ON: the default path reaches it,
     * and the identity reads have only ever completed after it. */
    if (result != 1) gpu_fail("GPU ON deadline");
    static const struct a733_mapping_rule read_write = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 1, 1};
    static const struct a733_mapping_rule read_only = {A733_MAPPING_READ, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    before();
    if (!a733_mapping_check(aarch64_device_translate, WRAPPER_PAGE, WRAPPER_PAGE, &read_write) ||
        !a733_mapping_check(aarch64_device_translate, SMC0_PAGE, SMC0_PAGE, &read_write) ||
        !a733_mapping_check(aarch64_device_translate, GPU_BASE, GPU_BASE, &read_only))
        gpu_fail("GPU probe read mapping");
    for (unsigned i = 0; i < FIRST_WRITE; ++i) (void)probe_access(i, 1);
    print_identity("GPU PROBE identity before", identity(3));

    /* The write window. The restore vectors enter the image's own table, so
     * that table must be the one in use. */
    uint64_t vbar;
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(vbar));
    if (vbar != (uintptr_t)_freertos_vector_table) bb_power_fail("GPU probe read vector table");
    const uint64_t now = bb_power_now(), monitor_deadline = RESULT->start_counter / 24 + MONITOR_DEADLINE_US;
    if (now + WINDOW_BOUND_US + WINDOW_SLACK_US > monitor_deadline) {
        bb_line_start(&line, "GPU PROBE window refused");
        bb_line_dec(&line, "now_us", now); bb_line_dec(&line, "monitor_deadline_us", monitor_deadline);
        bb_line_end(&line); bb_power_print(&line);
        platform_puts("FAIL GPU probe read window refused: monitor deadline too near, nothing written\n");
        platform_exit(PROBE_WINDOW_REFUSED);
    }
    probe_restore.drm_ctrl = probe_value[0];
    probe_restore.smc0 = probe_value[2];
    uint64_t daif, isr;
    __asm__ volatile("mrs %0, daif\n msr daifset, #0xf\n dsb sy\n msr vbar_el1, %1\n isb"
                     : "=&r"(daif) : "r"(probe_restore_vectors) : "memory");
    for (unsigned i = FIRST_WRITE; i < PROBE_STEPS; ++i) (void)probe_access(i, 0);
    __asm__ volatile("isb\n mrs %0, isr_el1\n msr vbar_el1, %1\n isb"
                     : "=&r"(isr) : "r"(_freertos_vector_table) : "memory");
    /* A pending asynchronous abort is reported before DAIF is restored;
     * both words already hold their entry values when it is taken. */
    bb_line_start(&line, "GPU PROBE window");
    bb_line_dec(&line, "serror_pending", (isr >> 8) & 1);
    bb_line_end(&line); bb_power_print(&line);
    __asm__ volatile("msr daif, %0\n isb" :: "r"(daif) : "memory");

    const uint64_t before_id = identity(3), after_id = identity(9);
    print_identity("GPU PROBE identity after", after_id);
    const int restored = probe_value[12] == probe_value[0] && probe_value[14] == probe_value[2];
    const int complete = before_id == EXPECTED_BVNC && after_id == EXPECTED_BVNC && restored && !((isr >> 8) & 1);
    bb_line_start(&line, "GPU PROBE result");
    bb_line_dec(&line, "before", before_id == EXPECTED_BVNC); bb_line_dec(&line, "after", after_id == EXPECTED_BVNC);
    bb_line_dec(&line, "restored", restored); bb_line_dec(&line, "accesses", probe_done);
    bb_line_end(&line); bb_power_print(&line);
    before();
    if (!complete) bb_power_fail("GPU probe read: identity or restore not as expected, GPU left powered");
    /* The OFF, supply end and park of the power-only client. */
    if (!a733_gpu_platform_off_begin(&gpu, bb_power.deadline)) gpu_fail("GPU OFF begin");
    while ((result = a733_gpu_platform_off_poll(&gpu)) == 0) gpu_pause("GPU OFF deadline");
    if (result != 1) gpu_fail("GPU OFF poll");
    gates_report();
    off_proof();
    while ((result = a733_gpu_platform_supply_end(&gpu)) == 0) gpu_pause("GPU supply end deadline");
    if (result != 1) gpu_fail("GPU supply end");
    bb_power_park();
    off_proof();
    if (!atomic_load_explicit(&bb_power.parked, memory_order_acquire) ||
        !a733_power_service_idle(&bb_power.domains)) bb_power_fail("GPU provider park proof");
    for (unsigned i = 0; i < A733_PMIC_REQUESTS; ++i)
        if (bb_power.pmic.slots[i].state || bb_power.pmic.slots[i].borrow.id)
            bb_power_fail("GPU provider request retained");
    before();
    platform_puts("COMPLETE GPU probe read: identity 36.56.104.183 before and after the secure configuration, "
                  "restored, OFF and provider parked\n");
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
        platform_panic("GPU probe-read initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
