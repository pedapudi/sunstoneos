/* SPDX-License-Identifier: MIT
 * The vendor-clk variant of the GPU power diagnostic. It compiles main.c as
 * the power-only variant with its client and entry renamed, so main.c and
 * the power-only image stay unchanged, and adds its own client and entry.
 * Its build links vendor-clk-power.c in place of drivers/gpu/a733_gpu_power.c:
 * the controller's ON releases the GPU bus reset before it opens the bus gate
 * and core clock, as the vendor does, and drops the inert CORE6 delay row.
 * Everything else matches the wrapper-read run that returned (set 14): the
 * same CORE6 PWCR 0 and policy 8 writes, 400 MHz from PERI0, DCDC4 at 800 mV.
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

/* One ON of the power-only sequence, with the CORE6 status poll kept only as
 * a 200 ms observation, then the GPU core identity read as the vendor's probe
 * reads it (rgxbvnc.c:119): OSReadHWReg64 is two 32-bit reads, +0x24 then
 * +0x20 in source order (osfunc.h:1086-1095). No wrapper, SMC0 or drm_ctrl
 * access, and no write. There is no OFF: the run ends after the reads and the
 * lifetime ends with a power removal. Before each read the progress record
 * between the result header and the log holds the read number and a counter
 * stamp, and the log names it, so a run that returns shows how far it went.
 * A read that never completes stops the CPU; then nothing returns and only a
 * power removal recovers the board. */
#define VCLK_ON_US 200000u
#define VCLK_RESERVE_US 100000u
#define VCLK_MAGIC UINT64_C(0x31304b4c43555047) /* "GPUCLK01" */
const uint32_t identity_words[2] = {0x01800024, 0x01800020};
struct vclk_progress { uint64_t magic, stage, count, stamp; uint32_t value[2]; };
#define VCLK_PROGRESS ((volatile struct vclk_progress *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct boot_result) == 128 && 128 + sizeof(struct vclk_progress) <= LOG_OFFSET,
               "the progress record lies between the result header and the log");
static unsigned identity_done;

static uint32_t identity_read(unsigned i)
{
    if (i >= 2 || i != identity_done) bb_power_fail("GPU vendor-clk read order");
    const uintptr_t address = identity_words[i];
    before();
    VCLK_PROGRESS->stage = i + 1;
    VCLK_PROGRESS->stamp = counter();
    __asm__ volatile("dsb sy" ::: "memory");
    struct bb_line line;
    bb_line_start(&line, "GPU VCLK reading");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", address);
    bb_line_end(&line); bb_power_print(&line);
    const uint32_t value = a733_mmio_read32_dsb_sy_both(address);
    VCLK_PROGRESS->value[i] = value;
    VCLK_PROGRESS->count = ++identity_done;
    before();
    bb_line_start(&line, "GPU VCLK");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", address); bb_line_hex(&line, "v", value);
    bb_line_end(&line); bb_power_print(&line);
    return value;
}

void gpu_power_id_client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    VCLK_PROGRESS->magic = VCLK_MAGIC;
    VCLK_PROGRESS->stage = VCLK_PROGRESS->count = 0;
    VCLK_PROGRESS->value[0] = VCLK_PROGRESS->value[1] = 0;
    VCLK_PROGRESS->stamp = counter();
    snapshot(0);
    cycle = 1;
    before();
    const uint64_t start = last_now, on_deadline = last_now + VCLK_ON_US;
    if (on_deadline + VCLK_RESERVE_US > bb_power.deadline) bb_power_fail("GPU vendor-clk time budget");
    if (!a733_gpu_platform_begin(&gpu, bb_power.deadline)) gpu_fail("GPU ON begin");
    int result;
    while ((result = a733_gpu_platform_poll(&gpu)) == 0) {
        if (bb_power_now() >= on_deadline) break;
        vTaskDelay(1);
    }
    before();
    snapshot(1);
    /* The controller and CORE6 lines, then the CORE6 observation. */
    {
        const struct a733_gpu_power *p = &gpu.power;
        struct bb_line line;
        bb_line_start(&line, "GPU power");
        bb_line_dec(&line, "phase", p->phase); bb_line_dec(&line, "error", p->error);
        bb_line_dec(&line, "step", p->index); bb_line_hex(&line, "last", p->last_value);
        bb_line_end(&line); bb_power_print(&line);
        core_report();
        bb_line_start(&line, "GPU VCLK core6");
        bb_line_dec(&line, "on", result == 1); bb_line_hex(&line, "status", p->last_value);
        bb_line_dec(&line, "waited_us", last_now - start);
        bb_line_end(&line); bb_power_print(&line);
    }
    if (result < 0) gpu_fail("GPU ON poll");
    /* The MISR read after the reset release precedes only the final status
     * poll, so the GPU clock is open and its reset released, as before the
     * vendor's probe-time read. */
    if (result == 0 && !(gpu.power.core_seen & (UINT32_C(8) << A733_GPU_NOTE_MISR_LATE)))
        gpu_fail("GPU vendor-clk refused before clock and reset progress");
    static const struct a733_mapping_rule device = {A733_MAPPING_READ, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    before();
    if (!a733_mapping_check(aarch64_device_translate, GPU_BASE, GPU_BASE, &device))
        gpu_fail("GPU vendor-clk mapping");
    for (unsigned i = 0; i < 2; ++i) (void)identity_read(i);
    const uint64_t bvnc = (uint64_t)VCLK_PROGRESS->value[0] << 32 | VCLK_PROGRESS->value[1];
    const int matches = bvnc == EXPECTED_BVNC;
    struct bb_line line;
    bb_line_start(&line, "GPU VCLK identity");
    bb_line_hex(&line, "raw", bvnc);
    bb_line_dec(&line, "b", bvnc >> 48); bb_line_dec(&line, "v", (bvnc >> 32) & 0xffff);
    bb_line_dec(&line, "n", (bvnc >> 16) & 0xffff); bb_line_dec(&line, "c", bvnc & 0xffff);
    bb_line_dec(&line, "matches", matches);
    bb_line_end(&line); bb_power_print(&line);
    before();
    /* No OFF: CORE6 never acknowledged ON, and the lifetime ends here. */
    if (!matches) bb_power_fail("GPU vendor-clk: identity not 36.56.104.183, GPU left powered");
    platform_puts("COMPLETE GPU vendor-clk: identity 36.56.104.183 with the vendor clock and reset order, "
                  "GPU left powered\n");
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
        platform_panic("GPU vendor-clk initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
