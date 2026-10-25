/* SPDX-License-Identifier: Apache-2.0
 * Unit harness of the vendor-state diagnostic's own copies, compiled for
 * AArch64 and run under Unicorn by vendor-state-units.py, which models the
 * registers. It links the copies themselves, so each case runs the code
 * that the vendor-state image links.
 *
 * vsu_power: the controller (vendor-state-power.c) on the native operations
 * (vendor-state-native.c), with the resources and the domain-5 row answered
 * here as vsu_config names. It polls until the controller leaves STARTING
 * or vsu_config.polls polls have run.
 * vsu_poll_again: one more poll of that controller.
 * vsu_native: the native allowlist's refusals, one result bit each.
 * vsu_resources: one LOCAL acquisition of the resource manager copy
 * (vendor-state-resources.c), its registers read from the modeled pages
 * and its DCDC4 snapshot from vsu_rail.
 * vendor-state-units-supply.c holds the PMIC provider and consumer cases.
 */
#include "a733_gpu_platform_native.h"
#include "vendor-state.h"
#include <stddef.h>

void *memset(void *p, int v, size_t n)
{ volatile unsigned char *q = p; while (n--) *q++ = (unsigned char)v; return p; }
void *memcpy(void *d, const void *s, size_t n)
{ volatile unsigned char *q = d; const unsigned char *r = s; while (n--) *q++ = *r++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    volatile unsigned char *q = d; const unsigned char *r = s;
    if (q < r) while (n--) *q++ = *r++;
    else while (n--) q[n] = r[n];
    return d;
}

/* Written by the script before each call. */
struct vsu_config {
    uint32_t parent_hz, supply_uv, row_answer, polls;
    uint64_t deadline_us;
};
volatile struct vsu_config vsu_config;
/* Read by the script after each call. */
struct vsu_result { uint32_t result, phase, error, index, polls, row_requests, locks_held; };
volatile struct vsu_result vsu_result;

static struct a733_gpu_platform_native native;
static struct a733_gpu_power power;
static unsigned register_held;
static int lock_enter(void *c, uint64_t deadline) { (void)c; (void)deadline; ++register_held; return 1; }
static void lock_leave(void *c) { (void)c; --register_held; }
static int now(void *c, uint64_t *out) { (void)c; return a733_gpu_platform_native_now(&native, out); }
static int acquire(void *c, struct a733_gpu_power_resources *out)
{ (void)c; out->parent_hz = vsu_config.parent_hz; out->supply_uv = vsu_config.supply_uv; return 1; }
static int release(void *c) { (void)c; return 1; }
static int read_reg(void *c, enum a733_gpu_power_bank bank, uint32_t off, uint32_t *out)
{ (void)c; return native.operations.read(&native, bank, off, out); }
static int update_reg(void *c, enum a733_gpu_power_bank bank, uint32_t off, uint32_t clear, uint32_t set)
{ (void)c; return native.operations.update(&native, bank, off, clear, set, power.deadline); }
/* The power-domain owner's answer to the domain-5 row: immediate. */
static void row_notify(void *c)
{
    (void)c;
    ++vsu_result.row_requests;
    const unsigned g = (atomic_load(&power.row.request) >> 1) & A733_POWER_ROW_GENERATION_MASK;
    atomic_store(&power.row.state, g << 3 | vsu_config.row_answer);
}
static struct a733_power_domains *const fake_domains = (struct a733_power_domains *)0x40180000;

static int setup(void)
{
    memset(&native, 0, sizeof native);
    memset(&power, 0, sizeof power);
    memset(&vendor_state_clock, 0, sizeof vendor_state_clock);
    register_held = 0;
    const struct a733_gpu_resource_locks locks = {lock_enter, lock_leave, lock_enter, lock_leave, 0};
    const struct a733_gpu_power_ops ops = {now, acquire, release, read_reg, update_reg};
    if (!a733_gpu_platform_native_init(&native, &locks) || !a733_gpu_power_init(&power, &ops, 0)) return 0;
    power.row = (struct a733_power_domain_row){.domains = fake_domains, .notify = row_notify, .domain = 5};
    return 1;
}

