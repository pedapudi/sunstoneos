#include "a733_npu_reserved.h"
#include "aarch64_device_memory.h"
#include "npu-convolution-data.h"
#include "platform.h"
#include <string.h>

static struct a733_npu_reserved session;
static struct a733_npu_startup absent_startup;
static StaticTask_t client_storage;
static StackType_t client_stack[configMINIMAL_STACK_SIZE];
static void unused_client(void *argument)
{ (void)argument; for (;;) __asm__ volatile("wfe"); }
BaseType_t npu_convolution_task_privileged(TaskFunction_t entry, void *argument)
{ return npu_service_task_privileged(entry, argument); }
static unsigned char model[NPU_CONVOLUTION_MODEL_BYTES];
volatile unsigned npu_reserved_case = 0xfeed;
static unsigned char guard[4096] __attribute__((aligned(4096)));
extern void platform_exit_privileged(unsigned) __attribute__((noreturn));

/* RAM translation/cache instructions are real. The register address is a QEMU
 * Device page whose observed PA is substituted for native admission only.
 * The check issues no NPU MMIO and never reports physical startup readiness. */
int __real_aarch64_device_translate(uintptr_t, int, struct aarch64_translation *);
int __wrap_aarch64_device_translate(uintptr_t address, int write, struct aarch64_translation *out)
{
    int result = __real_aarch64_device_translate(address, write, out);
    if (result && address >= 0x09000000 && address < 0x09001000)
        out->physical = address - 0x09000000 + 0x03600000;
    return result;
}
static void fatal(void *cookie)
{ (void)cookie; platform_panic("unexpected native fault", 0); }

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }

void kernel_main(void)
{
    platform_init();
    configASSERT(npu_reserved_case <= 2);
    memset(guard, 0x7b, sizeof(guard));
    memcpy(model, fixture_model, sizeof(model));
    struct device_memory_pool *pool = a733_memory_reservations_init();
    configASSERT(pool && pool->backing.bytes == 2u * 1024u * 1024u);
    configASSERT(a733_memory_reservations_init() == pool);
    struct device_memory_lease blocker = {0};
    struct device_memory_extent blocked = {0};
    if (npu_reserved_case == 2)
        configASSERT(device_memory_acquire(pool, 99, 1024u * 1024u, &blocker, &blocked));
    if (npu_reserved_case == 1) model[20] ^= 1;
    struct npu_convolution *convolution = a733_npu_reserved_prepare(&session, 7,
        model, sizeof(model), 0x09000000, fatal, NULL);
    if (npu_reserved_case == 2) {
        configASSERT(!convolution && !session.admitted && !session.convolution.prepared);
        configASSERT(pool->next_generation == 2 && device_memory_release(pool, blocker));
        for (unsigned i = 0; i < DEVICE_MEMORY_MAX_ALLOCATIONS; ++i)
            configASSERT(pool->allocations[i].state == DEVICE_MEMORY_FREE);
        platform_puts("PASS reserved NPU: insufficient capacity leaves session and pool unmodified\n");
    } else {
        configASSERT(session.admitted && session.pool == pool && session.extent.bytes == A733_NPU_RESERVED_BYTES);
        configASSERT(session.lease.owner == 7 && session.lease.pool == (uintptr_t)pool);
        configASSERT(!device_memory_release(pool, session.lease));
        if (npu_reserved_case == 1) {
            configASSERT(!convolution && atomic_load(&session.convolution.failed));
            configASSERT(!session.convolution.native.initialized);
            configASSERT(device_memory_lookup(pool, session.lease)->state == DEVICE_MEMORY_RETAINED);
            configASSERT(!a733_npu_reserved_prepare(&session, 7, fixture_model,
                sizeof(fixture_model), 0x09000000, fatal, NULL));
            platform_puts("PASS reserved NPU: rejected model retains complete parent allocation\n");
        } else {
            configASSERT(convolution == &session.convolution && convolution->prepared);
            configASSERT(convolution->native.initialized && !convolution->bound);
            configASSERT(!memcmp(convolution->immutable_digest, fixture_immutable, sizeof(fixture_immutable)));
            memset(model, 0, sizeof(model));
            size_t offset = 0;
            for (unsigned i = 0; i < NPU_CONVOLUTION_BUFFERS; ++i) {
                const struct a733_npu_buffer *b = &convolution->buffers[i];
                configASSERT(b->cpu == session.extent.cpu + offset && b->physical == session.extent.physical + offset);
                configASSERT(!(b->bytes & 4095));
                offset += b->bytes;
            }
            configASSERT(convolution->native.memory.arena == session.extent.cpu + offset);
            configASSERT(convolution->native.memory.capacity == A733_NPU_RESERVED_TABLE_BYTES);
            configASSERT(offset + A733_NPU_RESERVED_TABLE_BYTES == session.extent.bytes);
            configASSERT(convolution->native.memory.image.bytes == A733_NPU_RESERVED_TABLE_BYTES);
            struct a733_npu_handoff base = {.command_admission = 1, .controller = 0x03600000}, handoff;
            configASSERT(npu_convolution_handoff(convolution, &base, &handoff));
            configASSERT(handoff.backend_cookie == convolution && handoff.command_count == 2);
            configASSERT(!handoff.established && !handoff.evidence);
            TaskHandle_t client = xTaskCreateStaticAffinitySet(unused_client, "reserved-client",
                configMINIMAL_STACK_SIZE, NULL, 1, client_stack, &client_storage, 1);
            configASSERT(client);
            configASSERT(!npu_convolution_bind(convolution, &absent_startup, client, 2, 1));
            configASSERT(!convolution->bound && !convolution->service.initialized);
            configASSERT(!device_memory_release(pool, session.lease));
            configASSERT(handoff.image.physical == convolution->native.memory.image.physical);
            /* Publication uses actual native cache operations. No device is
             * running; no completion/drain success is synthesized. */
            configASSERT(handoff.backend.publish(handoff.backend_cookie, &handoff.image, &handoff.commands[0], 1));
            configASSERT(convolution->native.memory.state == A733_NPU_MEMORY_DEVICE);
            configASSERT(!device_memory_release(pool, session.lease));
            configASSERT(device_memory_lookup(pool, session.lease)->state == DEVICE_MEMORY_DEVICE);
            platform_puts("PASS reserved NPU: authenticated native preparation, exact extents, handoff and cache publication\n");
        }
    }
    for (size_t i = 0; i < sizeof(guard); ++i) configASSERT(guard[i] == 0x7b);
    platform_exit_privileged(0);
}
