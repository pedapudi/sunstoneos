/* SPDX-License-Identifier: MIT
 * The vendor-state variant of the GPU power diagnostic. It compiles main.c
 * as the power-only variant with its client and entry renamed, so main.c and
 * the power-only image stay unchanged, and adds its own client and entry.
 * Its build links the copies of this directory in place of the shared
 * units: the monitor vendor-state-monitor.c, which makes BL31's cold-boot
 * security writes at EL3 before EL1 starts; the PMIC provider, supply
 * consumer and resource manager vendor-state-pmic.c, -consumer.c and
 * -resources.c, which admit DCDC4 at 960 mV; and the controller and native
 * operations vendor-state-power.c and -native.c, which run the GPU core
 * from PLL_GPU0 at 1008 MHz.
 */
#define GPU_POWER_ONLY 1
#define kernel_main gpu_power_only_kernel_main
#define gpu_power_id_client gpu_power_only_client
#include "main.c"
#undef kernel_main
#undef gpu_power_id_client
#include "boot.h"
#include "vendor-state.h"

void kernel_main(void);
void gpu_power_id_client(void *unused);

/* The state the vendor's GPU probe reads the identity in, applied in its
 * order (docs/reference-backsolve-gpu.md):
 * 1. at EL3, before EL1 started, BL31's security writes; the client
 *    prints the monitor's EL3 record and requires the clock-controller
 *    security switch to read 7;
 * 2. DCDC4 from 0x1e (800 mV) to 0x2e (960 mV) through the provider's
 *    write path with its readback, then 1.2 ms for the output to settle;
 * 3. one ON: TOP5 adopted ON, CORE6 PWCR 0 and policy 8, then PLL_GPU0
 *    enabled, the GPU bus reset, bus gate and core clock on PLL_GPU0, and
 *    PLL_GPU0 at 1008 MHz; the CORE6 status poll is kept only as a 200 ms
 *    observation;
 * 4. the identity read as the vendor's probe reads it (rgxbvnc.c:119):
 *    OSReadHWReg64 is two 32-bit reads, +0x24 then +0x20 (osfunc.h:
 *    1086-1095).
 * There is no OFF: the lifetime ends with a power removal. Before each step
 * the progress record between the result header and the log holds the
 * step and a counter stamp, and before each identity read the read number
 * too, so a run that returns shows how far it went. A read that never
 * completes stops the CPU; then nothing returns and only a power removal
 * recovers the board. */
#define VST_SETTLE_US 1200u
#define VST_ON_US 200000u
#define VST_RESERVE_US 100000u
#define VST_MAGIC UINT64_C(0x3130545356555047) /* "GPUVST01" */
enum vst_stage { VST_START, VST_EL3, VST_LEASE, VST_WRITE, VST_SETTLE, VST_ON, VST_READ1, VST_READ2, VST_EXIT };
const uint32_t vendor_state_identity[2] = {0x01800024, 0x01800020};
struct vst_progress { uint64_t magic, stage, count, stamp; uint32_t value[2]; };
#define VST_PROGRESS ((volatile struct vst_progress *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct boot_result) == 128 && 128 + sizeof(struct vst_progress) <= LOG_OFFSET,
               "the progress record lies between the result header and the log");
static unsigned identity_done;

