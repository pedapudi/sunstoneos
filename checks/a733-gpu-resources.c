#include "a733_gpu_resources.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct reg { uintptr_t address; uint32_t value; };
struct model {
    struct reg regs[11];
    struct a733_gpu_resource_rail rail;
    uint64_t now;
    unsigned calls, fail_at, late_at, updates, partial, corrupt;
    atomic_uint pause, entered;
};
static struct model model;
static struct a733_gpu_resource_manager manager;
static int callback(struct model *m)
{
    ++m->calls; ++m->now;
    if (m->calls == m->late_at) m->now = 100000;
    return m->calls != m->fail_at;
}
static int now(void *context, uint64_t *out)
{ struct model *m = context; if (!callback(m)) return 0; *out = m->now; return 1; }
static uint32_t *reg(struct model *m, uintptr_t address)
{
    for (unsigned i = 0; i < 11; ++i) if (m->regs[i].address == address) return &m->regs[i].value;
    assert(0); return 0;
}
static int read32(void *context, uintptr_t address, uint32_t *out)
{
    struct model *m = context;
    if (atomic_load(&m->pause)) {
        atomic_store(&m->entered, 1);
        while (atomic_load(&m->pause)) { }
    }
    if (!callback(m)) return 0;
    *out = *reg(m, address); return 1;
}
static int update32(void *context, uintptr_t address, uint32_t required,
                     uint32_t set, uint64_t deadline)
{
    struct model *m = context;
    assert(m->now < deadline);
    if (!callback(m)) return 0;
    uint32_t *value = reg(m, address);
    assert((*value & required) == required);
    ++m->updates;
    if (!m->corrupt) *value |= set;
    return !m->partial;
}
static int rail(void *context, struct a733_gpu_resource_rail *out, uint64_t deadline)
{
    struct model *m = context;
    assert(m->now < deadline);
    if (!callback(m)) return 0;
    *out = m->rail; return 1;
}
static const struct a733_gpu_resource_ops ops = {now, read32, update32, rail};
static void fresh(void)
{
    memset(&model, 0, sizeof(model)); memset(&manager, 0, sizeof(manager));
    atomic_init(&model.pause, 0); atomic_init(&model.entered, 0);
    const struct reg regs[] = {
        {0x07090160, 0}, {0x02002000, 0xf8000000u | (99u << 16) | (99u << 8)},
        {0x020020a0, 0xfe000000u | (1u << 20) | (99u << 8)},
        {0x020020a8, 0}, {0x020020ac, 0}, {0x070101ac, 0x40},
        {0x020025c0, 0x1100007f}, {0x020025e0, 0x61045802},
        {0x02002b20, 0x10200100}, {0x02002b24, 0x24680000}, {0x07065008, 0},
    };
    memcpy(model.regs, regs, sizeof(regs));
    model.rail = (struct a733_gpu_resource_rail){0, 3, 0, 0xff, 0x9e, 0};
    model.now = 100;
    assert(a733_gpu_resources_init(&manager, &ops, &model));
}
static void terminal(void)
{
    uint64_t token = 0; struct a733_gpu_power_resources ready = {0};
    unsigned calls = model.calls;
    assert(a733_gpu_resources_acquire(&manager, 2, 1, 10000, &token, &ready) == A733_GPU_RESOURCE_FAULT);
    assert(a733_gpu_resources_change(&manager, 2, 1, &token) == A733_GPU_RESOURCE_FAULT);
    assert(a733_gpu_resources_release(&manager, 0, 1, 10000) == A733_GPU_RESOURCE_FAULT);
    assert(a733_gpu_resources_change_end(&manager, 0, 1, 1) == A733_GPU_RESOURCE_FAULT);
    assert(calls == model.calls);
}
static enum a733_gpu_resource_result acquire(unsigned client, uint32_t mask, uint64_t *token)
{
    struct a733_gpu_power_resources ready = {7, 9};
    enum a733_gpu_resource_result r = a733_gpu_resources_acquire(&manager, client, mask, 10000, token, &ready);
    if (r == A733_GPU_RESOURCE_OK) {
        assert(ready.parent_hz == ((mask & A733_GPU_RESOURCE_PARENT) ? 400000000 : 0));
        assert(ready.supply_uv == ((mask & A733_GPU_RESOURCE_RAIL) ? 800000 : 0));
    } else assert(ready.parent_hz == 7 && ready.supply_uv == 9);
    return r;
}
static void *worker(void *unused)
{
    (void)unused; uint64_t token;
    assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_OK);
    return 0;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    fresh(); uint64_t token = 0, shared = 0, change = 0;
    if (!strcmp(argv[1], "--pll-input-only")) {
        assert(acquire(0, A733_GPU_RESOURCE_PARENT, &token) == A733_GPU_RESOURCE_OK);
        assert(!model.updates);
        fresh(); *reg(&model, 0x020020a0) |= 2;
        assert(acquire(0, A733_GPU_RESOURCE_PARENT, &token) == A733_GPU_RESOURCE_FAULT);
        assert(manager.error == A733_GPU_RESOURCE_PLL && !model.updates);
        assert(manager.leases[0].mode == A733_GPU_RESOURCE_RETAINED);
        terminal();
        puts("GPU inherited PERI0 input divisor: unchanged profile admitted, divided input retained without writes");
        return 0;
    }
    assert(!a733_gpu_resources_init(&manager, &ops, &model));
    assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_OK);
    assert(token == 1 && model.updates == 3);
    assert(*reg(&model, 0x070101ac) == 0x41);
    assert(*reg(&model, 0x020025c0) == 0x110000ff);
    assert(*reg(&model, 0x020025e0) == 0x61055802);
    unsigned acquire_calls = model.calls;
    assert(a733_gpu_resources_release(&manager, 0, token, 10000) == A733_GPU_RESOURCE_OK);
    unsigned total_calls = model.calls;
    FILE *f = fopen(argv[1], "w"); assert(f);
    for (unsigned i = 0; i < 11; ++i) fprintf(f, "%lx %08x\n", (unsigned long)model.regs[i].address, model.regs[i].value);
    assert(!fclose(f));
    assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_OK && token == 2);
    assert(model.updates == 3); /* Adopt already enabled gates without a write. */
    assert(acquire(1, A733_GPU_RESOURCE_PARENT | A733_GPU_RESOURCE_RAIL, &shared) == A733_GPU_RESOURCE_OK);
    unsigned calls = model.calls;
    assert(acquire(2, A733_GPU_RESOURCE_ALL, &change) == A733_GPU_RESOURCE_BUSY && !change);
    assert(a733_gpu_resources_change(&manager, 2, A733_GPU_RESOURCE_PARENT, &change) == A733_GPU_RESOURCE_BUSY);
    assert(a733_gpu_resources_change(&manager, 2, A733_GPU_RESOURCE_OSC, &change) == A733_GPU_RESOURCE_BUSY);
    assert(a733_gpu_resources_release(&manager, 0, token - 1, 10000) == A733_GPU_RESOURCE_INVALID);
    assert(calls == model.calls);
    assert(a733_gpu_resources_release(&manager, 0, token, 10000) == A733_GPU_RESOURCE_OK);
    assert(a733_gpu_resources_change(&manager, 2, A733_GPU_RESOURCE_PARENT, &change) == A733_GPU_RESOURCE_BUSY);
    assert(a733_gpu_resources_release(&manager, 1, shared, 10000) == A733_GPU_RESOURCE_OK);
    assert(a733_gpu_resources_change(&manager, 2, A733_GPU_RESOURCE_RAIL, &change) == A733_GPU_RESOURCE_OK);
    assert(acquire(1, A733_GPU_RESOURCE_OSC, &shared) == A733_GPU_RESOURCE_OK);
    assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_BUSY);
    assert(a733_gpu_resources_change_end(&manager, 2, change, 1) == A733_GPU_RESOURCE_OK);
    assert(a733_gpu_resources_release(&manager, 1, shared, 10000) == A733_GPU_RESOURCE_OK);
    unsigned faults = 0;
    for (unsigned i = 1; i <= total_calls; ++i) {
        fresh(); model.fail_at = i;
        enum a733_gpu_resource_result r = acquire(0, A733_GPU_RESOURCE_ALL, &token);
        if (r == A733_GPU_RESOURCE_OK) r = a733_gpu_resources_release(&manager, 0, token, 10000);
        assert(r == A733_GPU_RESOURCE_FAULT && manager.error == A733_GPU_RESOURCE_IO);
        assert(manager.leases[0].mode == A733_GPU_RESOURCE_RETAINED);
        terminal(); ++faults;
    }
    const struct { uintptr_t address; uint32_t xor; enum a733_gpu_resource_error error; } bad[] = {
        {0x07090160, 0x4000, A733_GPU_RESOURCE_OSCILLATOR},
        {0x02002000, 0x10000000, A733_GPU_RESOURCE_PLL},
        {0x02002000, 0x00010000, A733_GPU_RESOURCE_PLL},
        {0x020020a0, 0x10000000, A733_GPU_RESOURCE_PLL},
        {0x020020a0, 0x00000100, A733_GPU_RESOURCE_PLL},
        {0x020020a8, 0x80000000, A733_GPU_RESOURCE_PLL},
        {0x020020ac, 0x08000000, A733_GPU_RESOURCE_PLL},
        {0x020025c0, 1, A733_GPU_RESOURCE_GATE},
        {0x020025e0, 0x1000000, A733_GPU_RESOURCE_GATE},
        {0x02002b20, 0x80000000, A733_GPU_RESOURCE_LIVE},
        {0x02002b24, 0x10000, A733_GPU_RESOURCE_LIVE},
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        fresh(); *reg(&model, bad[i].address) ^= bad[i].xor;
        assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_FAULT);
        assert(manager.error == bad[i].error);
        if (bad[i].error == A733_GPU_RESOURCE_LIVE) assert(!model.updates);
        terminal(); ++faults;
    }
    for (unsigned i = 0; i < 6; ++i) {
        fresh(); ((uint8_t *)&model.rail)[i] ^= i == 3 ? 8 : 1;
        assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_FAULT);
        assert(manager.error == A733_GPU_RESOURCE_SUPPLY && !model.updates); terminal(); ++faults;
    }
    fresh();
    assert(acquire(0, A733_GPU_RESOURCE_AHB, &token) == A733_GPU_RESOURCE_FAULT);
    assert(manager.error == A733_GPU_RESOURCE_GATE && !model.updates); terminal(); ++faults;
    fresh(); struct a733_gpu_power_resources alias_ready = {0};
    assert(a733_gpu_resources_acquire(&manager, 0, 1, 10000, &manager.next_token, &alias_ready) == A733_GPU_RESOURCE_INVALID);
    assert(a733_gpu_resources_change(&manager, 0, 1, &manager.next_token) == A733_GPU_RESOURCE_INVALID);
    assert(a733_gpu_resources_acquire(&manager, 0, 1, 10000, &token,
        (struct a733_gpu_power_resources *)&token) == A733_GPU_RESOURCE_INVALID);
    assert(!model.calls);
    fresh(); model.partial = 1;
    assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_FAULT);
    assert(*reg(&model, 0x070101ac) == 0x41 && manager.leases[0].mask == A733_GPU_RESOURCE_ALL);
    terminal(); ++faults;
    fresh(); model.corrupt = 1;
    assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_FAULT);
    assert(manager.error == A733_GPU_RESOURCE_GATE); terminal(); ++faults;
    fresh(); model.late_at = 3;
    assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_FAULT);
    assert(manager.error == A733_GPU_RESOURCE_TIMEOUT); terminal(); ++faults;
    fresh(); manager.last_now = 200;
    assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_FAULT);
    assert(manager.error == A733_GPU_RESOURCE_CLOCK); terminal(); ++faults;
    fresh(); assert(acquire(0, A733_GPU_RESOURCE_ALL, &token) == A733_GPU_RESOURCE_OK);
    *reg(&model, 0x07065008) = 8;
    assert(a733_gpu_resources_release(&manager, 0, token, 10000) == A733_GPU_RESOURCE_FAULT);
    assert(manager.leases[0].mode == A733_GPU_RESOURCE_RETAINED); terminal(); ++faults;
    fresh(); assert(a733_gpu_resources_change(&manager, 0, A733_GPU_RESOURCE_RAIL, &token) == A733_GPU_RESOURCE_OK);
    assert(a733_gpu_resources_change_end(&manager, 0, token, 0) == A733_GPU_RESOURCE_FAULT);
    terminal(); ++faults;
    fresh(); atomic_store(&model.pause, 1);
    pthread_t thread; assert(!pthread_create(&thread, 0, worker, 0));
    while (!atomic_load(&model.entered)) { }
    assert(acquire(1, A733_GPU_RESOURCE_ALL, &shared) == A733_GPU_RESOURCE_BUSY);
    assert(a733_gpu_resources_change(&manager, 2, A733_GPU_RESOURCE_RAIL, &change) == A733_GPU_RESOURCE_BUSY);
    atomic_store(&model.pause, 0); assert(!pthread_join(thread, 0));
    assert(acquire(1, A733_GPU_RESOURCE_PARENT, &shared) == A733_GPU_RESOURCE_OK);
    printf("PASS GPU resource sharing, change guards, concurrent admission, inherited state and %u retained faults (%u acquisition callbacks)\n",
           faults, acquire_calls);
}
