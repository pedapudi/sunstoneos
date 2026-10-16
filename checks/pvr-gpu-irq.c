/* Reuse the established software firmware model and allocation fixture. */
#define kernel_main gpu_poll_fixture_main
#include "pvr-gpu-service.c"
#undef kernel_main
#include "gicv3_spi.h"

#define TEST_INTID 95u
static const struct gicv3_spi spi = {0x08000000, TEST_INTID};
static uint32_t interrupt_count, interrupt_cores, mask_calls, rearm_calls;
static uint32_t defer_rearm, deferred_seen, reject_rearm;
static uint32_t event_batches, irq_failures;
extern void gpu_irq_base_handler(uint64_t);
static volatile uint32_t *gic(unsigned offset)
{ return (volatile uint32_t *)(uintptr_t)(0x08000000u + offset); }
static int irq_mask(void *ctx)
{
    configASSERT(ctx == &config);
    __atomic_fetch_add(&mask_calls, 1, __ATOMIC_RELEASE);
    return gicv3_spi_mask(&spi);
}
static int irq_rearm(void *ctx)
{
    configASSERT(ctx == &config && xTaskGetCurrentTaskHandle() == service.worker);
    __atomic_fetch_add(&rearm_calls, 1, __ATOMIC_RELEASE);
    if (get(&reject_rearm)) return -1;
    if (get(&defer_rearm)) { __atomic_fetch_add(&deferred_seen, 1, __ATOMIC_RELEASE); return 0; }
    /* A drained control batch must acknowledge every published firmware event. */
    configASSERT(get((uint32_t *)resource(3)) == get((uint32_t *)(resource(3) + 128)));
    return gicv3_spi_rearm(&spi);
}
void vApplicationIRQHandler(uint64_t interrupt)
{
    if (interrupt != TEST_INTID) { gpu_irq_base_handler(interrupt); return; }
    __atomic_fetch_add(&interrupt_count, 1, __ATOMIC_RELEASE);
    __atomic_fetch_or(&interrupt_cores, 1u << portGET_CORE_ID(), __ATOMIC_RELEASE);
    BaseType_t woken = pdFALSE;
    if (!pvr_gpu_service_interrupt(&service, &woken))
        __atomic_fetch_add(&irq_failures, 1, __ATOMIC_RELEASE);
    portYIELD_FROM_ISR(woken);
}
static void assert_masked(void)
{ configASSERT(!(*gic(0x100 + 4 * (TEST_INTID / 32)) & (1u << (TEST_INTID % 32)))); }
static void pend(void)
{
    *gic(0x200 + 4 * (TEST_INTID / 32)) = 1u << (TEST_INTID % 32);
    __asm__ volatile("dsb sy" ::: "memory");
}
static void publish_events(unsigned count)
{
    unsigned start = get((uint32_t *)resource(3));
    for (unsigned i = 0; i < count; ++i) {
        unsigned char *command = resource(4) + ((start + i) & 31) * 88;
        memset(command, 0, 88); put(command, 4, PVR_CCB_MAGIC | 101); put(command + 8, 4, 0x1234);
    }
    set((uint32_t *)resource(3), (start + count) & 31);
    __atomic_fetch_add(&event_batches, 1, __ATOMIC_RELEASE);
}
static void wait_at_least(const uint32_t *p, unsigned n)
{
    TickType_t begin = xTaskGetTickCount();
    while (get(p) < n) { configASSERT(xTaskGetTickCount() - begin < 1000); vTaskDelay(1); }
}
static void suspend_armed_worker(void)
{
    TickType_t begin = xTaskGetTickCount();
    for (;;) {
        vTaskSuspend(service.worker);
        if (!service.irq_masked && !service.irq_wake_pending && !uxQueueMessagesWaiting(service.requests)) return;
        vTaskResume(service.worker);
        configASSERT(xTaskGetTickCount() - begin < 100);
        vTaskDelay(1);
    }
}
static void irq_owner(void *arg)
{
    (void)arg;
    configASSERT(request(PVR_GPU_SERVICE_OPEN, 1000000).result == PVR_GPU_SERVICE_OK);
    configASSERT(wait_phase(PVR_GPU_SERVICE_READY).owns_session);
    /* IRQ routes to core 0; the supervisor moved the worker to core 1. */
    uint32_t before = get(&interrupt_count);
    pend(); wait_at_least(&interrupt_count, before + 1);
    configASSERT(get(&interrupt_cores) == 1 && get(&cores) == 3);
    /* Fill the request queue while the worker is suspended. A failed ISR
     * wake enqueue must retain the mask; queued messages already guarantee
     * progress after the worker resumes. Internal wake messages are fillers. */
    suspend_armed_worker();
    const struct pvr_gpu_service_message filler = {.client = PVR_GPU_SERVICE_CLIENTS};
    while (xQueueSend(service.requests, &filler, 0) == pdTRUE) {}
    before = get(&interrupt_count); pend(); wait_at_least(&interrupt_count, before + 1);
    assert_masked(); configASSERT(!service.irq_wake_pending); vTaskResume(service.worker);
    /* Hold rearm temporarily. A firmware batch larger than the worker budget
     * must drain before enabling the GIC. Additional pending interrupts survive. */
    set(&defer_rearm, 1); wait_at_least(&deferred_seen, 1); assert_masked();
    before = get(&interrupt_count);
    publish_events(9); pend();
    wait_at_least(&handled, 9); assert_masked();
    configASSERT(get(&interrupt_count) == before);
    set(&defer_rearm, 0); wait_at_least(&interrupt_count, before + 1);
    configASSERT(get(&handled) == 9);
    configASSERT(request(PVR_GPU_SERVICE_CLOSE, 1000000).result == PVR_GPU_SERVICE_OK);
    configASSERT(!wait_phase(PVR_GPU_SERVICE_STOPPED).owns_session); assert_masked();
    /* A stale pending IRQ stays disabled through stop, then reaches the same
     * dispatcher after reopen without reading retired firmware resources. */
    before = get(&interrupt_count); pend(); vTaskDelay(2);
    configASSERT(get(&interrupt_count) == before);
    configASSERT(request(PVR_GPU_SERVICE_OPEN, 1000000).result == PVR_GPU_SERVICE_OK);
    configASSERT(wait_phase(PVR_GPU_SERVICE_READY).epoch == 2);
    wait_at_least(&interrupt_count, before + 1);
    if (gpu_service_case == 1) set(&reject_rearm, 1);
    else { configASSERT(gpu_service_case == 2); set(&defer_rearm, 1); }
    struct pvr_gpu_service_reply r = wait_phase(PVR_GPU_SERVICE_FAILED);
    configASSERT(r.error == PVR_GPU_SERVICE_INTERRUPT && r.owns_session); assert_masked();
    unsigned saved_calls = get(&calls), saved_mask = get(&mask_calls), saved_rearm = get(&rearm_calls);
    vTaskDelay(4); configASSERT(request(PVR_GPU_SERVICE_OPEN, 100000).result == PVR_GPU_SERVICE_FAULT);
    configASSERT(saved_calls == get(&calls) && saved_mask == get(&mask_calls) && saved_rearm == get(&rearm_calls));
    configASSERT(!get(&irq_failures) && get(&event_batches) == 1);
    if (gpu_service_case == 2) configASSERT(service.irq_deferred == PVR_GPU_SERVICE_IRQ_REARM_ATTEMPTS);
    platform_puts("PASS GPU IRQ: case "); platform_hex(gpu_service_case);
    platform_puts("; cross-core GIC delivery, queue saturation, masked backlog, deferred rearm, reopen, retained fault\n");
    platform_exit(0);
}
void kernel_main(void)
{
    platform_init(); memset(arena, 0xa5, sizeof(arena));
    const unsigned bank = TEST_INTID / 32, bit = 1u << (TEST_INTID % 32);
    configASSERT(gicv3_spi_mask(&spi)); assert_masked();
    *gic(0x280 + 4 * bank) = bit;
    *gic(0x80 + 4 * bank) |= bit;
    *gic(0xc00 + 4 * (TEST_INTID / 16)) &= ~(3u << (2 * (TEST_INTID % 16)));
    *(volatile uint8_t *)(uintptr_t)(0x08000400 + TEST_INTID) = portLOWEST_USABLE_INTERRUPT_PRIORITY << portPRIORITY_SHIFT;
    *(volatile uint64_t *)(uintptr_t)(0x08006000 + 8 * TEST_INTID) = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    struct gicv3_spi invalid = {0, 95};
    configASSERT(!gicv3_spi_mask(&invalid) && gicv3_spi_rearm(&invalid) == -1);
    invalid = (struct gicv3_spi){0x08000000, 31};
    configASSERT(!gicv3_spi_mask(&invalid) && gicv3_spi_rearm(&invalid) == -1);
    /* This GIC implements fewer SPIs: its unimplemented enable bit reads zero.
     * Exercise refusal and the adapter's subsequent disable command. */
    configASSERT(((*gic(4) & 31) + 1) * 32 <= 1000);
    invalid = (struct gicv3_spi){0x08000000, 1000};
    configASSERT(gicv3_spi_rearm(&invalid) == -1);
    config.firmware = gpu_firmware; config.firmware_bytes = (size_t)(gpu_firmware_end - gpu_firmware);
    config.state = (struct pvr_fw_state_config){PVR_FW_A733_BVNC, 123456789, 1, 16};
    config.queue = (struct pvr_ccb_ops){read_mem, write_mem, barrier, kick};
    config.start = (struct pvr_gpu_start_ops){now, read_reg, write_reg, started, publish};
    config.control = (struct pvr_gpu_control_ops){now, irq_read, irq_write, compat, event};
    config.stop = (struct pvr_gpu_stop_ops){now, read_reg, write_reg, sync_write, sync_read, isolate};
    config.enable = enable; config.context = &config;
    unsigned byte = 0, page = 0;
    for (unsigned i = 0; i < 22; ++i) {
        unsigned n = i < 2 ? (i ? 20480 : 102400) : i == 2 ? 16384 :
            pvr_fw_state_layout((enum pvr_fw_resource)(i - 3))->allocation_bytes;
        struct pvr_mips_buffer *b = i < 3 ? &config.firmware_buffers[i] : &config.resources[i - 3];
        *b = (struct pvr_mips_buffer){{arena + byte, n}, pages + page, n / 4096};
        for (unsigned p = 0; p < n / 4096; ++p) pages[page++] = (uintptr_t)(arena + byte + p * 4096);
        guards[i] = arena + byte + n; byte += n + 4096;
    }
    config.irq.mask = irq_mask;
    configASSERT(!pvr_gpu_service_init(&service, &config, 4, 1));
    config.irq.rearm = irq_rearm;
    configASSERT(pvr_gpu_service_init(&service, &config, 4, 1));
    TaskHandle_t client = xTaskCreateStaticAffinitySet(irq_owner, "irq-owner", 4096, NULL, 5, stacks[0], &tasks[0], 1);
    configASSERT(client && pvr_gpu_service_bind(&service, 0, client));
    configASSERT(xTaskCreateStaticAffinitySet(supervisor, "supervisor", 4096, NULL, 6, stacks[2], &tasks[2], 2));
    vTaskStartScheduler(); platform_panic("scheduler returned", 0);
}
