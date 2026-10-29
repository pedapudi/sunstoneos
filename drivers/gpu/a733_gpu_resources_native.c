#include "a733_gpu_resources_native.h"
#include "a733_mmio.h"
#include "boot.h"

static int native_now(void *context, uint64_t *out)
{
    (void)context;
    *out = counter() / (COUNTER_HZ / 1000000);
    return 1;
}
static int allowed(uintptr_t address)
{
    switch (address) {
    case 0x07090160: case 0x02002000: case 0x020020a0:
    case 0x020020a8: case 0x020020ac: case 0x020020e0: case 0x02003908: case 0x02003988: case 0x070101ac:
    case 0x020025c0: case 0x020025e0: case 0x02002b20:
    case 0x02002b24: case 0x07065008: case 0x07066000: case 0x07066008: case 0x0708300c:
    case 0x07083010: case 0x07083020: return 1;
    default: return 0;
    }
}
static int native_read(void *context, uintptr_t address, uint32_t *out)
{
    (void)context;
    if (!allowed(address) || !out) return 0;
    *out = a733_mmio_read32_dsb_sy_both(address);
    return 1;
}
static int native_update(void *context, uintptr_t address, uint32_t required,
                         uint32_t set, uint64_t deadline)
{
    struct a733_gpu_resource_native *n = context;
    if (!((address == 0x070101ac && required == 0 && set == 1) ||
          (address == 0x020025c0 && required == 0 && set == (1u << 6)) ||
          (address == 0x020025e0 && required == 0 && set == (1u << 18)) ||
          (address == 0x020025c0 && required == 0x0100007f && set == 0x010000ff) ||
          (address == 0x020025e0 && required == 0x41045800 && set == 0x41055800))) return 0;
    uint64_t now;
    native_now(n, &now);
    if (now >= deadline || !n->locks.register_enter(n->locks.context, deadline)) return 0;
    native_now(n, &now);
    if (now >= deadline) { n->locks.register_leave(n->locks.context); return 0; }
    uint32_t old;
    if (!native_read(n, address, &old) || (old & required) != required) {
        n->locks.register_leave(n->locks.context); return 0;
    }
    a733_mmio_write32_dsb_sy_after(address, old | set);
    n->locks.register_leave(n->locks.context);
    return 1;
}
static int native_rail(void *context, struct a733_gpu_resource_rail *out, uint64_t deadline)
{
    struct a733_gpu_resource_native *n = context;
    uint64_t now;
    if (n->pmic) {
        native_now(n, &now);
        const struct a733_gpu_resource_lease *lease=n->pmic_manager && n->pmic_client<A733_GPU_RESOURCE_CLIENTS ?
            &n->pmic_manager->leases[n->pmic_client] : 0;
        return now < deadline && n->pmic_view && lease && lease->token &&
            lease->mode==A733_GPU_RESOURCE_STABLE && (lease->mask==A733_GPU_RESOURCE_ALL || lease->mask==A733_GPU_RESOURCE_GPU) &&
            atomic_load_explicit(&n->pmic_manager->busy,memory_order_relaxed)==1 &&
            n->pmic_view->config.provider == n->pmic &&
            a733_pmic_consumer_take(n->pmic_view,n,deadline,out);
    }
    if (n->i2c_held) return 0;
    native_now(n, &now);
    if (now >= deadline || !n->locks.i2c_enter(n->locks.context, deadline)) return 0;
    n->i2c_held = 1; n->rail_step = 0;
    native_now(n, &now);
    if (now >= deadline || a733_i2c_open(&n->bus) != I2C_OK) return 0;
    static const uint8_t registers[] = {0xff, 0x0e, 0x0f, 0x10, 0x15, 0xff};
    uint8_t values[6] = {0};
    for (unsigned i = 0; i < 6; ++i) {
        n->rail_step = i + 1;
        native_now(n, &now);
        if (now >= deadline || a733_i2c_read8(&n->bus, 0x36, registers[i], &values[i]) != I2C_OK)
            return 0;
        native_now(n, &now);
        if (now >= deadline) return 0;
        if ((i == 0 && values[i] != 0) || (i == 1 && values[i] != 3) ||
            (i == 2 && values[i] != 0)) return 0;
    }
    *out = (struct a733_gpu_resource_rail){values[0], values[1], values[2],
                                         values[3], values[4], values[5]};
    n->rail_step = 7;
    a733_i2c_close(&n->bus);
    const uintptr_t addresses[] = {0x0708300c, 0x07083010, 0x07083020};
    const uint32_t masks[] = {0xf4, UINT32_MAX, 0x35};
    const uint32_t expected[] = {n->bus.control & 0xc4, 0xf8, 0x30};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t value;
        n->rail_step = 8 + i;
        native_now(n, &now);
        if (now >= deadline || !native_read(n, addresses[i], &value) ||
            (value & masks[i]) != expected[i]) return 0;
    }
    native_now(n, &now);
    if (now >= deadline) return 0;
    n->locks.i2c_leave(n->locks.context); n->i2c_held = 0;
    native_now(n, &now);
    return now < deadline;
}
int a733_gpu_resources_native_init(struct a733_gpu_resource_native *n,
                                   const struct a733_gpu_resource_locks *locks)
{
    if (!n || !locks || n->initialized || !locks->register_enter || !locks->register_leave ||
        !locks->i2c_enter || !locks->i2c_leave) return 0;
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (frequency != COUNTER_HZ) return 0;
    *n = (struct a733_gpu_resource_native){
        .operations = {native_now, native_read, native_update, native_rail},
        .locks = *locks, .initialized = 1};
    return 1;
}
int a733_gpu_resources_native_use_pmic(struct a733_gpu_resource_native *n,
    struct a733_gpu_resource_manager *m,struct a733_pmic *p)
{
    if(!n || !n->initialized || !m || !m->initialized || m->failed ||
        !p || p->self!=p || p->rails->binding.gpu!=m || n->i2c_held || n->rail_step || n->bus.opened)return 0;
    if(!n->manager) {
        if(m->context!=n || m->ops.now!=n->operations.now || m->ops.read32!=n->operations.read32 ||
            m->ops.update32!=n->operations.update32 || m->ops.rail!=n->operations.rail)return 0;
        n->manager=m;n->manager_operations=m->ops;n->manager_context=m->context;
    }
    if(n->manager!=m || m->context!=n->manager_context ||
        m->ops.now!=n->manager_operations.now || m->ops.read32!=n->manager_operations.read32 ||
        m->ops.update32!=n->manager_operations.update32 || m->ops.rail!=n->manager_operations.rail)return 0;
    if(n->pmic)return n->pmic==p && n->pmic_manager==m;
    for(unsigned i=0;i<A733_GPU_RESOURCE_SLOTS;++i)
        if(m->leases[i].mode!=A733_GPU_RESOURCE_FREE)return 0;
    n->pmic=p;n->pmic_manager=m;return 1;
}
int a733_gpu_resources_native_bind_view(struct a733_gpu_resource_native *n,struct a733_pmic_consumer *c,unsigned client)
{
    if(!n || !n->initialized || !n->pmic || n->pmic_view || !c || c->self!=c ||
        c->failed || c->stage || c->recipient || c->config.provider!=n->pmic ||
        c->config.rail!=A733_RAIL_DCDC4 || client>=A733_GPU_RESOURCE_CLIENTS)return 0;
    c->recipient=n;c->destination=&n->pmic_manager->rail;n->pmic_view=c;n->pmic_client=client;return 1;
}
