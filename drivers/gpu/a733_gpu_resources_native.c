#include "a733_gpu_resources_native.h"
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
    case 0x020020a8: case 0x020020ac: case 0x070101ac:
    case 0x020025c0: case 0x020025e0: case 0x02002b20:
    case 0x02002b24: case 0x07065008: case 0x0708300c:
    case 0x07083010: case 0x07083020: return 1;
    default: return 0;
    }
}
static int native_read(void *context, uintptr_t address, uint32_t *out)
{
    (void)context;
    if (!allowed(address) || !out) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *out = mmio_read(address);
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
static int native_update(void *context, uintptr_t address, uint32_t required,
                         uint32_t set, uint64_t deadline)
{
    struct a733_gpu_resource_native *n = context;
    if (!((address == 0x070101ac && required == 0 && set == 1) ||
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
    mmio_write(address, old | set);
    __asm__ volatile("dsb sy" ::: "memory");
    n->locks.register_leave(n->locks.context);
    return 1;
}
static int native_rail(void *context, struct a733_gpu_resource_rail *out, uint64_t deadline)
{
    struct a733_gpu_resource_native *n = context;
    uint64_t now;
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
