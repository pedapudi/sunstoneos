/* SPDX-License-Identifier: MIT
 * BVNC decoding follows the pinned Imagination pvr_device.c; see README.md.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 */
#include "power.h"
#include "platform.h"
#include "a733_gpu_platform_native.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"

/* Both variants run two cycles of the default GPU power path
 * (docs/designs/gpu-vendor-default.md): ON with CORE status 8, then OFF,
 * then provider park. The identity variant reads the GPU core identity in
 * each cycle as the vendor's probe does, two 32-bit loads. GPU_POWER_ONLY
 * builds the power-only variant, with no access to the GPU register window
 * at 0x01800000 (README.md). The second cycle shows that OFF returns the GPU
 * to a state from which ON succeeds again. */
#define GPU_CLIENT 7u
#define GPU_BASE UINT32_C(0x01800000)
#define EXPECTED_BVNC (UINT64_C(36) << 48 | UINT64_C(56) << 32 | UINT64_C(104) << 16 | 183)

static struct a733_gpu_platform_native native;
static struct a733_gpu_platform gpu;
static uint64_t last_now;
#define GPU_CYCLES 2u
static unsigned cycle;
#ifndef GPU_POWER_ONLY
static unsigned id_reads;
#endif

/* Read-only snapshot of the candidate CORE6 prerequisites: the CORE6 and
 * TOP5 PPU rows (DEN0051E registers and the vendor delay row), the R_CCU PPU
 * gate and VDD isolation gates, and the GPU PLL, clock and gate words of the
 * CCU. None is in the GPU register window. Snapshot 0 precedes the first ON;
 * snapshot n follows the ON attempt of cycle n, also after its failure. Each
 * page is checked as an identity Device-nGnRnE mapping before the reads. */
static const uint32_t snapshot_words[] = {
    0x07066000, 0x07066004, 0x07066008, 0x07066010, 0x07066014, 0x07066018, 0x07066020,
    0x07066024, 0x07066030, 0x07066034, 0x07066038, 0x0706603c, 0x07066160, 0x07066164,
    0x07066170, 0x07066174, 0x07066c00, 0x07066c04, 0x07066c10, 0x07066fb4,
    0x07065000, 0x07065008, 0x07065010, 0x07065014, 0x07065020, 0x07065170, 0x07065174,
    0x07065c00, 0x07065c04, 0x07065c10,
    0x070101ac, 0x07010250, 0x020020e0, 0x02002b20, 0x02002b24};
