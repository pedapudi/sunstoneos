/* SPDX-License-Identifier: Apache-2.0 */
/* The GPU platform's DCDC4 raise to the GPU operating point (960 mV) before
 * its first ON (docs/designs/gpu-vendor-default.md), with the actual GPU
 * platform, supply consumer, PMIC provider, rail ledger and power-domain
 * owner of checks/power-domains-model.h. The native resource adapter's two
 * binding calls are replaced by stand-ins that set the same fields, since
 * that adapter is AArch64 code. Each case runs the platform's admission
 * until the supply consumer has sampled DCDC4, or until the raise fails:
 * - DCDC4 at the power-on 800 mV is written to 960 mV with bit 7 kept, the
 *   exclusive lease is released and the consumer samples only after 1.2 ms;
 * - DCDC4 already at 960 mV is not written and the consumer samples at once;
 * - a write submit that meets a busy provider is retried, not failed;
 * - DCDC4 at another voltage fails the platform with the snapshot reaped,
 *   the lease released and no write;
 * - after an owner's boot check that refused DCDC4, the platform fails
 *   before it takes a lease.
 * Build: clang-21 -std=c11 -O1 -g -Wall -Wextra -Werror
 *   -fsanitize=address,undefined -Iinclude -Ichecks
 *   checks/a733-gpu-supply-raise.c drivers/power/a733_pmic.c
 *   drivers/power/a733_rail_leases.c drivers/clock/a733_clock_leases.c
 *   drivers/gpu/a733_gpu_resources.c drivers/pinctrl/a733_pins.c
 *   drivers/pinctrl/a733_pin_tables.c drivers/power/a733_power_domains.c
 *   drivers/power/a733_pmic_consumer.c drivers/gpu/a733_gpu_power.c
 *   drivers/gpu/a733_gpu_platform.c */
#include "power-domains-model.h"
#include "a733_gpu_platform.h"
#include "a733_gpu_resources_native.h"

int a733_gpu_resources_native_use_pmic(struct a733_gpu_resource_native *n,
    struct a733_gpu_resource_manager *manager, struct a733_pmic *p)
{
    if (n->pmic) return n->pmic == p && n->pmic_manager == manager;
    n->pmic = p; n->pmic_manager = manager; return 1;
}
int a733_gpu_resources_native_bind_view(struct a733_gpu_resource_native *n, struct a733_pmic_consumer *c,
    unsigned client)
{
    if (n->pmic_view || c->config.provider != n->pmic) return 0;
    c->recipient = n; c->destination = &n->pmic_manager->rail; n->pmic_view = c; n->pmic_client = client;
    return 1;
}

static struct a733_gpu_platform gpu;
static struct a733_gpu_resource_native native;
static int gpu_now(void *v, uint64_t *out) { (void)v; return pmic_model_now(&m, out); }
static int gpu_read(void *v, enum a733_gpu_power_bank b, uint32_t off, uint32_t *out)
{ (void)v; (void)b; (void)off; (void)out; assert(0); return 0; }
static int gpu_update(void *v, enum a733_gpu_power_bank b, uint32_t off, uint32_t c, uint32_t s, uint64_t d)
{ (void)v; (void)b; (void)off; (void)c; (void)s; (void)d; assert(0); return 0; }

/* The resource acquisition that follows the consumer's sample ends there:
 * its first register read fails, which ends the case. */
static int no_resources(void *v, uintptr_t a, uint32_t *out) { (void)v; (void)a; (void)out; return 0; }
/* Admission polls, one service step and one tick apart, until the consumer
 * has sampled DCDC4 or the platform fails. busy_submit holds the provider
 * busy for one poll once the raise reaches its write submit. */
