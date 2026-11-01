/* Real service/controller state machines, with modeled RAM-copy hardware.
 * No register callback accesses a physical DMA controller. */
#include "dma_service.h"
#include "a733_dma_regs.h"
#include "workload.h"
#include "platform.h"
#include <string.h>

#define DMA_BASE UINT32_C(0x04601000)
#define COPY_BYTES (64U * 1024U)
#define CHANNEL(r) (A733_DMA_REG_CHANNELS + (r))
static _Alignas(64) struct dma_service service;
static _Alignas(64) struct a733_dma_descriptor descriptors;
static _Alignas(64) uint8_t source[COPY_BYTES], destination[COPY_BYTES];
static atomic_uint regs[0x200 / 4], armed, irq_enabled, stop, done, jobs;
static StaticTask_t client_state;
static StackType_t client_stack[2048];
static TaskHandle_t client_task;

static uint32_t read_register(void *cookie, uintptr_t address)
{
    (void)cookie;
    configASSERT(address >= DMA_BASE && address < DMA_BASE + sizeof(regs));
    return atomic_load_explicit(&regs[(address - DMA_BASE) / 4], memory_order_acquire);
}
static void write_register(void *cookie, uintptr_t address, uint32_t value)
{
    (void)cookie;
    configASSERT(address >= DMA_BASE + A733_DMA_REG_CHANNELS && address < DMA_BASE + sizeof(regs));
    unsigned offset = (unsigned)(address - DMA_BASE);
    if (offset == CHANNEL(A733_DMA_REG_PENDING)) {
        atomic_fetch_and_explicit(&regs[offset / 4], ~value, memory_order_acq_rel);
    } else {
        atomic_store_explicit(&regs[offset / 4], value, memory_order_release);
    }
    if (offset != CHANNEL(A733_DMA_REG_ENABLE)) return;
    atomic_store_explicit(&regs[A733_DMA_REG_BUSY / 4], value ? 1 : 0, memory_order_release);
    if (value) {
        /* This CPU copy is the modeled DMA engine. The later periodic timer
         * publishes its completion interrupt; neither is board DMA evidence. */
        memcpy(destination, source, sizeof(source));
        atomic_store_explicit(&armed, 1, memory_order_release);
    } else atomic_store_explicit(&armed, 0, memory_order_release);
}
static void cache(void *cookie, enum a733_dma_cache operation, void *cpu, size_t bytes)
{
    (void)cookie; (void)cpu; (void)bytes;
    if (operation == A733_DMA_INVALIDATE)
        configASSERT(!atomic_load_explicit(&regs[A733_DMA_REG_BUSY / 4], memory_order_acquire));
}
static void barrier(void *cookie)
{ (void)cookie; atomic_thread_fence(memory_order_seq_cst); }
static uint64_t ticks(void *cookie)
{ (void)cookie; return ss_latency_now(); }
static int mapping(void *cookie, const struct a733_dma_mapping *m)
{
    (void)cookie;
    return m->bus == (uintptr_t)m->cpu &&
        ((m->cpu == &descriptors && m->bytes == sizeof(descriptors) && m->access == A733_DMA_DESCRIPTORS) ||
         (m->cpu == source && m->bytes == sizeof(source) && m->access == A733_DMA_READ) ||
         (m->cpu == destination && m->bytes == sizeof(destination) && m->access == A733_DMA_WRITE));
}
static int fifo(void *cookie, const struct a733_dma_endpoint *endpoint, enum a733_dma_access access)
{ (void)cookie; (void)endpoint; (void)access; return 0; }
static int rearm(void *cookie)
{
    configASSERT(cookie == &service);
    atomic_store_explicit(&irq_enabled, 1, memory_order_release);
    return 1;
}
void latency_dma_model_tick(void)
{
    if (!atomic_load_explicit(&armed, memory_order_acquire) ||
        !atomic_load_explicit(&irq_enabled, memory_order_acquire)) return;
    atomic_store_explicit(&irq_enabled, 0, memory_order_release);
    atomic_store_explicit(&armed, 0, memory_order_release);
    atomic_fetch_or_explicit(&regs[CHANNEL(A733_DMA_REG_PENDING) / 4], 4, memory_order_release);
    ss_latency_event_begin(SS_LAT_DMA);
    BaseType_t woken = pdFALSE;
    dma_service_irq_notify_from_isr(&service, 1, &woken);
    portYIELD_FROM_ISR(woken);
}
static struct dma_service_reply exchange(enum dma_service_operation operation)
{
    struct dma_service_request request = {.operation = operation, .job = 0,
        .timeout = operation == DMA_SERVICE_SUBMIT ? ss_latency_frequency / 2 : 0};
    struct dma_service_reply reply;
    uint64_t ticket;
    configASSERT(dma_service_send(&service, &request, pdMS_TO_TICKS(100), &ticket));
    configASSERT(dma_service_collect(&service, ticket, &reply, pdMS_TO_TICKS(100)));
    configASSERT(reply.result == DMA_SERVICE_OK);
    return reply;
}
static void client(void *argument)
{
    (void)argument;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    configASSERT(exchange(DMA_SERVICE_OPEN).phase == DMA_SERVICE_READY);
    while (!atomic_load_explicit(&stop, memory_order_acquire)) {
        unsigned generation = atomic_load_explicit(&jobs, memory_order_relaxed);
        for (unsigned i = 0; i < COPY_BYTES; ++i) source[i] = (uint8_t)(i ^ generation);
        configASSERT(exchange(DMA_SERVICE_SUBMIT).phase == DMA_SERVICE_RUNNING);
        TickType_t began = xTaskGetTickCount();
        struct dma_service_reply reply;
        do {
            vTaskDelay(1);
            if ((TickType_t)(xTaskGetTickCount() - began) >= pdMS_TO_TICKS(100)) {
                platform_puts("FAIL modeled DMA phase="); platform_hex(service.channels[0].phase);
                platform_puts(" armed="); platform_hex(atomic_load(&armed));
                platform_puts(" irq_enabled="); platform_hex(atomic_load(&irq_enabled));
                platform_puts(" pending="); platform_hex(atomic_load(&regs[CHANNEL(A733_DMA_REG_PENDING) / 4]));
                platform_puts(" busy="); platform_hex(atomic_load(&regs[A733_DMA_REG_BUSY / 4]));
                platform_puts(" events="); platform_hex(service.controller.events);
                platform_puts(" jobs="); platform_hex(atomic_load(&jobs));
                platform_puts("\n");
            }
            configASSERT((TickType_t)(xTaskGetTickCount() - began) < pdMS_TO_TICKS(100));
            reply = exchange(DMA_SERVICE_STATUS);
        } while (reply.phase != DMA_SERVICE_RESULT);
        configASSERT(reply.outcome == DMA_SERVICE_COMPLETE && reply.holds_buffers);
        configASSERT(!memcmp(source, destination, sizeof(source)));
        configASSERT(exchange(DMA_SERVICE_REAP).outcome == DMA_SERVICE_COMPLETE);
        atomic_fetch_add_explicit(&jobs, 1, memory_order_release);
    }
    configASSERT(exchange(DMA_SERVICE_CLOSE).phase == DMA_SERVICE_CLOSED);
    atomic_store_explicit(&done, 1, memory_order_release);
    vTaskSuspend(NULL);
    configASSERT(0);
}
void latency_dma_model_init(void)
{
    atomic_store(&regs[A733_DMA_REG_CPU_ROUTE / 4], 1);
    atomic_store(&regs[CHANNEL(A733_DMA_REG_DESCRIPTOR) / 4], 0x41000000);
    const struct a733_dma_operations operations = {read_register, write_register, cache,
        barrier, ticks, mapping, fifo};
    struct a733_dma_segment segment = {{(uintptr_t)source, 0, 4, 8},
        {(uintptr_t)destination, 0, 4, 8}, COPY_BYTES};
    struct a733_dma_mapping maps[] = {{source, (uintptr_t)source, sizeof(source), A733_DMA_READ},
        {destination, (uintptr_t)destination, sizeof(destination), A733_DMA_WRITE}};
    struct dma_service_job job = {.kind = DMA_SERVICE_RAM, .channel = 0,
        .transfer = {{&descriptors, (uintptr_t)&descriptors, sizeof(descriptors), A733_DMA_DESCRIPTORS},
            &segment, 1, maps, 2}};
    struct dma_service_protected protected = {(uintptr_t)&service, (uintptr_t)&service, sizeof(service)};
    configASSERT(dma_service_init(&service, &operations, NULL, DMA_BASE, 1,
        ss_latency_frequency / 100, &job, 1, &protected, 1, 4, 1));
    struct dma_service_irq irq = {rearm, &service, 4, ss_latency_frequency / 250};
    configASSERT(dma_service_bind_irq(&service, &irq));
    client_task = xTaskCreateStaticAffinitySet(client, "modeled-DMA", 2048, NULL, 2,
        client_stack, &client_state, 1);
    configASSERT(client_task && dma_service_bind(&service, 0, client_task, 1));
}
void latency_dma_model_start(void)
{ xTaskNotifyGive(client_task); }
unsigned latency_dma_model_stop(void)
{
    atomic_store_explicit(&stop, 1, memory_order_release);
    TickType_t began = xTaskGetTickCount();
    while (!atomic_load_explicit(&done, memory_order_acquire)) {
        configASSERT((TickType_t)(xTaskGetTickCount() - began) < pdMS_TO_TICKS(100));
        vTaskDelay(1);
    }
    vTaskSuspend(service.worker);
    unsigned count = atomic_load_explicit(&jobs, memory_order_acquire);
    configASSERT(count >= 2);
    return count;
}
