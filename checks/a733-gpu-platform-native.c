/* Reuse the checked I2C transport model and inherited register fixture. */
#define native_check resource_native_check
#include "a733-gpu-resources-native.c"
#undef native_check
#include "a733_gpu_platform_native.h"

static struct a733_gpu_platform_native power_native;
static struct a733_gpu_platform platform;
static unsigned power_writes;
static int platform_enter(void *ctx, uint64_t deadline)
{
    (void)ctx; (void)deadline;
    if (register_lock) issue = 1;
    if (fail_lock) return 0;
    register_lock = 1; ++power_writes; return 1;
}
static int setup(void)
{
    CHECK(!fresh());
    memset(&power_native, 0, sizeof(power_native)); memset(&platform, 0, sizeof(platform));
    power_writes = 0;
    const struct a733_gpu_resource_locks locks = {platform_enter, register_leave, bus_enter, bus_leave, 0};
    native.locks = locks;
    CHECK(a733_gpu_platform_native_init(&power_native, &locks));
    CHECK(a733_gpu_platform_init(&platform, &manager, 0, &power_native.operations, &power_native));
    return 0;
}
static int turn_on(void)
{
    CHECK(a733_gpu_platform_begin(&platform, 1000000));
    int result = 0;
    for (unsigned i = 0; i < 50 && result == 0; ++i) result = a733_gpu_platform_poll(&platform);
    CHECK(result == 1 && platform.token && platform.delegated && !issue);
    return 0;
}
static unsigned clock_calls;
static int regressing_clock(void *context, uint64_t *out)
{
    if (++clock_calls == 4) { *out = platform.power.last_now - 1; return 1; }
    return a733_gpu_platform_native_now(context, out);
}
unsigned native_check(void)
{
    CHECK(!setup());
    uint64_t guard;
    CHECK(a733_gpu_resources_change(&manager, 1, A733_GPU_RESOURCE_PARENT, &guard) == A733_GPU_RESOURCE_OK);
    CHECK(a733_gpu_platform_begin(&platform, 1000000));
    CHECK(a733_gpu_platform_poll(&platform) == 0);
    CHECK(!platform.token && !platform.power.lease_attempted && !power_writes && !reads);
    CHECK(a733_gpu_resources_change_end(&manager, 1, guard, 1) == A733_GPU_RESOURCE_OK);
    atomic_store(&manager.busy, 1);
    CHECK(a733_gpu_platform_poll(&platform) == 0 && !platform.token && !power_writes);
    atomic_store(&manager.busy, 0);
    int result = 0;
    for (unsigned i = 0; i < 50 && result == 0; ++i) result = a733_gpu_platform_poll(&platform);
    CHECK(result == 1 && platform.token && platform.delegated && !issue);
    CHECK((*mmio(0x02002b20) & 0x8700000f) == 0x83000000);
    CHECK((*mmio(0x02002b24) & 0x10001) == 0x10001);
    CHECK(*mmio(0x07065170) == 0x1f1f1f && *mmio(0x07065c00) == 0x08080808);
    CHECK(reads == 6 && opens == 1 && closes == 1 && !bus_lock && !register_lock);
    uint64_t token = platform.token;
    CHECK(a733_gpu_platform_off_begin(&platform, 1000000));
    for (unsigned i = 0; i < 20 && platform.phase != A733_GPU_PLATFORM_RELEASE; ++i)
        CHECK(a733_gpu_platform_off_poll(&platform) == 0);
    CHECK(platform.phase == A733_GPU_PLATFORM_RELEASE && platform.token == token && !platform.delegated);
    atomic_store(&manager.busy, 1);
    CHECK(a733_gpu_platform_off_poll(&platform) == 0 && platform.token == token);
    atomic_store(&manager.busy, 0);
    CHECK(a733_gpu_platform_off_poll(&platform) == 1 && !platform.token);
    CHECK(manager.leases[0].mode == A733_GPU_RESOURCE_FREE);
    CHECK(*mmio(0x070101ac) == 0x41 && *mmio(0x020025c0) == 0x110000ff && *mmio(0x020025e0) == 0x61055802);
    reads = 0;
    CHECK(!turn_on());

    /* Refuse unapproved register operations without taking a lock or writing. */
    unsigned count = power_writes;
    CHECK(!power_native.operations.update(&power_native, A733_GPU_CCU, 0xb20, 0, 1, 1000000));
    CHECK(!power_native.operations.read(&power_native, A733_GPU_CCU, 0xa0, &count));
    CHECK(power_writes == count);

    CHECK(!setup()); fail_read = 3;
    CHECK(a733_gpu_platform_begin(&platform, 1000000));
    CHECK(a733_gpu_platform_poll(&platform) == -1 && platform.token && bus_lock);
    CHECK(manager.leases[0].mode == A733_GPU_RESOURCE_RETAINED && !platform.power.lease_attempted);
    count = reads;
    CHECK(a733_gpu_platform_poll(&platform) == -1 && reads == count);

    CHECK(!setup());
    CHECK(a733_gpu_platform_begin(&platform, 1000000));
    CHECK(a733_gpu_platform_poll(&platform) == 0 && platform.token);
    fail_lock = 1;
    result = 0;
    for (unsigned i = 0; i < 50 && !result; ++i) result = a733_gpu_platform_poll(&platform);
    CHECK(result == -1 && platform.token && platform.delegated && !issue);
    CHECK(manager.leases[0].mode == A733_GPU_RESOURCE_STABLE);
    CHECK(a733_gpu_resources_change(&manager, 1, A733_GPU_RESOURCE_ALL, &guard) == A733_GPU_RESOURCE_BUSY);
    count = power_writes;
    CHECK(a733_gpu_platform_poll(&platform) == -1 && power_writes == count);

    CHECK(!setup());
    CHECK(a733_gpu_platform_begin(&platform, 1000000));
    atomic_store(&manager.busy, 1);
    platform.deadline = 1;
    CHECK(a733_gpu_platform_poll(&platform) == -1 && !platform.token && !platform.power.lease_attempted);
    atomic_store(&manager.busy, 0);
    CHECK(!setup()); CHECK(!turn_on());
    CHECK(a733_gpu_platform_off_begin(&platform, 1000000));
    for (unsigned i = 0; i < 20 && platform.phase != A733_GPU_PLATFORM_RELEASE; ++i)
        CHECK(a733_gpu_platform_off_poll(&platform) == 0);
    platform.deadline = 1;
    CHECK(a733_gpu_platform_off_poll(&platform) == -1 && platform.token);
    CHECK(manager.leases[0].mode == A733_GPU_RESOURCE_STABLE);
    return 0;
}
unsigned clock_check(void)
{
    CHECK(!setup());
    CHECK(a733_gpu_platform_begin(&platform, 1000000));
    CHECK(a733_gpu_platform_poll(&platform) == 0 && platform.token);
    clock_calls = 0; platform.ops.now = regressing_clock;
    CHECK(a733_gpu_platform_poll(&platform) == -1 && clock_calls == 4 && platform.token);
    CHECK(platform.phase == A733_GPU_PLATFORM_FAILED && platform.delegated);
    return 0;
}