static int admit(unsigned dcdc4, unsigned busy_submit, uint64_t *sampled_at, uint64_t *written_at)
{
    domains_model_setup(0x28, 23);
    m.pmic[0x15] = (uint8_t)(0x80 | dcdc4);
    memset(&gpu, 0, sizeof(gpu)); memset(&native, 0, sizeof(native));
    native.initialized = 1;
    m.gpu.ops.read32 = no_resources;
    static const struct a733_gpu_platform_ops ops = {gpu_now, gpu_read, gpu_update};
    assert(a733_gpu_platform_init(&gpu, &m.gpu, 3, &ops, 0));
    assert(a733_gpu_platform_bind_pmic(&gpu, &native, &m.provider, &m.lifetime, 9));
    assert(a733_gpu_platform_begin(&gpu, m.now + 1000000));
    *sampled_at = *written_at = 0;
    unsigned busy = 0;
    for (unsigned i = 0; i < 200; ++i) {
        if (busy_submit && !busy && gpu.supply_stage == 4) { atomic_store(&m.provider.busy, 1); busy = 1; }
        const int r = a733_gpu_platform_poll(&gpu);
        if (busy == 1) {
            assert(r == 0 && gpu.supply_stage == 4 && gpu.phase != A733_GPU_PLATFORM_FAILED);
            atomic_store(&m.provider.busy, 0); busy = 2;
        }
        if (gpu.supply_written && !*written_at) *written_at = m.now;
        if (gpu.pmic_observed) { *sampled_at = m.now; return 1; }
        if (r < 0) return -1;
        service_step(); m.now += TICK_US;
    }
    assert(0); return 0;
}

int main(void)
{
    uint64_t sampled, written;
    /* 800 mV: one write to 960 mV, bit 7 kept, sample after the settle. */
    assert(admit(0x1e, 0, &sampled, &written) == 1);
    assert(m.pmic[0x15] == (0x80 | A733_GPU_DCDC4_SELECTOR) && gpu.supply_written && written);
    assert(sampled > written + A733_GPU_SUPPLY_SETTLE_US && sampled > gpu.supply_settled);
    assert(!gpu.supply_token.id && !gpu.supply_request.id && gpu.pmic.rail.id && gpu.pmic.request.id);
    assert(gpu.pmic.snapshot.programmed_uv == A733_GPU_SUPPLY_UV);
    const uint64_t raise_us = sampled;
    /* 960 mV left by an earlier image: no write and no settle. */
    assert(admit(A733_GPU_DCDC4_SELECTOR, 0, &sampled, &written) == 1);
    assert(m.pmic[0x15] == (0x80 | A733_GPU_DCDC4_SELECTOR) && !gpu.supply_written && !written);
    assert(gpu.pmic.snapshot.programmed_uv == A733_GPU_SUPPLY_UV && sampled + A733_GPU_SUPPLY_SETTLE_US <= raise_us);
    /* A busy provider at the write submit is retried. */
    assert(admit(0x1e, 1, &sampled, &written) == 1);
    assert(m.pmic[0x15] == (0x80 | A733_GPU_DCDC4_SELECTOR) && gpu.supply_written);
    /* 860 mV: refused without a write; the snapshot and the lease end. */
    assert(admit(0x24, 0, &sampled, &written) == -1);
    assert(gpu.phase == A733_GPU_PLATFORM_FAILED && m.pmic[0x15] == (0x80 | 0x24) && !gpu.supply_written);
    assert(!gpu.supply_request.id && !gpu.supply_token.id && !gpu.pmic.rail.id);
    for (unsigned i = 0; i < A733_PMIC_REQUESTS; ++i) assert(!m.provider.slots[i].state);
    /* DCDC4 at 900 mV with a domain-5 row: the owner's boot check refuses
     * DCDC4, and the platform fails without a lease or a write. */
    domains_model_setup(0x28, 23); m.pmic[0x15] = 0x80 | 0x28;
    memset(&gpu, 0, sizeof(gpu)); memset(&native, 0, sizeof(native)); native.initialized = 1;
    static const struct a733_gpu_platform_ops refused_ops = {gpu_now, gpu_read, gpu_update};
    assert(a733_gpu_platform_init(&gpu, &m.gpu, 3, &refused_ops, 0));
    assert(a733_gpu_platform_bind_pmic(&gpu, &native, &m.provider, &m.lifetime, 9));
    domains_boot();
    assert(!(a733_power_authority_load(&m.rails.authority) & A733_POWER_DCDC4_VERIFIED));
    assert(a733_gpu_platform_begin(&gpu, m.now + 1000000) && a733_gpu_platform_poll(&gpu) == -1);
    assert(!gpu.supply_token.id && !gpu.supply_request.id && m.pmic[0x15] == (0x80 | 0x28));
    printf("PASS GPU DCDC4 raise: 800 mV written to 960 mV once with bit 7 kept and the consumer sampling "
           "after %u us of settling, 960 mV not written, a busy write submit retried, 860 mV refused with "
           "its snapshot reaped and the lease released, a refused boot check failing before any lease\n", A733_GPU_SUPPLY_SETTLE_US);
    return 0;
}