uint32_t vsu_power(void)
{
    memset((void *)&vsu_result, 0, sizeof vsu_result);
    if (!setup()) return 1;
    uint64_t start;
    now(0, &start);
    enum a733_gpu_power_result r = a733_gpu_power_on(&power, start + vsu_config.deadline_us);
    while (r == A733_GPU_POWER_AGAIN && vsu_result.polls < vsu_config.polls) {
        ++vsu_result.polls;
        r = a733_gpu_power_poll(&power);
    }
    vsu_result.result = r;
    vsu_result.phase = power.phase;
    vsu_result.error = power.error;
    vsu_result.index = power.index;
    vsu_result.locks_held = register_held;
    return 0;
}

/* One more poll after vsu_power: a failed controller makes no access. */
uint32_t vsu_poll_again(void) { return a733_gpu_power_poll(&power); }

/* Bit n set: refusal n happened. */
uint32_t vsu_native(void)
{
    if (!setup()) return 0;
    uint32_t v, bits = 0;
    const struct a733_gpu_platform_ops *o = &native.operations;
    bits |= (uint32_t)!o->update(&native, A733_GPU_CCU, 0x0e0, 0, 0x10000000, ~UINT64_C(0)) << 0;
    bits |= (uint32_t)!o->update(&native, A733_GPU_CCU, 0x0e0, UINT32_MAX, 0xe9105300, ~UINT64_C(0)) << 1;
    bits |= (uint32_t)!o->update(&native, A733_GPU_CCU, 0x0e0, 0x0070ff00, 0x00105400, ~UINT64_C(0)) << 2;
    bits |= (uint32_t)!o->update(&native, A733_GPU_CCU, 0x0e0, 0x80000000, 0, ~UINT64_C(0)) << 3;
    bits |= (uint32_t)!o->update(&native, A733_GPU_CCU, 0x0e4, 0, 0x08000000, ~UINT64_C(0)) << 4;
    bits |= (uint32_t)!o->read(&native, A733_GPU_CCU, 0x0e4, &v) << 5;
    bits |= (uint32_t)!o->update(&native, A733_GPU_CCU, 0xb20, 0x87000000, 0x88000000, ~UINT64_C(0)) << 6;
    bits |= (uint32_t)!o->update(&native, A733_GPU_R_CCU, 0x0e0, 0, 0x08000000, ~UINT64_C(0)) << 7;
    /* Admitted, for contrast: the read and one listed write. */
    bits |= (uint32_t)!!o->read(&native, A733_GPU_CCU, 0x0e0, &v) << 8;
    bits |= (uint32_t)!!o->update(&native, A733_GPU_CCU, 0x0e0, 0, 0x40000000, ~UINT64_C(0)) << 9;
    return bits | (uint32_t)(register_held != 0) << 31;
}

/* The DCDC4 snapshot the resource manager receives, as raw bytes. */
volatile struct a733_gpu_resource_rail vsu_rail;
struct vsu_acquired { uint32_t result, error, stage, supply_uv, parent_hz; };
volatile struct vsu_acquired vsu_acquired;
static struct a733_gpu_resource_manager manager;
static int manager_read(void *c, uintptr_t address, uint32_t *out)
{ (void)c; *out = *(volatile uint32_t *)address; return 1; }
static int manager_update(void *c, uintptr_t address, uint32_t required, uint32_t set, uint64_t deadline)
{
    (void)c; (void)deadline;
    volatile uint32_t *r = (volatile uint32_t *)address;
    if ((*r & required) != required) return 0;
    *r |= set;
    return 1;
}
static int manager_rail(void *c, struct a733_gpu_resource_rail *out, uint64_t deadline)
{ (void)c; (void)deadline; *out = vsu_rail; return 1; }
uint32_t vsu_resources(void)
{
    memset(&manager, 0, sizeof manager);
    memset(&native, 0, sizeof native);
    const struct a733_gpu_resource_locks locks = {lock_enter, lock_leave, lock_enter, lock_leave, 0};
    const struct a733_gpu_resource_ops ops = {now, manager_read, manager_update, manager_rail};
    if (!a733_gpu_platform_native_init(&native, &locks) || !a733_gpu_resources_init(&manager, &ops, 0)) return 1;
    uint64_t start, token = 0;
    struct a733_gpu_power_resources ready = {0};
    now(0, &start);
    vsu_acquired.result = a733_gpu_resources_acquire(&manager, 0, A733_GPU_RESOURCE_ALL, start + 100000, &token, &ready);
    vsu_acquired.error = manager.error;
    vsu_acquired.stage = manager.stage;
    vsu_acquired.supply_uv = ready.supply_uv;
    vsu_acquired.parent_hz = ready.parent_hz;
    return 0;
}
