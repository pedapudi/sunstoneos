/* Reuse native power/memory setup without repeating its lifecycle suite. */
#include "a733-gpu-backend.c"
volatile unsigned late_mode;
static int mode_setup(uint32_t count, uint64_t capabilities)
{
    CHECK(!setup()); late_mode = 0; observe = 0;
    *(volatile uint32_t *)(uintptr_t)(A733_GPU_REGISTERS + PVR_COMPUTE_MODE_SYSTEM) = count;
    *(volatile uint64_t *)(uintptr_t)(A733_GPU_REGISTERS + PVR_COMPUTE_MODE_CAPABILITIES) = capabilities;
    observe = 1; CHECK(!bind()); service.epoch = 7;
    return 0;
}
static int mode_power(void)
{
    CHECK(config.power.begin(&backend, operation_deadline));
    CHECK(!config.power.poll(&backend) && !mmio_count);
    CHECK(!config.power.poll(&backend) && !mmio_count);
    expected_access = 1; expected_offset = 0x20; expected_width = 8;
    CHECK(!config.power.poll(&backend) && mmio_count == 1);
    expected_offset = PVR_COMPUTE_MODE_SYSTEM; expected_width = 4;
    return 0;
}
static int mode_admitted(void)
{
    CHECK(!mode_power());
    CHECK(!config.power.poll(&backend) && mmio_count == 2);
    expected_offset = PVR_COMPUTE_MODE_CAPABILITIES; expected_width = 8;
    CHECK(config.power.poll(&backend) == 1 && mmio_count == 3);
    CHECK(backend.phase == A733_GPU_BACKEND_ACTIVE && config.compute_mode(&backend) == 1);
    return 0;
}
unsigned compute_mode_check(void)
{
    /* Documented MC1, compute/primary minimum and reserved capability bits. */
    const uint64_t caps[] = {0x78, 0x18, UINT64_C(0xf00000000000001f)};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(!mode_setup(1, caps[i])); CHECK(!mode_admitted());
        CHECK(backend.mode.capabilities == caps[i] && backend.mode.cores == 1 &&
            backend.mode.token == 1 && backend.mode.epoch == 7);
        unsigned before = mmio_count;
        CHECK(config.compute_mode(&backend) == 1 && mmio_count == before);
    }
    const uint32_t counts[] = {0, 2, 3, 4, 15, 16, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(counts)/sizeof(*counts); ++i) {
        CHECK(!mode_setup(counts[i], 0x78)); CHECK(!mode_power());
        CHECK(config.power.poll(&backend) == -1 && mmio_count == 2 && power.token);
        CHECK(backend.mode.cores == counts[i] && backend.mode.capabilities == 0);
        CHECK(backend.mode.error == ((!counts[i] || counts[i] > 15) ?
            PVR_COMPUTE_MODE_COUNT : PVR_COMPUTE_MODE_MULTICORE_REQUIRED));
        CHECK(config.power.poll(&backend) == -1 && mmio_count == 2);
        CHECK(!config.compute_mode(&backend) && mmio_count == 2);
    }
    const uint64_t missing[] = {0, 8, 16, 0x60};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(!mode_setup(1, missing[i])); CHECK(!mode_power());
        CHECK(!config.power.poll(&backend));
        expected_offset = PVR_COMPUTE_MODE_CAPABILITIES; expected_width = 8;
        CHECK(config.power.poll(&backend) == -1 && mmio_count == 3 && power.token);
        CHECK(backend.mode.error == PVR_COMPUTE_MODE_CAPABILITY);
    }
    /* Current ownership and epoch are checked again without cached admission. */
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(!mode_setup(1, 0x78)); CHECK(!mode_admitted());
        if (i == 0) ++power.token;
        if (i == 1) ++service.epoch;
        if (i == 2) power.phase = A733_GPU_PLATFORM_OFF;
        if (i == 3) service.phase = PVR_GPU_SERVICE_FAILED;
        CHECK(!config.compute_mode(&backend) && mmio_count == 3 && power.token);
    }
    /* New Device page admission and late observations retain ownership. */
    CHECK(!setup()); mapping_mode = 5;
    CHECK(!a733_gpu_backend_bind(&backend, &service, &config, &memory, &power, &external));
    CHECK(!backend.initialized && !mmio_count);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(!mode_setup(1, 0x78)); CHECK(!mode_power());
        if (i == 0) late_mode = 1;
        if (i != 0) {
            CHECK(!config.power.poll(&backend));
            expected_offset = PVR_COMPUTE_MODE_CAPABILITIES; expected_width = 8;
            if (i == 1) late_mode = 2; else mapping_mode = 5;
        }
        CHECK(config.power.poll(&backend) == -1 && power.token);
        CHECK(backend.phase != A733_GPU_BACKEND_ACTIVE);
        CHECK(atomic_load(&backend.error) == (i == 2 ? A733_GPU_BACKEND_MAPPING : A733_GPU_BACKEND_CLOCK));
    }
    /* Lease replacement between the two observations cannot produce proof. */
    CHECK(!mode_setup(1, 0x78)); CHECK(!mode_power());
    CHECK(!config.power.poll(&backend)); ++power.token;
    expected_offset = PVR_COMPUTE_MODE_CAPABILITIES; expected_width = 8;
    CHECK(config.power.poll(&backend) == -1 && power.token == 2);
    CHECK(atomic_load(&backend.error) == A733_GPU_BACKEND_COMPUTE_MODE);
    struct pvr_compute_mode record = {0};
    CHECK(!pvr_compute_mode_count(&record, PVR_FW_A733_BVNC ^ 1, 1, 1, 1));
    CHECK(record.error == PVR_COMPUTE_MODE_IDENTITY);
    record = (struct pvr_compute_mode){0};
    CHECK(!pvr_compute_mode_count(&record, PVR_FW_A733_BVNC, 0, 1, 1));
    CHECK(record.error == PVR_COMPUTE_MODE_LEASE);
    record = (struct pvr_compute_mode){0};
    CHECK(!pvr_compute_mode_capabilities(&record, 0x78));
    CHECK(record.error == PVR_COMPUTE_MODE_ORDER);
    return 0;
}
