#include "a733_gpu_resources_native.h"
#include <stddef.h>

void *memset(void *p, int v, size_t n)
{ volatile unsigned char *q = p; while (n--) *q++ = (unsigned char)v; return p; }
static struct a733_gpu_resource_manager manager;
static struct a733_gpu_resource_native native;
static unsigned reads, opens, closes, bus_lock, register_lock, fail_read, fail_lock, wrong_page;
static const uint8_t registers[6] = {0xff, 0x0e, 0x0f, 0x10, 0x15, 0xff};
static int issue;
static unsigned bad_close;
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static uint32_t *mmio(uintptr_t address) { return (uint32_t *)address; }
static int register_enter(void *ctx, uint64_t deadline)
{
    (void)ctx; (void)deadline;
    if (register_lock || atomic_load(&manager.busy) != 1) issue = 1;
    if (fail_lock) return 0;
    register_lock = 1; return 1;
}
static void register_leave(void *ctx)
{ (void)ctx; if (!register_lock) issue = 1; register_lock = 0; }
static int bus_enter(void *ctx, uint64_t deadline)
{ (void)ctx; (void)deadline; if (bus_lock) issue = 1; bus_lock = 1; return 1; }
static void bus_leave(void *ctx)
{ (void)ctx; if (!bus_lock) issue = 1; bus_lock = 0; }
enum i2c_result a733_i2c_open(struct a733_i2c *bus)
{ ++opens; if (!bus_lock) issue = 1; bus->opened = 1; bus->control = 0x44; return I2C_OK; }
enum i2c_result a733_i2c_read8(struct a733_i2c *bus, uint8_t address, uint8_t reg, uint8_t *out)
{
    if (!bus_lock || !bus->opened || address != 0x36 || reads >= 6 || reg != registers[reads]) issue = 1;
    static const uint8_t values[6] = {0, 3, 0, 0xff, 0x9e, 0};
    ++reads;
    *mmio(0x0708300c) = 0x40;
    if (reads == fail_read) return I2C_STATUS;
    *out = (wrong_page && reads == 1) ? 1 : values[reads - 1]; return I2C_OK;
}
void a733_i2c_close(struct a733_i2c *bus)
{
    if (!bus_lock || !bus->opened) issue = 1;
    ++closes; bus->opened = 0;
    if (bad_close != 1) *mmio(0x0708300c) = bus->control & ~0x38u;
    if (bad_close == 2) *mmio(0x0708300c) |= 0x20;
    if (bad_close == 3) *mmio(0x07083010) = 0x28;
    if (bad_close == 4) *mmio(0x07083020) = 0x10;
}
static int fresh(void)
{
    memset(&manager, 0, sizeof(manager)); memset(&native, 0, sizeof(native));
    reads = opens = closes = bus_lock = register_lock = fail_read = fail_lock = wrong_page = bad_close = 0;
    *mmio(0x07090160) = 0x10000002;
    *mmio(0x02002000) = 0xf8636300;
    *mmio(0x020020a0) = 0xfe106300;
    *mmio(0x02003908) = 0x8fff0fff;
    *mmio(0x02003988) = 0x0fff0000;
    *mmio(0x020020a8) = *mmio(0x020020ac) = 0;
    *mmio(0x070101ac) = 0x40;
    *mmio(0x020025c0) = 0x1100007f;
    *mmio(0x020025e0) = 0x61045802;
    *mmio(0x02002b20) = 0x10200100;
    *mmio(0x02002b24) = 0x24680000;
    *mmio(0x07065008) = 0;
    *mmio(0x0708300c) = 0x44;
    *mmio(0x07083010) = 0xf8;
    *mmio(0x07083020) = 0x30;
    const struct a733_gpu_resource_locks locks = {register_enter, register_leave, bus_enter, bus_leave, 0};
    CHECK(a733_gpu_resources_native_init(&native, &locks));
    CHECK(a733_gpu_resources_init(&manager, &native.operations, &native));
    return 0;
}
unsigned native_check(void)
{
    uint64_t token; struct a733_gpu_power_resources ready;
    int r = fresh(); CHECK(!r);
    CHECK(a733_gpu_resources_acquire(&manager, 0, A733_GPU_RESOURCE_ALL, 1000000, &token, &ready) == A733_GPU_RESOURCE_OK);
    CHECK(ready.parent_hz == 400000000 && ready.supply_uv == 800000);
    CHECK(reads == 6 && opens == 1 && closes == 1 && !bus_lock && !register_lock && !issue);
    CHECK(*mmio(0x070101ac) == 0x41 && *mmio(0x020025c0) == 0x110000ff && *mmio(0x020025e0) == 0x61055802);
    CHECK(a733_gpu_resources_release(&manager, 0, token, 1000000) == A733_GPU_RESOURCE_OK);
    CHECK(!native.operations.read32(&native, 0x100, (uint32_t *)&r));
    CHECK(!native.operations.update32(&native, 0x070101ac, 1, 1, 1000000));
    for (unsigned i = 1; i <= 6; ++i) {
        CHECK(!fresh()); fail_read = i;
        CHECK(a733_gpu_resources_acquire(&manager, 0, A733_GPU_RESOURCE_ALL, 1000000, &token, &ready) == A733_GPU_RESOURCE_FAULT);
        CHECK(reads == i && !closes && bus_lock && native.i2c_held && !issue);
        CHECK(manager.leases[0].mode == A733_GPU_RESOURCE_RETAINED);
    }
    for (unsigned i = 1; i <= 4; ++i) {
        CHECK(!fresh()); bad_close = i;
        CHECK(a733_gpu_resources_acquire(&manager, 0, A733_GPU_RESOURCE_ALL, 1000000, &token, &ready) == A733_GPU_RESOURCE_FAULT);
        CHECK(reads == 6 && closes == 1 && bus_lock && native.i2c_held && !issue);
        CHECK(manager.leases[0].mode == A733_GPU_RESOURCE_RETAINED);
    }
    CHECK(!fresh()); wrong_page = 1;
    CHECK(a733_gpu_resources_acquire(&manager, 0, A733_GPU_RESOURCE_ALL, 1000000, &token, &ready) == A733_GPU_RESOURCE_FAULT);
    CHECK(reads == 1 && bus_lock && !closes && !issue);
    CHECK(!fresh()); fail_lock = 1;
    CHECK(a733_gpu_resources_acquire(&manager, 0, A733_GPU_RESOURCE_ALL, 1000000, &token, &ready) == A733_GPU_RESOURCE_FAULT);
    CHECK(*mmio(0x070101ac) == 0x40 && !register_lock && !bus_lock && !issue);
    return 0;
}