static void snapshot(unsigned which)
{
    static const struct a733_mapping_rule device = {A733_MAPPING_READ, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    static const uintptr_t pages[] = {0x07066000, 0x07065000, 0x07010000, 0x02002000};
    for (unsigned i = 0; i < sizeof(pages) / sizeof(*pages); ++i)
        if (!a733_mapping_check(aarch64_device_translate, pages[i], pages[i], &device))
            bb_power_fail("GPU snapshot mapping");
    for (unsigned i = 0; i < sizeof(snapshot_words) / sizeof(*snapshot_words); ++i) {
        struct bb_line line;
        bb_line_start(&line, "GPU SNAP");
        bb_line_dec(&line, "n", which);
        bb_line_hex(&line, "a", snapshot_words[i]);
        bb_line_hex(&line, "v", a733_mmio_read32_dsb_sy_both(snapshot_words[i]));
        bb_line_end(&line); bb_power_print(&line);
    }
}

/* The controller's cached observations of the last ON, with its cycle number
 * from 1: CORE6 entry words, identification and accept/deny inputs, the
 * status after the PWCR and policy writes, and the PLL_GPU0 lock and GPU
 * clock update reads with the last PLL_GPU0 word. */
static void gates_report(void);
static void core_report(void)
{
    const struct a733_gpu_power *p = &gpu.power;
    struct bb_line line;
    bb_line_start(&line, "GPU CORE entry");
    bb_line_dec(&line, "cycle", cycle);
    bb_line_dec(&line, "seen", p->core_seen); bb_line_hex(&line, "policy", p->core_policy);
    bb_line_hex(&line, "status", p->core_status); bb_line_hex(&line, "pwcr", p->core_config);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU CORE inputs");
    bb_line_dec(&line, "cycle", cycle);
    bb_line_hex(&line, "idr0", p->core_note[A733_GPU_NOTE_IDR0]);
    bb_line_hex(&line, "misr", p->core_note[A733_GPU_NOTE_MISR]);
    bb_line_hex(&line, "misr_late", p->core_note[A733_GPU_NOTE_MISR_LATE]);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU CORE after");
    bb_line_dec(&line, "cycle", cycle);
    bb_line_hex(&line, "pwcr_status", p->core_note[A733_GPU_NOTE_AFTER_PWCR]);
    bb_line_hex(&line, "policy_status", p->core_note[A733_GPU_NOTE_AFTER_POLICY]);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU clock");
    bb_line_dec(&line, "cycle", cycle);
    bb_line_hex(&line, "pll", p->pll_word);
    bb_line_dec(&line, "lock1_reads", p->pll_lock_reads[0]); bb_line_dec(&line, "lock2_reads", p->pll_lock_reads[1]);
    bb_line_dec(&line, "update1_reads", p->clock_update_reads[0]);
    bb_line_dec(&line, "update2_reads", p->clock_update_reads[1]);
    bb_line_end(&line); bb_power_print(&line);
    gates_report();
}
/* The GPU AHB and MBUS master gate words (CCU 0x5c0 and 0x5e0) that the
 * controller recorded: at the last ON's entry and after its TOP5 ON, and
 * after the last OFF's TOP5 OFF and at its end. */
static void gates_report(void)
{
    const uint32_t *n = gpu.power.core_note;
    struct bb_line line;
    bb_line_start(&line, "GPU gates");
    bb_line_dec(&line, "cycle", cycle);
    bb_line_hex(&line, "ahb_entry", n[A733_GPU_NOTE_AHB_ENTRY]);
    bb_line_hex(&line, "mbus_entry", n[A733_GPU_NOTE_MBUS_ENTRY]);
    bb_line_hex(&line, "ahb_on", n[A733_GPU_NOTE_AHB_TOP5_ON]);
    bb_line_hex(&line, "mbus_on", n[A733_GPU_NOTE_MBUS_TOP5_ON]);
    bb_line_hex(&line, "ahb_top5_off", n[A733_GPU_NOTE_AHB_TOP5_OFF]);
    bb_line_hex(&line, "mbus_top5_off", n[A733_GPU_NOTE_MBUS_TOP5_OFF]);
    bb_line_hex(&line, "ahb_off", n[A733_GPU_NOTE_AHB_OFF_END]);
    bb_line_hex(&line, "mbus_off", n[A733_GPU_NOTE_MBUS_OFF_END]);
    bb_line_end(&line); bb_power_print(&line);
}
static _Noreturn void gpu_fail(const char *reason)
{
    /* Cached observations only: a refusal must not make another device read. */
    const struct a733_gpu_power *p = &gpu.power;
    struct bb_line line;
    bb_line_start(&line, "GPU power");
    bb_line_dec(&line, "phase", p->phase); bb_line_dec(&line, "error", p->error);
    bb_line_dec(&line, "step", p->index); bb_line_hex(&line, "last", p->last_value);
    bb_line_end(&line); bb_power_print(&line);
    /* A release that finds CORE6 policy or status nonzero after TOP5 OFF
     * fails here with error LIVE and the word it read. */
    bb_line_start(&line, "GPU resources");
    bb_line_dec(&line, "result", gpu.resource_result); bb_line_dec(&line, "error", bb_power.gpu.error);
    bb_line_dec(&line, "stage", bb_power.gpu.stage); bb_line_hex(&line, "last", bb_power.gpu.last_value);
    bb_line_end(&line); bb_power_print(&line);
    core_report();
    bb_power_fail(reason);
}
static void gpu_pause(const char *reason)
{
    if (bb_power_now() >= bb_power.deadline) gpu_fail(reason);
    vTaskDelay(1);
}

static void before(void)
{
    uint64_t now = bb_power_now();
    if (now < last_now || now < gpu.last_now || now < gpu.power.last_now || now >= bb_power.deadline)
        bb_power_fail("GPU identity deadline/clock");
    last_now = now;
}

#ifndef GPU_POWER_ONLY
/* Only the identity registers are accessible to this consumer:
 * RGX_CR_CORE_ID__PBVNC, read as the vendor's probe reads it (rgxbvnc.c:119;
 * OSReadHWReg64 is two 32-bit loads, +0x24 then +0x20, osfunc.h:1086-1095).
 * The actual native translation is checked before each ordered read. */
static uint32_t id_read(unsigned offset)
{
    static const struct a733_mapping_rule device = {A733_MAPPING_READ, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    if (gpu.phase != A733_GPU_PLATFORM_ON || !gpu.token || !gpu.delegated || (offset != 0x24 && offset != 0x20))
        bb_power_fail("GPU identity ownership/access");
    before();
    uintptr_t address = GPU_BASE + offset;
    if (!a733_mapping_check(aarch64_device_translate, address, address, &device))
        bb_power_fail("GPU identity mapping");
    before();
    const uint32_t value = a733_mmio_read32_dsb_sy_both(address);
    ++id_reads;
    before();
    return value;
}
#endif

static void off_proof(void)
{
    const struct a733_power_domain_row *row = &gpu.power.row;
    if (gpu.phase != A733_GPU_PLATFORM_OFF || gpu.token || gpu.delegated ||
        gpu.power.phase != A733_GPU_POWER_OFF || gpu.power.lease_held ||
        row->domains != &bb_power.domains || row->domain != 5 || row->counted ||
        a733_power_domain_state(row) != A733_POWER_ROW_OFF ||
        bb_power.domains.failed || bb_power.domains.domain[2].id != 5 ||
        bb_power.domains.domain[2].state != A733_POWER_DOMAIN_OFF || bb_power.domains.domain[2].count ||
        bb_power.gpu.failed || bb_power.gpu.leases[GPU_CLIENT].token ||
        bb_power.gpu.leases[GPU_CLIENT].mode != A733_GPU_RESOURCE_FREE ||
        !gpu.pmic.rail.id || gpu.pmic.failed)
        bb_power_fail("GPU OFF ownership proof");
}

void gpu_power_id_client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    int result;
#ifdef GPU_POWER_ONLY
    uint32_t core_status[GPU_CYCLES] = {0};
#else
    uint64_t bvnc[GPU_CYCLES] = {0};
#endif
    snapshot(0);
    for (cycle = 1; cycle <= GPU_CYCLES; ++cycle) {
        before();
        if (!a733_gpu_platform_begin(&gpu, bb_power.deadline)) gpu_fail("GPU ON begin");
        /* An expired ON still takes its snapshot before the failure report. */
        while ((result = a733_gpu_platform_poll(&gpu)) == 0) {
            if (bb_power_now() >= bb_power.deadline) { result = -2; break; }
            vTaskDelay(1);
        }
        snapshot(cycle);
        if (result == -2) gpu_fail("GPU ON deadline");
        if (result != 1) gpu_fail("GPU ON poll");
        core_report();
#ifdef GPU_POWER_ONLY
        /* The controller's last ON step polls CORE status until it reads 8,
         * so a successful poll leaves that value as its last read. */
        core_status[cycle - 1] = gpu.power.last_value;
#else
        bvnc[cycle - 1] = (uint64_t)id_read(0x24) << 32;
        bvnc[cycle - 1] |= id_read(0x20);
#endif

        /* No firmware was constructed or booted, and no GPU command/DMA backing
         * exists. The admitted clean initial state is required for this unwind. */
        before();
        if (!a733_gpu_platform_off_begin(&gpu, bb_power.deadline)) gpu_fail("GPU OFF begin");
        while ((result = a733_gpu_platform_off_poll(&gpu)) == 0) gpu_pause("GPU OFF deadline");
        if (result != 1) gpu_fail("GPU OFF poll");
        gates_report();
        off_proof();
    }
    cycle = GPU_CYCLES;
    bb_power_park();
    off_proof();
    if (!atomic_load_explicit(&bb_power.parked, memory_order_acquire) ||
        !a733_power_service_idle(&bb_power.domains)) bb_power_fail("GPU provider park proof");
    for (unsigned i = 0; i < A733_PMIC_REQUESTS; ++i)
        if (bb_power.pmic.slots[i].state || bb_power.pmic.slots[i].borrow.id)
            bb_power_fail("GPU provider request retained");
    before();

    struct bb_line line;
#ifdef GPU_POWER_ONLY
    for (unsigned i = 0; i < GPU_CYCLES; ++i) {
        bb_line_start(&line, "GPU power");
        bb_line_dec(&line, "cycle", i + 1);
        bb_line_hex(&line, "core_status", core_status[i]);
        bb_line_end(&line); bb_power_print(&line);
    }
    platform_puts("COMPLETE GPU power only: CORE status 8, OFF and provider parked\n");
#else
    unsigned matches = 0;
    for (unsigned i = 0; i < GPU_CYCLES; ++i) {
        bb_line_start(&line, "GPU identity");
        bb_line_dec(&line, "cycle", i + 1);
        bb_line_hex(&line, "raw", bvnc[i]);
        bb_line_dec(&line, "b", bvnc[i] >> 48); bb_line_dec(&line, "v", (bvnc[i] >> 32) & 0xffff);
        bb_line_dec(&line, "n", (bvnc[i] >> 16) & 0xffff); bb_line_dec(&line, "c", bvnc[i] & 0xffff);
        bb_line_dec(&line, "matches", bvnc[i] == EXPECTED_BVNC);
        bb_line_end(&line); bb_power_print(&line);
        matches += bvnc[i] == EXPECTED_BVNC;
    }
    bb_line_start(&line, "GPU identity");
    bb_line_dec(&line, "reads", id_reads);
    bb_line_end(&line); bb_power_print(&line);
    if (matches != GPU_CYCLES) bb_power_fail("GPU identity not 36.56.104.183");
    platform_puts("COMPLETE GPU power identity: 36.56.104.183 in two ON/OFF cycles, provider parked\n");
#endif
    platform_exit(0);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{ (void)handle; platform_panic(name, 0); }

void kernel_main(void)
{
    platform_init();
    struct a733_pmic_lifetime life;
    if (!bb_power_snapshot_init(gpu_power_id_client, "gpu-id") ||
        !a733_gpu_platform_native_init(&native, &bb_power.locks.callbacks) ||
        !a733_gpu_platform_init(&gpu, &bb_power.gpu, GPU_CLIENT, &native.operations, &native) ||
        a733_pmic_identity(&bb_power.pmic, &life) != A733_PMIC_READY ||
        !a733_gpu_platform_bind_pmic(&gpu, &bb_power.gpu_native, &bb_power.pmic, &life, BB_POWER_CLIENT_OWNER))
        platform_panic("GPU identity initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
