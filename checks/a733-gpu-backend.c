#include "a733_gpu_backend.h"
#include "a733_gpu_platform_native.h"
#include <string.h>
#include "gpu-backend-traces.h"

static struct a733_gpu_backend backend;
static struct pvr_gpu_service service;
static struct pvr_gpu_service_config config;
static struct a733_gpu_memory memory;
static struct a733_gpu_platform power;
static struct a733_gpu_resource_manager manager;
static const unsigned char image[8] = {1};
static _Alignas(4096) unsigned char firmware_arena[6 * 4096];
_Alignas(4096) unsigned char resource_arena[64 * 4096];
static uint64_t pages[70];
static unsigned provider_calls, provider_mode, free_context;
volatile unsigned mapping_mode, observe, power_enabled, identity_confirmed, late_identity;
volatile unsigned expected_access, expected_offset, expected_width, mmio_count;
volatile uint64_t expected_value;
static unsigned power_polls, clock_regression;
static uint64_t operation_deadline;
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

/* The real power provider has separate native integration evidence. These
 * replacements check composition arguments and delayed completion only. */
int a733_gpu_platform_begin(void *context, uint64_t deadline)
{
    if (context != &power || deadline != operation_deadline ||
        (power.phase != A733_GPU_PLATFORM_NEW && power.phase != A733_GPU_PLATFORM_OFF)) return 0;
    power.phase = A733_GPU_PLATFORM_ADMIT; power_polls = 0; return 1;
}
int a733_gpu_platform_poll(void *context)
{
    if (context != &power) return -1;
    if (++power_polls == 1) {
        if (clock_regression == 1) power.last_now = operation_deadline - 1;
        if (clock_regression == 2) power.power.last_now = operation_deadline - 1;
        return 0;
    }
    power.phase = A733_GPU_PLATFORM_ON; power.token = 1; power.delegated = 1;
    power_enabled = 1; return 1;
}
static int isolated(void *context, struct a733_gpu_platform *p, uint64_t deadline)
{
    if (context != &provider_calls || p != &power || deadline != operation_deadline) return 0;
    ++provider_calls;
    if (provider_mode == 1) return 1;
    if (provider_mode == 2) {
        power.phase = A733_GPU_PLATFORM_OFF; power.token = power.delegated = 0;
        power_enabled = identity_confirmed = 0; return 1;
    }
    return 0;
}
static int handled(void *context, const unsigned char *command)
{ return context == &provider_calls && command[0] == 7; }
static struct a733_gpu_backend_external external = {.isolate=isolated, .event=handled, .context=&provider_calls};
static uint64_t until(void)
{ uint64_t time; a733_gpu_platform_native_now(NULL, &time); return time + 100000; }
static void *region(unsigned id)
{ return memory.resources[id].storage.cpu; }
static int setup(void)
{
    observe = 0; mapping_mode = power_enabled = identity_confirmed = late_identity = 0;
    expected_access = expected_offset = expected_width = mmio_count = 0;
    provider_calls = provider_mode = clock_regression = 0;
    memset(&backend, 0, sizeof(backend)); memset(&service, 0, sizeof(service));
    memset(&config, 0, sizeof(config)); memset(&memory, 0, sizeof(memory));
    memset(&power, 0, sizeof(power)); memset(&manager, 0, sizeof(manager));
    memset(firmware_arena, 0, sizeof(firmware_arena)); memset(resource_arena, 0, sizeof(resource_arena));
    unsigned page = 0;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned count = i == 2 ? 4 : 1;
        config.firmware_buffers[i] = (struct pvr_mips_buffer){{firmware_arena + page * 4096, count * 4096}, pages + page, count};
        for (unsigned j = 0; j < count; ++j) pages[page + j] = (uintptr_t)firmware_arena + (page + j) * 4096;
        page += count;
    }
    unsigned shared_page = 0;
    for (unsigned i = 0; i < PVR_FW_RESOURCE_COUNT; ++i) {
        unsigned count = pvr_fw_state_layout(i)->allocation_bytes / 4096;
        config.resources[i] = (struct pvr_mips_buffer){{resource_arena + shared_page * 4096, count * 4096}, pages + page, count};
        for (unsigned j = 0; j < count; ++j) pages[page + j] = (uintptr_t)resource_arena + (shared_page + j) * 4096;
        page += count; shared_page += count;
    }
    CHECK(page <= 70 && shared_page <= 64);
    CHECK(a733_gpu_memory_init(&memory, config.firmware_buffers, config.resources));
    power.initialized = manager.initialized = 1; power.manager = &manager;
    config.firmware = image; config.firmware_bytes = sizeof(image);
    config.state = (struct pvr_fw_state_config){PVR_FW_A733_BVNC, 400000000, 1, 16};
    operation_deadline = until();
    *(volatile uint64_t *)(uintptr_t)(A733_GPU_REGISTERS + 0x20) = PVR_FW_A733_BVNC;
    *(volatile uint32_t *)(uintptr_t)(A733_GPU_REGISTERS + PVR_COMPUTE_MODE_SYSTEM) = 1;
    *(volatile uint64_t *)(uintptr_t)(A733_GPU_REGISTERS + PVR_COMPUTE_MODE_CAPABILITIES) = 0x78;
    *(volatile uint32_t *)(uintptr_t)(A733_GPU_GICD) = 0;
    observe = 1;
    return 0;
}
static int bind(void)
{
    CHECK(a733_gpu_backend_bind(&backend, &service, &config, &memory, &power, &external));
    CHECK(config.context == &backend && !config.enable && config.power.begin && config.irq.mask);
    service.config = config; service.initialized = 1; service.epoch = 1;
    CHECK(!mmio_count && !power_enabled);
    return 0;
}
static int enabled(void)
{
    CHECK(config.power.begin(&backend, operation_deadline));
    CHECK(!config.power.poll(&backend) && !power_enabled && !mmio_count);
    CHECK(!config.power.poll(&backend) && power_enabled && !mmio_count);
    int result = 0;
    for (unsigned i = 0; i < 5 && !result; ++i) result = config.power.poll(&backend);
    CHECK(result == 1 && backend.identity == PVR_FW_A733_BVNC);
    identity_confirmed = 1; return 0;
}
static int accesses(const struct trace *trace, unsigned count, int stopping)
{
    for (unsigned i = 0; i < count; ++i) {
        const struct trace *t = &trace[i];
        expected_access = t->write ? 2 : 1; expected_offset = t->offset;
        expected_width = t->width; expected_value = t->value;
        service.start.phase = PVR_GPU_START_RUNNING; service.start.deadline = operation_deadline;
        service.start.index = 0; service.start.count = 1; service.start.slc_value = 0;
        service.start.steps[0] = (struct pvr_gpu_start_step){0, t->offset, t->width, t->value};
        service.stop.phase = PVR_GPU_STOP_REGISTERS; service.stop.deadline = operation_deadline;
        service.stop.index = 0; service.stop.count = 1;
        service.stop.steps[0] = (struct pvr_gpu_stop_step){0, t->offset, t->width, 0, t->value};
        unsigned previous = mmio_count; uint64_t value = 0;
        int success = t->write ? (stopping ? config.stop.write : config.start.write)(&backend, t->offset, t->width, t->value) :
                                (stopping ? config.stop.read : config.start.read)(&backend, t->offset, t->width, &value);
        CHECK(success && mmio_count == previous + 1);
    }
    expected_access = 0; return 0;
}
static int uncertain_allocate(void *context, uint64_t epoch, uint64_t owner, uint64_t token,
                              unsigned slot, struct pvr_free_list_page *out)
{
    if (context != &free_context || epoch != 3 || owner != 4 || token != 5 || slot != 6) return 0;
    out->lease = 9; return -1;
}
static int forwarded_page(void *ctx, const struct pvr_free_list_page *page)
{ return ctx == &free_context && page->lease == 9 ? 5 : 0; }
static int forwarded_read(void *ctx, uint64_t token, enum pvr_free_list_memory m, uint32_t off, void *out, size_t n)
{ (void)m; (void)off; (void)out; (void)n; return ctx == &free_context && token == 5 ? 6 : 0; }
static int forwarded_write(void *ctx, uint64_t token, enum pvr_free_list_memory m, uint32_t off, const void *in, size_t n)
{ (void)m; (void)off; (void)in; (void)n; return ctx == &free_context && token == 5 ? 7 : 0; }
static int forwarded_publish(void *ctx, uint64_t token, enum pvr_free_list_memory m, uint32_t off, size_t n)
{ (void)m; (void)off; (void)n; return ctx == &free_context && token == 5 ? 8 : 0; }
static int failed_recovery(void *ctx, uint64_t token)
{ return ctx == &free_context && token == 5 ? -2 : 0; }
unsigned backend_check(void)
{
    CHECK(!setup());
    struct pvr_gpu_service_config saved = config;
    for (unsigned i = 1; i <= 4; ++i) {
        mapping_mode = i;
        CHECK(!a733_gpu_backend_bind(&backend, &service, &config, &memory, &power, &external));
        CHECK(!memcmp(&saved, &config, sizeof(config)) && !backend.initialized && !mmio_count);
    }
    mapping_mode = 0;
    CHECK(!a733_gpu_backend_bind(&backend, &service, &config, &memory, &power, NULL));
    CHECK(!bind()); CHECK(!enabled());
    CHECK(!accesses(start_trace_0, sizeof(start_trace_0) / sizeof(*start_trace_0), 0));
    CHECK(!accesses(start_trace_1, sizeof(start_trace_1) / sizeof(*start_trace_1), 0));
    CHECK(config.start.publish(&backend) && memory.phase == A733_GPU_MEMORY_PUBLISHED);
    *(unsigned char *)((uintptr_t)region(PVR_FW_SYSTEM_INIT) + 352) = 1;
    uint32_t value = 0;
    CHECK(config.start.started(&backend, &value) && value == 1);
    *(unsigned char *)((uintptr_t)region(PVR_FW_OS_INIT) + 96) = 1;
    unsigned char updated = 0;
    CHECK(config.control.compat(&backend, 56, &updated, 1) && updated == 1);
    value = 16;
    CHECK(config.queue.write(&backend, PVR_CCB_HOST_CONTROL, 0, &value, 4));
    value = 0;
    CHECK(config.queue.read(&backend, PVR_CCB_HOST_CONTROL, 0, &value, 4) && value == 16);
    CHECK(config.queue.barrier(&backend, PVR_CCB_DEVICE_COMPLETE));
    service.control.phase = PVR_GPU_CONTROL_READY;
    CHECK(config.queue.kick(&backend, 0xb00, 0));
    CHECK(config.control.read32(&backend, 0x8a8, &value));
    CHECK(config.control.write32(&backend, 0x8b0, 1));
    const unsigned char command[88] = {7}; CHECK(config.control.event(&backend, command));
    CHECK(config.stop.sync_write(&backend, 0));
    *(uint32_t *)region(PVR_FW_POWER_SYNC) = 1;
    CHECK(config.stop.sync_read(&backend, &value) && value == 1);
    CHECK(!accesses(stop_trace_0, sizeof(stop_trace_0) / sizeof(*stop_trace_0), 1));
    CHECK(!accesses(stop_trace_1, sizeof(stop_trace_1) / sizeof(*stop_trace_1), 1));
    CHECK(config.irq.mask(&backend)); CHECK(config.irq.rearm(&backend) == 1);
    service.stop.phase = PVR_GPU_STOP_ISOLATE; provider_mode = 2;
    CHECK(config.stop.isolate(&backend) && provider_calls == 1);
    CHECK(memory.phase == A733_GPU_MEMORY_OFFLINE && backend.phase == A733_GPU_BACKEND_ISOLATED && !power.token);
    mmio_count = 0; CHECK(!enabled());

    /* Legacy identity uses three reads, then count/capabilities add two. */
    CHECK(!setup()); observe = 0;
    *(volatile uint64_t *)(uintptr_t)(A733_GPU_REGISTERS + 0x20) = 0x243800;
    *(volatile uint32_t *)(uintptr_t)(A733_GPU_REGISTERS + 0x18) = 0x68b7;
    observe = 1; CHECK(!bind()); CHECK(!enabled()); CHECK(mmio_count == 5);

    CHECK(!setup()); observe = 0;
    *(volatile uint64_t *)(uintptr_t)(A733_GPU_REGISTERS + 0x20) = PVR_FW_A733_BVNC + 1;
    observe = 1; CHECK(!bind()); CHECK(config.power.begin(&backend, operation_deadline));
    CHECK(!config.power.poll(&backend)); CHECK(!config.power.poll(&backend));
    CHECK(config.power.poll(&backend) == -1 && power.token && atomic_load(&backend.error) == A733_GPU_BACKEND_IDENTITY);
    unsigned previous = mmio_count; CHECK(config.power.poll(&backend) == -1 && mmio_count == previous);

    CHECK(!setup()); CHECK(!bind()); late_identity = 1;
    CHECK(config.power.begin(&backend, operation_deadline));
    CHECK(!config.power.poll(&backend)); CHECK(!config.power.poll(&backend));
    CHECK(config.power.poll(&backend) == -1 && power.token && atomic_load(&backend.error) == A733_GPU_BACKEND_CLOCK);

    CHECK(!setup()); CHECK(!bind()); CHECK(!enabled());
    service.start.phase = PVR_GPU_START_RUNNING; service.start.deadline = operation_deadline;
    service.start.count = 1; service.start.steps[0] = (struct pvr_gpu_start_step){0, 0x100, 8, 0};
    previous = mmio_count;
    CHECK(!config.start.write(&backend, 0x100, 4, 0) && mmio_count == previous);
    CHECK(!config.start.write(&backend, 0x100, 8, 0) && mmio_count == previous);
    CHECK(config.irq.mask(&backend));

    CHECK(!setup()); CHECK(!bind()); CHECK(!enabled());
    service.start.phase = PVR_GPU_START_RUNNING; service.start.deadline = operation_deadline;
    service.start.count = 1; service.start.steps[0] = (struct pvr_gpu_start_step){0, 0x100, 8, 0};
    previous = mmio_count;
    CHECK(!config.start.read(&backend, 0x100, 8, &backend.identity) && mmio_count == previous);

    CHECK(!setup()); CHECK(!bind()); CHECK(!enabled());
    service.start.phase = PVR_GPU_START_RUNNING; service.start.deadline = operation_deadline;
    service.start.count = 1; service.start.steps[0] = (struct pvr_gpu_start_step){0, 0x100, 8, 0};
    mapping_mode = 1; previous = mmio_count; uint64_t untouched = 7;
    CHECK(!config.start.read(&backend, 0x100, 8, &untouched) && untouched == 7 && mmio_count == previous);

    CHECK(!setup()); CHECK(!bind()); CHECK(!enabled()); CHECK(config.start.publish(&backend));
    service.stop.phase = PVR_GPU_STOP_ISOLATE; service.stop.deadline = operation_deadline;
    provider_mode = 1;
    CHECK(!config.stop.isolate(&backend) && provider_calls == 1 && power.token);
    CHECK(memory.phase == A733_GPU_MEMORY_PUBLISHED && atomic_load(&backend.error) == A733_GPU_BACKEND_ISOLATION);
    CHECK(!setup()); config.context = &free_context; config.free_lists.count = 1;
    config.free_lists.ops = (struct pvr_free_list_ops){uncertain_allocate, forwarded_page, forwarded_read,
        forwarded_write, forwarded_publish, failed_recovery};
    CHECK(!bind()); struct pvr_free_list_page page = {0};
    CHECK(config.free_lists.ops.allocate(&backend, 3, 4, 5, 6, &page) == -1 && page.lease == 9);
    CHECK(config.free_lists.ops.publish_page(&backend, &page) == 5);
    CHECK(config.free_lists.ops.read(&backend, 5, PVR_FREE_LIST_TABLE, 0, &value, 4) == 6);
    CHECK(config.free_lists.ops.write(&backend, 5, PVR_FREE_LIST_TABLE, 0, &value, 4) == 7);
    CHECK(config.free_lists.ops.publish(&backend, 5, PVR_FREE_LIST_TABLE, 0, 4) == 8);
    CHECK(config.free_lists.ops.recover_targets(&backend, 5) == -2);
    return 0;
}

