#ifndef SUNSTONE_A733_DMA_POOL_SESSION_H
#define SUNSTONE_A733_DMA_POOL_SESSION_H
#include "a733_memory_reservations.h"
#include "a733_dma_startup.h"
#include "dma_service.h"

#define A733_DMA_POOL_BOOT_STACK 3072u
enum a733_dma_pool_state { A733_DMA_POOL_EMPTY, A733_DMA_POOL_WAITING,
    A733_DMA_POOL_READY, A733_DMA_POOL_FAILED };
struct a733_dma_pool_config {
    const struct a733_dma_handoff *handoff;
    struct a733_counter_clock *clock;
    struct a733_clock_leases *registry;
    struct a733_platform_locks *locks;
    uint64_t drain_ticks;
    unsigned copy_bytes; /* 4..4096, a multiple of four; one job per selected channel. */
    UBaseType_t priority, affinity;
    void (*fatal)(void *);
    void *fatal_cookie;
};
/* Private platform-lifetime storage. Zero once, never copy or reinitialize.
 * Its parent allocation remains DEVICE forever, including completed/reaped
 * jobs and runtime failures. CPU access follows the service's per-job protocol.
 * No runtime pool operation, release, or allocator state transition occurs.
 * Boot may expose a const pointer to atomic state for client observation and
 * copy its admitted buffer pointers into a separate immutable client view. */
struct a733_dma_pool_session {
    struct a733_dma_native native;
    struct a733_dma_startup startup;
    struct dma_service service;
    struct device_memory_pool *pool;
    struct device_memory_lease lease;
    struct device_memory_extent extent;
    struct dma_service_job jobs[DMA_SERVICE_JOBS];
    struct a733_dma_segment segments[DMA_SERVICE_JOBS];
    struct a733_dma_mapping mappings[DMA_SERVICE_JOBS][2];
    struct a733_dma_mapping stream_mappings[A733_I2S_STREAM_PERIODS];
    struct a733_dma_mapping duplex_mappings[A733_I2S_STREAM_PERIODS];
    unsigned periods,period_bytes;
    enum a733_i2s_direction direction;
    unsigned count, copy_bytes, admitted;
    atomic_uint state;
    atomic_uint retire_requested, retired;
    TaskHandle_t bootstrap;
    StaticTask_t bootstrap_state;
    StackType_t bootstrap_stack[A733_DMA_POOL_BOOT_STACK];
};
/* Call at EL1 during serialized boot before scheduling or secondary allocation.
 * Borrow the ONE initialized counter/registry/lock set for the image. All input,
 * session and provider storage must be disjoint, identity mapped image memory
 * outside the linker pool. The handoff is copied; other objects remain private
 * and immutable for platform lifetime. fatal_cookie, if present, refers to
 * private image state whose complete allocation stays outside the pool.
 * Real external exclusion, alias control, coherent caches and admitted recovery
 * must already be established. The default entry requires watchdog recovery;
 * the explicit supervised stream entry retains state for manual power removal. This constructor cannot establish them.
 * Creates the actual deferred worker and its only privileged bootstrap task.
 * Three pages per selected DMA0 channel: descriptors, source, destination.
 * Failure after acquisition retains the parent and prohibits another instance.
 * The acquired clock token remains private and permanently held; no runtime
 * publication/release may replace it. Atomic common-provider faults stop the
 * worker before its next device/cache operation through the nonreturning fatal
 * handler. Every parent and active transfer remains reserved on that path.
 */
/* Storage outside the image for the session's own state, its configuration and the objects it borrows: identity-mapped
 * write-back, inner-shareable pages, outside 0x41000000 to 0x42200000, given once at EL1 before the scheduler and before
 * any session. The session then accepts state there as it accepts image memory, and its DMA service refuses transfers
 * into those pages as it refuses them into the image. */
int a733_dma_pool_session_storage(const void *,size_t bytes);
int a733_dma_pool_session_init(struct a733_dma_pool_session *,
                              const struct a733_dma_pool_config *);
/* Fixed I2S0 consumer of the same native singleton and DMA worker. One selected
 * DMA channel, 2..8 period pages plus one descriptor page. The profile/cookie
 * must be protected image memory; direction admits only its exact FIFO route.
 * Period payloads are whole frames, at most A733_I2S_PERIOD_CAPACITY bytes,
 * 8-access aligned; each period has its own page.
 * For duplex, pass two compatible profiles with duplex=1, RX first and TX
 * second, and select exactly two channels. Each direction has its own descriptor
 * page and period pages. stream_mappings is RX; duplex_mappings is TX.
 * Parent and every allocation remain DEVICE for the image lifetime. */
int a733_dma_pool_session_stream_init(struct a733_dma_pool_session *,
    const struct a733_dma_pool_config *,const struct dma_service_i2s_stream_profile *,
    unsigned periods,unsigned period_bytes);
int a733_dma_pool_session_stream_supervised_init(struct a733_dma_pool_session *,
    const struct a733_dma_pool_config *,const struct dma_service_i2s_stream_profile *,
    unsigned periods,unsigned period_bytes);
/* Use with the private FreeRTOS port's task-privilege hook. Only the exact
 * bootstrap creation and existing service worker creation receive EL1t. */
BaseType_t a733_dma_pool_task_privileged(TaskFunction_t, void *);
/* The bound client 0 calls from EL1 task context to publish the supervisor's
 * absolute return deadline once. Repeating that deadline succeeds; changing
 * it, an expired deadline or another task is refused. Returns 1 or -1.
 * The limit covers pending startup and final verification, without changing
 * the admitted startup configuration or opening a service channel. */
int a733_dma_pool_session_return_limit(struct a733_dma_pool_session *,uint64_t);
/* After normal CLOSE and dma_service_retired: wake the original bootstrap
 * to verify the final platform, then wait for its last-access acknowledgment.
 * No pool, clock token or descriptor allocation is released. */
int a733_dma_pool_session_retire(struct a733_dma_pool_session *);
#endif
