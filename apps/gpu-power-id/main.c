/* SPDX-License-Identifier: MIT
 * BVNC decoding follows the pinned Imagination pvr_device.c; see README.md.
 * Copyright (c) 2023 Imagination Technologies Ltd.
 */
#include "power.h"
#include "platform.h"
#include "a733_gpu_platform_native.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"

/* GPU_POWER_ONLY builds the power-only variant: TOP and CORE ON with CORE
 * status 8, then OFF and provider park, with no access to the GPU register
 * window at 0x01800000 (README.md). */
#define GPU_CLIENT 7u
#define GPU_BASE UINT32_C(0x01800000)
#define EXPECTED_BVNC (UINT64_C(36) << 48 | UINT64_C(56) << 32 | UINT64_C(104) << 16 | 183)

static struct a733_gpu_platform_native native;
static struct a733_gpu_platform gpu;
static uint64_t last_now;
#ifndef GPU_POWER_ONLY
static unsigned id_reads;
#endif

static _Noreturn void gpu_fail(const char *reason)
{
    /* Cached observations only: a refusal must not make another device read. */
    const struct a733_gpu_power *p = &gpu.power;
    struct bb_line line;
    bb_line_start(&line, "GPU power");
    bb_line_dec(&line, "phase", p->phase); bb_line_dec(&line, "error", p->error);
    bb_line_dec(&line, "step", p->index); bb_line_hex(&line, "last", p->last_value);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU CORE entry");
    bb_line_dec(&line, "seen", p->core_seen); bb_line_hex(&line, "policy", p->core_policy);
    bb_line_hex(&line, "status", p->core_status); bb_line_hex(&line, "pwcr", p->core_config);
    bb_line_end(&line); bb_power_print(&line);
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
/* Only the identity registers are accessible to this consumer. The actual
 * native translation and access width are checked before each ordered read. */
static uint64_t id_read(unsigned offset, unsigned width)
{
    static const struct a733_mapping_rule device = {A733_MAPPING_READ, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    if (gpu.phase != A733_GPU_PLATFORM_ON || !gpu.token || !gpu.delegated ||
        !((offset == 0x20 && (width == 8 || width == 4)) || (offset == 0x18 && width == 4)))
        bb_power_fail("GPU identity ownership/access");
    before();
    uintptr_t address = GPU_BASE + offset;
    if (!a733_mapping_check(aarch64_device_translate, address, address, &device))
        bb_power_fail("GPU identity mapping");
    before();
    uint64_t value = width == 8 ? a733_mmio_read64_dsb_sy_both(address) :
                                 a733_mmio_read32_dsb_sy_both(address);
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
    before();
    if (!a733_gpu_platform_begin(&gpu, bb_power.deadline)) gpu_fail("GPU ON begin");
    int result;
    while ((result = a733_gpu_platform_poll(&gpu)) == 0) gpu_pause("GPU ON deadline");
    if (result != 1) gpu_fail("GPU ON poll");

#ifdef GPU_POWER_ONLY
    /* The last controller read is the CORE status poll; ON requires 8. */
    const uint32_t core_status = gpu.power.last_value;
    if (gpu.phase != A733_GPU_PLATFORM_ON || gpu.power.phase != A733_GPU_POWER_ON || core_status != 8)
        gpu_fail("GPU CORE status");
#else
    uint64_t raw = id_read(0x20, 8), bvnc = raw;
    uint32_t revision = 0, core = 0;
    if (!(raw >> 48)) {
        revision = (uint32_t)id_read(0x20, 4);
        core = (uint32_t)id_read(0x18, 4);
        bvnc = (uint64_t)((revision >> 16) & 255) << 48 |
               (uint64_t)((revision >> 8) & 255) << 32 |
               (uint64_t)((core >> 8) & 255) << 16 | (core & 255);
    }
#endif

    /* No firmware was constructed or booted, and no GPU command/DMA backing
     * exists. The admitted clean initial state is required for this unwind. */
    before();
    if (!a733_gpu_platform_off_begin(&gpu, bb_power.deadline)) gpu_fail("GPU OFF begin");
    while ((result = a733_gpu_platform_off_poll(&gpu)) == 0) gpu_pause("GPU OFF deadline");
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

    struct bb_line line;
#ifdef GPU_POWER_ONLY
    bb_line_start(&line, "GPU power");
    bb_line_hex(&line, "core_status", core_status);
    bb_line_end(&line); bb_power_print(&line);
    platform_puts("COMPLETE GPU power only: CORE status 8, OFF and provider parked\n");
#else
    bb_line_start(&line, "GPU identity");
    bb_line_hex(&line, "raw", raw);
    bb_line_hex(&line, "bvnc", bvnc);
    bb_line_dec(&line, "matches", bvnc == EXPECTED_BVNC);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "GPU identity");
    bb_line_hex(&line, "revision", revision);
    bb_line_hex(&line, "core", core);
    bb_line_dec(&line, "reads", id_reads);
    bb_line_end(&line); bb_power_print(&line);
    platform_puts("COMPLETE GPU power identity: OFF and provider parked\n");
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