static void stamp(enum vst_stage stage)
{
    VST_PROGRESS->stage = stage;
    VST_PROGRESS->stamp = counter();
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Step 1: the monitor's record of BL31's security writes. */
static void el3_report(void)
{
    const volatile struct vendor_state_el3 *r = (const volatile struct vendor_state_el3 *)VENDOR_STATE_EL3_RECORD;
    struct bb_line line;
    const unsigned done = r->done < VENDOR_STATE_EL3_WRITES ? r->done : VENDOR_STATE_EL3_WRITES;
    uint32_t sw = 0;
    for (unsigned i = 0; i < done; ++i) {
        bb_line_start(&line, "GPU VST el3");
        bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", r->address[i]);
        bb_line_hex(&line, "entry", r->entry[i]); bb_line_hex(&line, "after", r->after[i]);
        bb_line_end(&line); bb_power_print(&line);
        if (r->address[i] == VENDOR_STATE_CCU_SWITCH) sw = r->after[i];
    }
    if (r->magic != VENDOR_STATE_EL3_MAGIC || r->done != VENDOR_STATE_EL3_WRITES)
        bb_power_fail("GPU vendor-state: EL3 security record incomplete");
    if (sw != 7) bb_power_fail("GPU vendor-state: CCU security switch did not read 7");
}

/* Step 2: DCDC4 to 960 mV under an exclusive token, through the provider's
 * write with its readback; the token is released before GPU admission,
 * whose supply consumer then holds DCDC4 shared. */
static uint64_t dcdc4_raise(void)
{
    stamp(VST_LEASE);
    struct a733_rail_token token;
    enum a733_rail_result r;
    while ((r = a733_rail_acquire(&bb_power.rails, BB_POWER_CLIENT_OWNER, 0, A733_RAIL_BIT(A733_RAIL_DCDC4),
                                  bb_power.deadline, &token)) == A733_RAIL_BUSY)
        bb_power_pause("GPU vendor-state DCDC4 lease deadline");
    if (r != A733_RAIL_PENDING && r != A733_RAIL_READY) bb_power_fail("GPU vendor-state DCDC4 lease");
    while ((r = a733_rail_poll(&bb_power.rails, &token)) != A733_RAIL_READY) {
        if (r != A733_RAIL_PENDING && r != A733_RAIL_BUSY) bb_power_fail("GPU vendor-state DCDC4 lease");
        bb_power_pause("GPU vendor-state DCDC4 lease deadline");
    }
    stamp(VST_WRITE);
    struct a733_pmic_request request = {0};
    enum a733_pmic_status s;
    while ((s = a733_pmic_submit_write(&bb_power.pmic, BB_POWER_CLIENT_OWNER, &token, A733_RAIL_DCDC4,
                                       VENDOR_STATE_DCDC4_ENTRY, VENDOR_STATE_DCDC4_TARGET, bb_power.deadline,
                                       &request)) == A733_PMIC_BUSY)
        bb_power_pause("GPU vendor-state DCDC4 write deadline");
    if (s != A733_PMIC_PENDING || !request.id) bb_power_fail("GPU vendor-state DCDC4 write refused");
    struct a733_pmic_snapshot snap = {0};
    while ((s = a733_pmic_result(&bb_power.pmic, &request, &snap)) == A733_PMIC_PENDING || s == A733_PMIC_BUSY)
        bb_power_pause("GPU vendor-state DCDC4 write deadline");
    const uint64_t written = bb_power_now();
    struct bb_line line;
    bb_line_start(&line, "GPU VST dcdc4");
    bb_line_dec(&line, "status", s);
    bb_line_hex(&line, "boot_selector", bb_power.domains.dcdc4.selector);
    bb_line_hex(&line, "selector", snap.selector); bb_line_hex(&line, "enable", snap.enable);
    bb_line_dec(&line, "uv", snap.programmed_uv);
    bb_line_end(&line); bb_power_print(&line);
    if (s != A733_PMIC_COMPLETE) bb_power_fail("GPU vendor-state DCDC4 write not complete");
    if (snap.rail != A733_RAIL_DCDC4 || snap.page_before || snap.chip != 3 || snap.revision || snap.page_after ||
        !snap.enabled || !snap.decoded || snap.programmed_uv != VENDOR_STATE_SUPPLY_UV ||
        (snap.selector & 127) != VENDOR_STATE_DCDC4_TARGET)
        bb_power_fail("GPU vendor-state DCDC4 readback not 960 mV");
    while ((s = a733_pmic_reap(&bb_power.pmic, &request)) == A733_PMIC_BUSY)
        bb_power_pause("GPU vendor-state DCDC4 reap deadline");
    if (s != A733_PMIC_COMPLETE) bb_power_fail("GPU vendor-state DCDC4 reap");
    while ((r = a733_rail_release(&bb_power.rails, &token, bb_power.deadline)) == A733_RAIL_BUSY)
        bb_power_pause("GPU vendor-state DCDC4 release deadline");
    if (r != A733_RAIL_READY) bb_power_fail("GPU vendor-state DCDC4 release");
    return written;
}

static uint32_t identity_read(unsigned i)
{
    if (i >= 2 || i != identity_done) bb_power_fail("GPU vendor-state read order");
    const uintptr_t address = vendor_state_identity[i];
    before();
    VST_PROGRESS->count = identity_done;
    stamp(i ? VST_READ2 : VST_READ1);
    struct bb_line line;
    bb_line_start(&line, "GPU VST reading");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", address);
    bb_line_end(&line); bb_power_print(&line);
    const uint32_t value = a733_mmio_read32_dsb_sy_both(address);
    VST_PROGRESS->value[i] = value;
    VST_PROGRESS->count = ++identity_done;
    before();
    bb_line_start(&line, "GPU VST");
    bb_line_dec(&line, "n", i + 1); bb_line_hex(&line, "a", address); bb_line_hex(&line, "v", value);
    bb_line_end(&line); bb_power_print(&line);
    return value;
}

void gpu_power_id_client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    VST_PROGRESS->magic = VST_MAGIC;
    VST_PROGRESS->count = 0;
    VST_PROGRESS->value[0] = VST_PROGRESS->value[1] = 0;
    stamp(VST_START);
    before();
    if (last_now + VST_SETTLE_US + VST_ON_US + VST_RESERVE_US > bb_power.deadline)
        bb_power_fail("GPU vendor-state time budget");
    stamp(VST_EL3);
    el3_report();
    const uint64_t written = dcdc4_raise();
    stamp(VST_SETTLE);
    /* At least 1.2 ms after the readback: the regulator's 1 ms ramp delay for
     * 160 mV at 250 uV/us and the vendor's 200 us (sunxi_platform.c:473-485). */
    while (bb_power_now() <= written + VST_SETTLE_US)
        if (bb_power_now() >= bb_power.deadline) bb_power_fail("GPU vendor-state settle deadline");
    before();
    snapshot(0);
    stamp(VST_ON);
    cycle = 1;
    before();
    const uint64_t start = last_now, on_deadline = last_now + VST_ON_US;
    if (on_deadline + VST_RESERVE_US > bb_power.deadline) bb_power_fail("GPU vendor-state time budget");
    if (!a733_gpu_platform_begin(&gpu, bb_power.deadline)) gpu_fail("GPU ON begin");
    int result;
    while ((result = a733_gpu_platform_poll(&gpu)) == 0) {
        if (bb_power_now() >= on_deadline) break;
        vTaskDelay(1);
    }
    before();
    snapshot(1);
    /* The controller, CORE6 and clock lines, then the CORE6 observation. */
    {
        const struct a733_gpu_power *p = &gpu.power;
        const struct vendor_state_clock *c = &vendor_state_clock;
        struct bb_line line;
        bb_line_start(&line, "GPU power");
        bb_line_dec(&line, "phase", p->phase); bb_line_dec(&line, "error", p->error);
        bb_line_dec(&line, "step", p->index); bb_line_hex(&line, "last", p->last_value);
        bb_line_end(&line); bb_power_print(&line);
        core_report();
        bb_line_start(&line, "GPU VST clock");
        bb_line_hex(&line, "pll", c->pll_last); bb_line_dec(&line, "pll_checks", c->pll_steps);
        bb_line_dec(&line, "lock1_reads", c->lock_reads[0]); bb_line_dec(&line, "lock1_run", c->lock_run[0]);
        bb_line_dec(&line, "lock2_reads", c->lock_reads[1]); bb_line_dec(&line, "lock2_run", c->lock_run[1]);
        bb_line_dec(&line, "update1_reads", c->update_reads[0]); bb_line_dec(&line, "update2_reads", c->update_reads[1]);
        bb_line_end(&line); bb_power_print(&line);
        bb_line_start(&line, "GPU VST core6");
        bb_line_dec(&line, "on", result == 1); bb_line_hex(&line, "status", p->last_value);
        bb_line_dec(&line, "waited_us", last_now - start);
        bb_line_end(&line); bb_power_print(&line);
    }
    if (result < 0) gpu_fail("GPU ON poll");
    /* Every PLL_GPU0 read-back passed and the PLL reads 1008 MHz, and the
     * MISR read after the clock steps precedes only the final status poll. */
    if ((vendor_state_clock.pll_last & ~(UINT32_C(1) << 28)) != VENDOR_STATE_PLL_1008 ||
        vendor_state_clock.pll_steps != 10 ||
        !(gpu.power.core_seen & (UINT32_C(8) << A733_GPU_NOTE_MISR_LATE)))
        gpu_fail("GPU vendor-state refused before the GPU clock reached 1008 MHz");
    static const struct a733_mapping_rule device = {A733_MAPPING_READ, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    before();
    if (!a733_mapping_check(aarch64_device_translate, GPU_BASE, GPU_BASE, &device))
        gpu_fail("GPU vendor-state mapping");
    for (unsigned i = 0; i < 2; ++i) (void)identity_read(i);
    const uint64_t bvnc = (uint64_t)VST_PROGRESS->value[0] << 32 | VST_PROGRESS->value[1];
    const int matches = bvnc == EXPECTED_BVNC;
    struct bb_line line;
    bb_line_start(&line, "GPU VST identity");
    bb_line_hex(&line, "raw", bvnc);
    bb_line_dec(&line, "b", bvnc >> 48); bb_line_dec(&line, "v", (bvnc >> 32) & 0xffff);
    bb_line_dec(&line, "n", (bvnc >> 16) & 0xffff); bb_line_dec(&line, "c", bvnc & 0xffff);
    bb_line_dec(&line, "matches", matches);
    bb_line_end(&line); bb_power_print(&line);
    before();
    stamp(VST_EXIT);
    /* No OFF: CORE6 never acknowledged ON, and the lifetime ends here. */
    if (!matches) bb_power_fail("GPU vendor-state: identity not 36.56.104.183, GPU left powered");
    platform_puts("COMPLETE GPU vendor-state: identity 36.56.104.183 at 1008 MHz from the GPU PLL, DCDC4 960 mV "
                  "and the BL31 security setup, GPU left powered\n");
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
        platform_panic("GPU vendor-state initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