/* A private power callback can observe a newer timestamp than the backend.
 * A later backend reading must not regress even if its own last_now is older. */
unsigned backend_clock_check(void)
{
    for (unsigned mode = 1; mode <= 2; ++mode) {
        CHECK(!setup()); CHECK(!bind());
        CHECK(config.power.begin(&backend, operation_deadline));
        clock_regression = mode;
        CHECK(config.power.poll(&backend) == -1);
        CHECK(atomic_load(&backend.error) == A733_GPU_BACKEND_CLOCK && !mmio_count);
        CHECK(backend.phase == A733_GPU_BACKEND_POWERING && power.phase == A733_GPU_PLATFORM_ADMIT);
        CHECK(config.power.poll(&backend) == -1 && !mmio_count);
    }
    return 0;
}

unsigned backend_event_check(void)
{
    CHECK(!setup()); CHECK(!bind());
    CHECK(pvr_ccb_init(&service.queue, &config.queue, &backend, 1) == PVR_CCB_OK);
    CHECK(!enabled()); CHECK(config.start.publish(&backend));
    CHECK(pvr_ccb_start(&service.queue, 1) == PVR_CCB_OK);
    unsigned char expected[PVR_CCB_COMMAND_BYTES] = {0}, received[PVR_CCB_COMMAND_BYTES] = {0};
    uint32_t type = PVR_CCB_MAGIC | 103;
    memcpy(expected, &type, sizeof(type)); expected[8] = 42;
    memcpy(region(PVR_FW_EVENTS), expected, sizeof(expected));
    *(volatile uint32_t *)region(PVR_FW_EVENT_CONTROL) = 1;
    CHECK(pvr_ccb_receive(&service.queue, received) == PVR_CCB_OK);
    CHECK(!memcmp(received, expected, sizeof(expected)));
    CHECK(!memcmp(service.queue.last_event, expected, sizeof(expected)));
    CHECK(service.queue.firmware_read == 1 && *(volatile uint32_t *)((uintptr_t)region(PVR_FW_EVENT_CONTROL) + 128) == 1);
    CHECK(pvr_ccb_receive(&service.queue, received) == PVR_CCB_AGAIN && !atomic_load(&backend.error));
    for (unsigned mode = 0; mode < 4; ++mode) {
        CHECK(!setup()); CHECK(!bind());
        CHECK(pvr_ccb_init(&service.queue, &config.queue, &backend, 1) == PVR_CCB_OK);
        CHECK(pvr_ccb_start(&service.queue, 1) == PVR_CCB_OK);
        enum pvr_ccb_region r = mode == 0 ? PVR_CCB_HOST_COMMANDS : PVR_CCB_FIRMWARE_COMMANDS;
        uint32_t off = mode == 1 ? PVR_CCB_COMMAND_BYTES : 0;
        void *out = service.queue.last_event + (mode == 2 ? 1 : 0);
        size_t bytes = PVR_CCB_COMMAND_BYTES - (mode == 3 ? 1 : 0);
        CHECK(!config.queue.read(&backend, r, off, out, bytes));
        CHECK(atomic_load(&backend.error) == A733_GPU_BACKEND_ACCESS && !mmio_count);
        for (unsigned i = 0; i < PVR_CCB_COMMAND_BYTES; ++i) CHECK(service.queue.last_event[i] == 0);
    }
    return 0;
}
