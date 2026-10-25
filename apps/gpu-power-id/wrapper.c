/* SPDX-License-Identifier: MIT
 * The wrapper-read variant of the GPU power diagnostic. It compiles main.c
 * as the power-only variant with its client and entry renamed, so main.c and
 * the power-only image stay unchanged, and adds its own client and entry.
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

/* The controller's failure state and the resource manager's record, then the
 * CORE6 observations: the lines gpu_fail prints, without ending the run. */
static void power_report(void)
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

/* The wrapper-read variant: one ON as in the power-only variant, with its own
 * 200 ms bound, then four 32-bit reads of the GPU wrapper registers that the
 * vendor DVFS source names (sunxi_dvfs_ctrl.c:113-150), in this order:
 * pwr_qchan_cfg, pwr_cmp_dly, mult_core_cnt and mult_core_id. They are the
 * only GPU-window accesses of this image, and there is no GPU-window write.
 * Every observation the image already holds is printed before the first
 * wrapper read. Before each read the progress record between the result
 * header and the log names the read, and the log names it too, so a run that
 * returns shows how far it went. A read that never completes stops the CPU;
 * then nothing returns and only a power removal recovers the board. */
#define WRAPPER_ON_US 200000u
#define WRAPPER_RESERVE_US 100000u
#define WRAPPER_MAGIC UINT64_C(0x3130505257555047) /* "GPUWRP01" */
const uint32_t wrapper_words[4] = {0x01880104, 0x01880100, 0x01880018, 0x0188001c};
struct wrapper_progress { uint64_t magic, stage, count, stamp; };
#define WRAPPER_PROGRESS ((volatile struct wrapper_progress *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct boot_result) == 128 && 128 + sizeof(struct wrapper_progress) <= LOG_OFFSET,
               "the progress record lies between the result header and the log");
static unsigned wrapper_reads;

static uint32_t wrapper_read(unsigned i)
{
    static const struct a733_mapping_rule device = {A733_MAPPING_READ, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    if (i >= sizeof(wrapper_words) / sizeof(*wrapper_words) || i != wrapper_reads)
        bb_power_fail("GPU wrapper read order");
    const uintptr_t address = wrapper_words[i];
    before();
    if (!a733_mapping_check(aarch64_device_translate, address & ~(uintptr_t)0xfff,
                            address & ~(uintptr_t)0xfff, &device))
        bb_power_fail("GPU wrapper mapping");
    before();
    WRAPPER_PROGRESS->stage = i + 1;
    WRAPPER_PROGRESS->stamp = counter();
    __asm__ volatile("dsb sy" ::: "memory");
    struct bb_line line;
    bb_line_start(&line, "GPU WRAPPER reading");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", address);
    bb_line_end(&line); bb_power_print(&line);
    const uint32_t value = a733_mmio_read32_dsb_sy_both(address);
    WRAPPER_PROGRESS->count = ++wrapper_reads;
    before();
    bb_line_start(&line, "GPU WRAPPER");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", address); bb_line_hex(&line, "v", value);
    bb_line_end(&line); bb_power_print(&line);
    return value;
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
    /* The ON wait ends 200 ms after this sample, and at least 100 ms of the
     * client deadline must remain for the reports and the wrapper reads. */
    const uint64_t on_deadline = last_now + WRAPPER_ON_US;
    if (on_deadline + WRAPPER_RESERVE_US > bb_power.deadline) bb_power_fail("GPU wrapper read time budget");
    if (!a733_gpu_platform_begin(&gpu, on_deadline)) gpu_fail("GPU ON begin");
    int result;
    while ((result = a733_gpu_platform_poll(&gpu)) == 0) {
        if (bb_power_now() >= on_deadline) { result = -2; break; }
        vTaskDelay(1);
    }
    snapshot(1);
    power_report();
    /* The wrapper is read only after the controller completed the GPU clock,
     * bus gate and reset release steps: the MISR read that follows them is
     * the last observation before the final status poll. */
    if (result != 1 && !(gpu.power.core_seen & (UINT32_C(8) << A733_GPU_NOTE_MISR_LATE)))
        bb_power_fail("GPU wrapper read refused before clock and reset progress");
    uint32_t words[4];
    for (unsigned i = 0; i < 4; ++i) words[i] = wrapper_read(i);
    struct bb_line line;
    bb_line_start(&line, "GPU WRAPPER qchan");
    bb_line_dec(&line, "core0_sw", words[0] & 1); bb_line_dec(&line, "core0_active", (words[0] >> 1) & 1);
    bb_line_dec(&line, "core0_accept", (words[0] >> 2) & 1); bb_line_dec(&line, "core0_deny", (words[0] >> 3) & 1);
    bb_line_dec(&line, "core1_sw", (words[0] >> 4) & 1); bb_line_dec(&line, "core0_clk_en", (words[0] >> 8) & 1);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU WRAPPER delay");
    bb_line_hex(&line, "up", words[1] & 0xffff); bb_line_hex(&line, "down", words[1] >> 16);
    bb_line_dec(&line, "cores", words[2] & 3);
    bb_line_hex(&line, "primary", words[3] & 15); bb_line_hex(&line, "secondary", (words[3] >> 4) & 15);
    bb_line_end(&line); bb_power_print(&line);
    if (result != 1) bb_power_fail("GPU wrapper read: CORE6 not ON, four wrapper words read");
    before();
    if (!a733_gpu_platform_off_begin(&gpu, bb_power.deadline)) gpu_fail("GPU OFF begin");
    while ((result = a733_gpu_platform_off_poll(&gpu)) == 0) gpu_pause("GPU OFF deadline");
    if (result != 1) gpu_fail("GPU OFF poll");
    off_proof();
    bb_power_park();
    off_proof();
    if (!atomic_load_explicit(&bb_power.parked, memory_order_acquire) ||
        !a733_power_service_idle(&bb_power.domains)) bb_power_fail("GPU provider park proof");
    before();
    platform_puts("COMPLETE GPU wrapper read: CORE status 8, four wrapper words, OFF and provider parked\n");
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
