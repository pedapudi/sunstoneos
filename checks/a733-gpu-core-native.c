/* Actual native CORE callbacks; only the register lock and counter are modeled. */
#include "../drivers/gpu/a733_gpu_platform_native.c"
#include "../drivers/gpu/a733_gpu_resources_native.c"

volatile unsigned core_lock, core_stage;
static unsigned denied;
static int lock_enter(void *context, uint64_t deadline)
{ (void)context; (void)deadline; if (denied) return 0; core_lock = 1; return 1; }
static void lock_leave(void *context) { (void)context; core_lock = 0; }
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
unsigned core_native_check(unsigned mode)
{
    struct a733_gpu_platform_native n = {0};
    const struct a733_gpu_resource_locks locks = {lock_enter, lock_leave, lock_enter, lock_leave, 0};
    CHECK(a733_gpu_platform_native_init(&n, &locks));
    uint32_t out = 0;
    core_stage = mode + 1;
    if (mode == 0) {
        CHECK(n.operations.read(&n, A733_GPU_CORE, 0, &out) && out == 8);
        CHECK(n.operations.read(&n, A733_GPU_CORE, 8, &out) && out == 8);
        CHECK(n.operations.read(&n, A733_GPU_CORE, 0x20, &out) && out == 0x010101ff);
        CHECK(n.operations.read(&n, A733_GPU_CORE, 0x14, &out) && out == 0x100);
        CHECK(n.operations.read(&n, A733_GPU_CORE, 0xfb0, &out) && out == 0x00031001);
        CHECK(n.operations.update(&n, A733_GPU_CORE, 0x20, UINT32_MAX, 0, 100000));
        CHECK(n.operations.update(&n, A733_GPU_CORE, 0, UINT32_MAX, 8, 100000));
        CHECK(n.operations.update(&n, A733_GPU_CORE, 0, UINT32_MAX, 0, 100000));
        CHECK(native_read(0, 0x07066000, &out) && out == 0);
        CHECK(native_read(0, 0x07066008, &out) && out == 8);
    } else if (mode == 1) {
        const uint32_t offsets[] = {1, 4, 0xc, 0x10, 0x24, 0x38, 0x100, 0xfb4, 0x5000, 0x6000};
        for (unsigned i = 0; i < sizeof(offsets)/sizeof(*offsets); ++i) {
            CHECK(!n.operations.read(&n, A733_GPU_CORE, offsets[i], &out));
            CHECK(!n.operations.update(&n, A733_GPU_CORE, offsets[i], UINT32_MAX, 0, 100000));
        }
        CHECK(!n.operations.read(&n, A733_GPU_CORE, 0, 0));
        CHECK(!n.operations.read(&n, (enum a733_gpu_power_bank)3, 0, &out));
        CHECK(!n.operations.update(&n, A733_GPU_CORE, 8, UINT32_MAX, 0, 100000));
        CHECK(!n.operations.update(&n, A733_GPU_CORE, 0x14, UINT32_MAX, 0, 100000));
        CHECK(!n.operations.update(&n, A733_GPU_CORE, 0xfb0, UINT32_MAX, 0, 100000));
        CHECK(!n.operations.update(&n, A733_GPU_CORE, 0, 15, 8, 100000));
        CHECK(!n.operations.update(&n, A733_GPU_CORE, 0, UINT32_MAX, 1, 100000));
        CHECK(!n.operations.update(&n, A733_GPU_CORE, 0x20, UINT32_MAX, 1, 100000));
        CHECK(!native_read(0, 0x07066020, &out));
        CHECK(!native_read(0, 0x07066001, &out));
        CHECK(!native_update(0, 0x07066000, 0, 8, 100000));
        CHECK(!native_update(0, 0x07066008, 0, 8, 100000));
    } else {
        denied = mode == 2;
        CHECK(!n.operations.update(&n, A733_GPU_CORE, 0, UINT32_MAX, 8, mode == 2 ? 100000 : 1));
    }
    CHECK(!core_lock);
    return 0;
}
