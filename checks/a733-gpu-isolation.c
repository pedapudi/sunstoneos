/* Check only the isolation-provider result boundary. Power, firmware and
 * physical block/drain are modeled; no hardware isolation is established. */
#include "a733-gpu-backend.c"
static int isolation_result;
static int partial_isolation(void *context, struct a733_gpu_platform *p, uint64_t deadline)
{
    if (context != &provider_calls || p != &power || deadline != operation_deadline) return 0;
    ++provider_calls;
    /* An uncertain provider may already have released its power ownership. */
    p->phase = A733_GPU_PLATFORM_OFF; p->token = 0; p->delegated = 0;
    power_enabled = identity_confirmed = 0;
    return isolation_result;
}
unsigned backend_isolation_check(void)
{
    const int results[] = {-1, 2, 1};
    for (unsigned i = 0; i < sizeof(results)/sizeof(*results); ++i) {
        CHECK(!setup()); CHECK(!bind()); CHECK(!enabled());
        CHECK(config.start.publish(&backend));
        CHECK(memory.phase == A733_GPU_MEMORY_PUBLISHED);
        backend.external.isolate = partial_isolation;
        service.stop.phase = PVR_GPU_STOP_ISOLATE;
        service.stop.deadline = operation_deadline;
        isolation_result = results[i];
        CHECK(config.stop.isolate(&backend) == (results[i] == 1));
        CHECK(provider_calls == 1 && power.phase == A733_GPU_PLATFORM_OFF && !power.token);
        if (results[i] != 1) {
            CHECK(atomic_load(&backend.error) == A733_GPU_BACKEND_ISOLATION);
            CHECK(memory.phase == A733_GPU_MEMORY_PUBLISHED);
            CHECK(backend.phase == A733_GPU_BACKEND_ACTIVE);
            unsigned accesses_before = mmio_count;
            CHECK(!config.stop.isolate(&backend));
            CHECK(provider_calls == 1 && mmio_count == accesses_before);
        } else {
            CHECK(!atomic_load(&backend.error));
            CHECK(memory.phase == A733_GPU_MEMORY_OFFLINE);
            CHECK(backend.phase == A733_GPU_BACKEND_ISOLATED);
        }
    }
    return 0;
}
