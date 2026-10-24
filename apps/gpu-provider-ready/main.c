/* SPDX-License-Identifier: Apache-2.0 */
#include "power.h"
#include "platform.h"
#include "a733_gpu_platform_native.h"

static struct a733_gpu_platform_native native;
static struct a733_gpu_platform gpu;

static void unused_gpu(void)
{
    const struct a733_power_domain_row *row = &gpu.power.row;
    if (gpu.phase != A733_GPU_PLATFORM_NEW || gpu.token || gpu.delegated ||
        gpu.power.phase != A733_GPU_POWER_NEW || gpu.power.lease_attempted || gpu.power.lease_held ||
        gpu.pmic.stage || gpu.pmic.failed || gpu.pmic.armed || gpu.pmic.rail.id || gpu.pmic.request.id ||
        bb_power.domains.rows != row || row->next || row->domains != &bb_power.domains || row->domain != 5 ||
        row->counted || atomic_load_explicit(&row->request, memory_order_acquire) ||
        atomic_load_explicit(&row->state, memory_order_acquire) != A733_POWER_ROW_IDLE ||
        bb_power.domains.failed || bb_power.accessed || bb_power.writes ||
        bb_power.gpu.failed || bb_power.gpu.next_token != 1)
        bb_power_fail("GPU readiness ownership");
    for (unsigned i = 0; i < A733_POWER_DOMAINS; ++i)
        if (bb_power.domains.domain[i].state != A733_POWER_DOMAIN_UNKNOWN || bb_power.domains.domain[i].count)
            bb_power_fail("GPU readiness domain used");
    for (unsigned i = 0; i < A733_GPU_RESOURCE_SLOTS; ++i)
        if (bb_power.gpu.leases[i].mode != A733_GPU_RESOURCE_FREE || bb_power.gpu.leases[i].token)
            bb_power_fail("GPU readiness resource used");
}

static unsigned verified(void)
{
    const unsigned required = A733_POWER_PROVIDER_READY | A733_POWER_SUPPLY_DONE |
        A733_POWER_DCDC2_VERIFIED | A733_POWER_DCDC4_VERIFIED;
    const unsigned faults = A733_POWER_PMIC_TRANSFER | A733_POWER_DOMAIN1_FAULT |
        A733_POWER_DOMAIN4_FAULT | A733_POWER_DOMAIN5_FAULT;
    unsigned value = a733_power_authority_load(&bb_power.rails.authority);
    if ((value & (required | faults)) != required)
        bb_power_fail("GPU readiness supplies unverified");
    return value;
}

static void empty_requests(void)
{
    const struct a733_rail_leases *rails = &bb_power.rails;
    if (rails->free_slots != UINT32_MAX || rails->shared_held || rails->exclusive_held ||
        atomic_load_explicit(&rails->busy, memory_order_acquire) ||
        bb_power.domains.supply_request.id ||
        atomic_load_explicit(&bb_power.pmic.queued, memory_order_acquire))
        bb_power_fail("GPU readiness rail accounting");
    for (unsigned i = 0; i < A733_RAIL_CLIENTS; ++i)
        if (rails->slots[i].id || rails->pins[i]) bb_power_fail("GPU readiness rail retained");
    for (unsigned i = 0; i < A733_RAIL_BORROWS; ++i)
        if (rails->borrows[i].id) bb_power_fail("GPU readiness borrow retained");
    for (unsigned i = 0; i < A733_RAIL_COUNT; ++i)
        if (rails->sharers[i]) bb_power_fail("GPU readiness sharer retained");
    for (unsigned i = 0; i < A733_PMIC_REQUESTS; ++i)
        if (bb_power.pmic.slots[i].state || bb_power.pmic.slots[i].borrow.id)
            bb_power_fail("GPU readiness request retained");
}

void gpu_provider_ready_client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    uint64_t before = bb_power_now();
    if (before >= bb_power.deadline) bb_power_fail("GPU readiness deadline");
    (void)verified();
    unused_gpu();
    bb_power_park();
    if (!atomic_load_explicit(&bb_power.parked, memory_order_acquire) ||
        !a733_power_service_idle(&bb_power.domains)) bb_power_fail("GPU readiness provider not parked");
    unsigned authority = verified();
    unused_gpu();
    empty_requests();
    uint64_t after = bb_power_now();
    if (after < before || after >= bb_power.deadline) bb_power_fail("GPU readiness deadline");
    struct bb_line line;
    bb_line_start(&line, "GPU provider readiness");
    bb_line_hex(&line, "authority", authority);
    bb_line_dec(&line, "domain_accesses", bb_power.accessed);
    bb_line_dec(&line, "power_requests", atomic_load_explicit(&gpu.power.row.request, memory_order_relaxed));
    bb_line_end(&line);
    bb_power_print(&line);
    platform_puts("COMPLETE GPU provider readiness: supplies verified, parked, no GPU power request\n");
    platform_exit(0);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{ (void)handle; platform_panic(name, 0); }

/* Same constructor chain as the power/identity diagnostic. */
void kernel_main(void)
{
    platform_init();
    struct a733_pmic_lifetime life;
    if (!bb_power_snapshot_init(gpu_provider_ready_client, "gpu-ready") ||
        !a733_gpu_platform_native_init(&native, &bb_power.locks.callbacks) ||
        !a733_gpu_platform_init(&gpu, &bb_power.gpu, 7, &native.operations, &native) ||
        a733_pmic_identity(&bb_power.pmic, &life) != A733_PMIC_READY ||
        !a733_gpu_platform_bind_pmic(&gpu, &bb_power.gpu_native, &bb_power.pmic, &life, BB_POWER_CLIENT_OWNER))
        platform_panic("GPU readiness initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
