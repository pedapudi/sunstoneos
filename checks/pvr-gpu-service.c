#include "pvr_gpu_service.h"
#include "platform.h"
#include <string.h>
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
#include "a733_gpu_memory.h"
#include "aarch64_device_memory.h"
static struct a733_gpu_memory memory, refused;
static uint64_t *fixture_level2;
#endif

extern const unsigned char gpu_firmware[], gpu_firmware_end[];
/* The runner changes this initialized fixture word without recompiling code. */
volatile uint32_t gpu_service_case = 1;
static struct pvr_gpu_service service;
static struct pvr_gpu_service_config config;
static _Alignas(4096) unsigned char arena[101 * 4096];
static uint64_t pages[79];
static unsigned char *guards[22];
static StaticTask_t tasks[3];
static StackType_t stacks[3][4096];
static uint32_t stage, peer_done, publish_waiting, publish_release, fault_mode;
static uint32_t calls, published, enabled, isolated, handled, cores;
static unsigned char registers[0xa108];
static unsigned running, stopping, first_publish = 1;

static uint32_t get(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void set(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint64_t word(const void *ptr, unsigned bytes)
{ const unsigned char *p = ptr; uint64_t v = 0; for (unsigned i=0;i<bytes;++i) v |= (uint64_t)p[i]<<(8*i); return v; }
static void put(void *ptr, unsigned bytes, uint64_t value)
{ unsigned char *p = ptr; for (unsigned i=0;i<bytes;++i) p[i]=(unsigned char)(value>>(8*i)); }
static unsigned char *resource(unsigned id)
{ configASSERT(id < PVR_FW_RESOURCE_COUNT); return config.resources[id].storage.cpu; }
static uint64_t level(void)
{ register uint64_t result __asm__("x0"); __asm__ volatile("svc #120" : "=r"(result) :: "memory"); return result; }
static void callback(void *ctx)
{
    uint64_t el, sp;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, spsel" : "=r"(el), "=r"(sp));
    configASSERT(ctx == &config && el == 4 && sp == 0 && xTaskGetCurrentTaskHandle() == service.worker);
    __atomic_fetch_add(&calls, 1, __ATOMIC_RELEASE);
    __atomic_fetch_or(&cores, 1u << portGET_CORE_ID(), __ATOMIC_RELEASE);
}
static int now(void *ctx, uint64_t *out)
{
    callback(ctx); uint64_t counter, frequency;
    __asm__ volatile("mrs %0, cntpct_el0\n mrs %1, cntfrq_el0" : "=r"(counter), "=r"(frequency));
    *out = (counter / frequency) * 1000000 + (counter % frequency) * 1000000 / frequency;
    return 1;
}
static int read_reg(void *ctx, uint32_t off, uint32_t width, uint64_t *out)
{
    callback(ctx); configASSERT(off + width <= sizeof(registers));
    *out = word(registers + off, width);
    if (stopping && off != 0x100) *out = off == 0x3c8 ? 0x7f : off == 0x3898 ? 0xff : 0;
    return 1;
}
static int write_reg(void *ctx, uint32_t off, uint32_t width, uint64_t value)
{
    callback(ctx); configASSERT(get(&enabled) && off + width <= sizeof(registers));
    put(registers + off, width, value);
    if (off == 0x100 && !value) running = 1;
    return 1;
}
static int enable(void *ctx)
{
    callback(ctx); configASSERT(!running && (!get(&enabled) || get(&isolated)));
    memset(registers, 0, sizeof(registers)); stopping = 0;
    __atomic_fetch_add(&enabled, 1, __ATOMIC_RELEASE); return 1;
}
static int publish(void *ctx)
{
    callback(ctx);
    configASSERT(word(resource(PVR_FW_OS_INIT) + 40, 4) == 3);
    configASSERT(word(resource(PVR_FW_SYSTEM_INIT) + 344, 4) == config.state.gpu_clock_hz);
    configASSERT(!resource(PVR_FW_SYSTEM_INIT)[352]);
    configASSERT(word(resource(PVR_FW_HOST_CONTROL) + 132, 4) == 127);
    configASSERT(word(config.firmware_buffers[2].storage.cpu, 4));
    if (first_publish) {
        first_publish = 0; set(&publish_waiting, 1);
        TickType_t start = xTaskGetTickCount();
        while (!get(&publish_release)) { configASSERT(xTaskGetTickCount() - start < 500); vTaskDelay(1); }
        callback(ctx); configASSERT(portGET_CORE_ID() == 1);
    }
    __asm__ volatile("dsb sy" ::: "memory");
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    configASSERT(a733_gpu_memory_publish(&memory));
#endif
    __atomic_fetch_add(&published, 1, __ATOMIC_RELEASE); return 1;
}
static int started(void *ctx, uint32_t *out)
{
    callback(ctx); *out = 0;
    if (running && get(&fault_mode) != 1) {
        resource(PVR_FW_SYSTEM_INIT)[352] = 1; *out = 1;
        unsigned char *b = resource(PVR_FW_OS_INIT) + 40;
        put(b + 8, 8, PVR_FW_A733_BVNC); put(b + 24, 8, PVR_FW_A733_BVNC); b[56] = 1;
    }
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    return a733_gpu_memory_started(&memory, out);
#else
    return 1;
#endif
}
static int read_mem(void *ctx, enum pvr_ccb_region region, uint32_t off, void *out, size_t n)
{
    callback(ctx); configASSERT(off <= pvr_ccb_region_bytes(region) && n <= pvr_ccb_region_bytes(region) - off);
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    return a733_gpu_memory_read(&memory, region, off, out, n);
#else
    if (n == 4) { uint32_t v = get((uint32_t *)(resource(region) + off)); memcpy(out, &v, 4); }
    else memcpy(out, resource(region) + off, n);
    return 1;
#endif
}
static int write_mem(void *ctx, enum pvr_ccb_region region, uint32_t off, const void *in, size_t n)
{
    callback(ctx); configASSERT(off <= pvr_ccb_region_bytes(region) && n <= pvr_ccb_region_bytes(region) - off);
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    return a733_gpu_memory_write(&memory, region, off, in, n);
#else
    memcpy(resource(region) + off, in, n); return 1;
#endif
}
static int barrier(void *ctx, enum pvr_ccb_barrier kind)
{
    callback(ctx);
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    return a733_gpu_memory_barrier(&memory, kind);
#else
    (void)kind; __asm__ volatile("dsb sy" ::: "memory"); return 1;
#endif
}
static int kick(void *ctx, uint32_t off, uint32_t value)
{
    callback(ctx); configASSERT(running && off == 0xb00 && !value);
    unsigned next = (unsigned)word(resource(0), 4), slot = (next - 1) & 127;
    unsigned char *command = resource(1) + slot * 88;
    uint32_t type = (uint32_t)word(command, 4);
    configASSERT(type == PVR_CCB_HEALTH || type == (PVR_CCB_MAGIC | 107));
    if (type != PVR_CCB_HEALTH) {
        configASSERT(!word(resource(PVR_FW_POWER_SYNC), 4));
        if (get(&fault_mode) != 2) put(resource(PVR_FW_POWER_SYNC), 4, 1);
        if (word(command + 8, 4) == 1) stopping = 1;
    }
    put(resource(0) + 128, 4, next); put(resource(2) + slot * 4, 4, 1); return 1;
}
static int irq_read(void *ctx, uint32_t off, uint32_t *out)
{
    callback(ctx); configASSERT(off == 0x8a8);
    if (get(&fault_mode) == 3) return 0;
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    if (get(&fault_mode) == 4) {
        /* Deliberately violate pinned writable mappings. This is a fault fixture,
         * not a supported live remap. Keep NC attributes; no cached alias exists. */
        fixture_level2[(0x46000000 - 0x40000000) / 0x200000] |= 1u << 7;
        configASSERT(aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)fixture_level2, 4096));
        __asm__ volatile("dsb sy\n tlbi vmalle1is\n dsb sy\n isb" ::: "memory");
        *out = 0; return 1;
    }
#endif
    *out = get((uint32_t *)resource(3)) != get((uint32_t *)(resource(3) + 128)); return 1;
}
static int irq_write(void *ctx, uint32_t off, uint32_t value)
{ callback(ctx); configASSERT(off == 0x8b0 && value == 1); return 1; }
static int compat(void *ctx, uint32_t off, void *out, size_t n)
{
    callback(ctx); configASSERT(off <= 64 && n <= 64 - off);
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    return a733_gpu_memory_compat(&memory, off, out, n);
#else
    memcpy(out, resource(PVR_FW_OS_INIT) + 40 + off, n); return 1;
#endif
}
static int event(void *ctx, const unsigned char command[88])
{
    callback(ctx); configASSERT(word(command, 4) == (PVR_CCB_MAGIC | 101));
    configASSERT(word(command + 8, 4) == 0x1234);
    __atomic_fetch_add(&handled, 1, __ATOMIC_RELEASE); return 1;
}
static int sync_write(void *ctx, uint32_t value)
{
    callback(ctx);
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    return a733_gpu_memory_sync_write(&memory, value);
#else
    put(resource(PVR_FW_POWER_SYNC), 4, value); return 1;
#endif
}
static int sync_read(void *ctx, uint32_t *out)
{
    callback(ctx);
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    return a733_gpu_memory_sync_read(&memory, out);
#else
    *out = (uint32_t)word(resource(PVR_FW_POWER_SYNC), 4); return 1;
#endif
}
static int isolate(void *ctx)
{
    callback(ctx); configASSERT(stopping && service.queue.phase == PVR_CCB_RETIRED);
    configASSERT(word(registers + 0x100, 8) == UINT64_C(0xffeffffffffffc3d));
    running = 0; __atomic_fetch_add(&isolated, 1, __ATOMIC_RELEASE);
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    return a733_gpu_memory_reclaim(&memory);
#else
    return 1;
#endif
}
static struct pvr_gpu_service_reply request(enum pvr_gpu_service_operation op, uint64_t us)
{
    struct pvr_gpu_service_request r = {op, us}; struct pvr_gpu_service_reply reply; uint64_t ticket;
    configASSERT(level() == 0 && pvr_gpu_service_send(&service, &r, 100, &ticket));
    configASSERT(pvr_gpu_service_collect(&service, ticket, &reply, 1000) && level() == 0);
    return reply;
}
static struct pvr_gpu_service_reply wait_phase(enum pvr_gpu_service_phase phase)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        struct pvr_gpu_service_reply r = request(PVR_GPU_SERVICE_STATUS, 0);
        if (r.phase == phase) return r;
        configASSERT(r.phase != PVR_GPU_SERVICE_FAILED && xTaskGetTickCount() - start < 1000);
        vTaskDelay(1);
    }
}
static void wait_flag(const uint32_t *p, uint32_t value)
{
    TickType_t start = xTaskGetTickCount();
    while (get(p) != value) { configASSERT(xTaskGetTickCount() - start < 2000); vTaskDelay(1); }
}
static void supervisor(void *arg)
{
    (void)arg; uint64_t ticket = 42; struct pvr_gpu_service_request r = {PVR_GPU_SERVICE_STATUS, 0};
    configASSERT(level() == 0 && !pvr_gpu_service_send(&service, &r, 0, &ticket) && ticket == 42);
    wait_flag(&publish_waiting, 1);
    vTaskCoreAffinitySet(service.worker, 2); set(&publish_release, 1);
    vTaskSuspend(NULL);
}
static void peer(void *arg)
{
    (void)arg; wait_flag(&stage, 1);
    configASSERT(!request(PVR_GPU_SERVICE_STATUS, 0).owns_session);
    configASSERT(request(PVR_GPU_SERVICE_CLOSE, 100000).result == PVR_GPU_SERVICE_DENIED);
    configASSERT(request(PVR_GPU_SERVICE_OPEN, 100000).result == PVR_GPU_SERVICE_BUSY);
    set(&peer_done, 1);
    if (gpu_service_case != 1) vTaskSuspend(NULL);
    wait_flag(&stage, 2);
    configASSERT(request(PVR_GPU_SERVICE_OPEN, 1000000).result == PVR_GPU_SERVICE_OK);
    struct pvr_gpu_service_reply r = wait_phase(PVR_GPU_SERVICE_READY);
    configASSERT(r.owns_session && r.epoch == 2);
    configASSERT(request(PVR_GPU_SERVICE_CLOSE, 1000000).result == PVR_GPU_SERVICE_OK);
    configASSERT(!wait_phase(PVR_GPU_SERVICE_STOPPED).owns_session);
    set(&peer_done, 2); vTaskSuspend(NULL);
}
static void owner(void *arg)
{
    (void)arg;
    struct pvr_gpu_service_request r = {PVR_GPU_SERVICE_STATUS, 0};
    struct pvr_gpu_service_reply reply = {.ticket = 42}; uint64_t ticket, ignored = 99;
    configASSERT(!pvr_gpu_service_send(&service, &r, portMAX_DELAY, &ignored));
    configASSERT(pvr_gpu_service_send(&service, &r, 0, &ticket));
    configASSERT(!pvr_gpu_service_collect(&service, ticket, &reply, 0) && reply.ticket == 42);
    configASSERT(!pvr_gpu_service_collect(&service, ticket + 1, &reply, 0));
    configASSERT(!pvr_gpu_service_send(&service, &r, 0, &ignored) && ignored == 99);
    r.operation = PVR_GPU_SERVICE_OPEN;
    configASSERT(pvr_gpu_service_collect(&service, ticket, &reply, 1000));
    configASSERT(reply.phase == PVR_GPU_SERVICE_COLD && !reply.owns_session);
    configASSERT(!pvr_gpu_service_collect(&service, ticket, &reply, 0));
    configASSERT(request((enum pvr_gpu_service_operation)-1, 0).result == PVR_GPU_SERVICE_INVALID);
    configASSERT(request(PVR_GPU_SERVICE_OPEN, 0).result == PVR_GPU_SERVICE_INVALID);
    configASSERT(request(PVR_GPU_SERVICE_CLOSE, 10000).result == PVR_GPU_SERVICE_DENIED);
    configASSERT(request(PVR_GPU_SERVICE_OPEN, 1000000).result == PVR_GPU_SERVICE_OK);
    set(&stage, 1); wait_flag(&peer_done, 1);
    configASSERT(wait_phase(PVR_GPU_SERVICE_READY).owns_session);
    if (gpu_service_case == 1) {
        /* Firmware model publishes one known event. Worker polls without a
         * request; release/acquire on the cursor makes payload publication safe. */
        unsigned char *command = resource(PVR_FW_EVENTS);
        put(command, 4, PVR_CCB_MAGIC | 101); put(command + 8, 4, 0x1234);
        __atomic_store_n((uint32_t *)resource(PVR_FW_EVENT_CONTROL), 1, __ATOMIC_RELEASE);
        wait_flag(&handled, 1);
        configASSERT(request(PVR_GPU_SERVICE_CLOSE, 1000000).result == PVR_GPU_SERVICE_OK);
        configASSERT(!wait_phase(PVR_GPU_SERVICE_STOPPED).owns_session);
        configASSERT(get(&isolated) == 1); set(&stage, 2); wait_flag(&peer_done, 2);
        configASSERT(get(&isolated) == 2 && get(&published) == 2);
        set(&fault_mode, 1);
        configASSERT(request(PVR_GPU_SERVICE_OPEN, 100000).result == PVR_GPU_SERVICE_OK);
        vTaskDelay(150); /* Startup expires autonomously without STATUS requests. */
    } else if (gpu_service_case == 2) {
        set(&fault_mode, 2);
        configASSERT(request(PVR_GPU_SERVICE_CLOSE, 20000).result == PVR_GPU_SERVICE_OK);
        vTaskDelay(50); /* Missing power acknowledgment expires without a client. */
    } else {
        configASSERT(gpu_service_case == 3
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
            || gpu_service_case == 4
#endif
        );
        set(&fault_mode, gpu_service_case); vTaskDelay(10);
    }
    reply = request(PVR_GPU_SERVICE_STATUS, 0);
    configASSERT(reply.result == PVR_GPU_SERVICE_FAULT && reply.phase == PVR_GPU_SERVICE_FAILED && reply.owns_session);
    configASSERT(reply.error == (gpu_service_case == 1 ? PVR_GPU_SERVICE_START :
        gpu_service_case == 2 ? PVR_GPU_SERVICE_STOP : PVR_GPU_SERVICE_CONTROL));
    uint32_t count = get(&calls);
    configASSERT(request(PVR_GPU_SERVICE_CLOSE, 100000).result == PVR_GPU_SERVICE_FAULT);
    configASSERT(request(PVR_GPU_SERVICE_OPEN, 100000).result == PVR_GPU_SERVICE_FAULT);
    vTaskDelay(5); configASSERT(count == get(&calls));
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    configASSERT(memory.phase == (gpu_service_case == 4 ? A733_GPU_MEMORY_FAILED : A733_GPU_MEMORY_PUBLISHED));
#endif
    for (unsigned i=0;i<22;++i) for (unsigned j=0;j<4096;++j) configASSERT(guards[i][j] == 0xa5);
    configASSERT(get(&cores) == 3);
    platform_puts("PASS GPU service: case "); platform_hex(gpu_service_case);
    platform_puts("; EL0 ownership, EL1t worker, prepared firmware lifecycle, retained fault, intact guards\n");
    platform_exit(0);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{ (void)task; platform_panic(name, 0); }
void kernel_main(void)
{
    platform_init(); memset(arena, 0xa5, sizeof(arena));
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    uint64_t ttbr;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
    uint64_t *root = (void *)(uintptr_t)(ttbr & UINT64_C(0x000ffffffffff000));
    fixture_level2 = (void *)(uintptr_t)(root[1] & UINT64_C(0x000ffffffffff000));
    memset((void *)(uintptr_t)0x46000000, 0xa5, 0x200000);
#endif
    char move[8] = "abcdefg";
    configASSERT(memmove(move + 1, move, 6) == move + 1 && !memcmp(move, "aabcdef", 8));
    configASSERT(memmove(move, move + 1, 6) == move && !memcmp(move, "abcdeff", 8));
    configASSERT(memmove(move, move, 8) == move && memmove(move, move + 1, 0) == move);
    config.firmware = gpu_firmware; config.firmware_bytes = (size_t)(gpu_firmware_end - gpu_firmware);
    config.state = (struct pvr_fw_state_config){PVR_FW_A733_BVNC, 123456789, 1, 16};
    config.queue = (struct pvr_ccb_ops){read_mem, write_mem, barrier, kick};
    config.start = (struct pvr_gpu_start_ops){now, read_reg, write_reg, started, publish};
    config.control = (struct pvr_gpu_control_ops){now, irq_read, irq_write, compat, event};
    config.stop = (struct pvr_gpu_stop_ops){now, read_reg, write_reg, sync_write, sync_read, isolate};
    config.enable = enable; config.context = &config;
    unsigned byte = 0, page = 0;
    for (unsigned i=0;i<22;++i) {
        unsigned n = i < 2 ? (i ? 20480 : 102400) : i == 2 ? 16384 :
            pvr_fw_state_layout((enum pvr_fw_resource)(i - 3))->allocation_bytes;
        struct pvr_mips_buffer *b = i < 3 ? &config.firmware_buffers[i] : &config.resources[i - 3];
        unsigned char *cpu = arena + byte;
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
        if (i >= 3) cpu = (void *)(uintptr_t)(0x46000000 + byte);
#endif
        *b = (struct pvr_mips_buffer){{cpu, n}, pages + page, n/4096};
        for (unsigned p=0;p<n/4096;++p) pages[page++] = (uintptr_t)(cpu + p*4096);
        guards[i] = cpu + n; byte += n + 4096;
    }
    configASSERT(page == 79 && byte == sizeof(arena));
    struct pvr_mips_buffer saved = config.resources[0];
    config.resources[0].storage.cpu = config.firmware_buffers[0].storage.cpu;
    configASSERT(!pvr_gpu_service_init(&service, &config, 4, 1) && !service.initialized && !get(&calls));
    config.resources[0] = saved;
    uint64_t pa = pages[34]; pages[34] = pages[0];
    configASSERT(!pvr_gpu_service_init(&service, &config, 4, 1) && !service.initialized && !get(&calls));
    pages[34] = pa;
#ifdef SUNSTONE_GPU_NATIVE_MEMORY
    /* Cached shared memory, wrong PA, read-only NC and absent mappings must be
     * refused before any resource or adapter mutation. */
    const uintptr_t invalid_cpu[] = {(uintptr_t)arena + 0x60000, (uintptr_t)saved.storage.cpu,
                                     0x47000000, 0x48000000};
    for (unsigned i = 0; i < 4; ++i) {
        config.resources[0].storage.cpu = (void *)invalid_cpu[i];
        pages[34] = i == 1 ? pa + 4096 : invalid_cpu[i];
        configASSERT(!a733_gpu_memory_init(&refused, config.firmware_buffers, config.resources));
        for (unsigned j = 0; j < sizeof(refused); ++j) configASSERT(!((unsigned char *)&refused)[j]);
    }
    config.resources[0] = saved; pages[34] = pa;
    configASSERT(a733_gpu_memory_init(&memory, config.firmware_buffers, config.resources));
    configASSERT(memory.phase == A733_GPU_MEMORY_OFFLINE);
#endif
    configASSERT(pvr_gpu_service_init(&service, &config, 4, 1) && !get(&calls));
    TaskHandle_t a = xTaskCreateStaticAffinitySet(owner, "owner", 4096, NULL, 5, stacks[0], &tasks[0], 1);
    TaskHandle_t b = xTaskCreateStaticAffinitySet(peer, "peer", 4096, NULL, 5, stacks[1], &tasks[1], 2);
    configASSERT(a && b && !pvr_gpu_service_bind(&service, 0, service.worker));
    configASSERT(pvr_gpu_service_bind(&service, 0, a) && !pvr_gpu_service_bind(&service, 1, a));
    configASSERT(pvr_gpu_service_bind(&service, 1, b));
    configASSERT(xTaskCreateStaticAffinitySet(supervisor, "supervisor", 4096, NULL, 6, stacks[2], &tasks[2], 2));
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
