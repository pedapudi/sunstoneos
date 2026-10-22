#ifndef SUNSTONE_A733_SPI_PLATFORM_H
#define SUNSTONE_A733_SPI_PLATFORM_H
#include "a733_spi.h"
#include "a733_pins.h"
#include "a733_clock_leases.h"
#include <stdatomic.h>

enum a733_spi_route {
    A733_SPI0_PC, A733_SPI1_PD, A733_SPI2_PB, A733_SPI3_PE,
    A733_S_SPI_PL, A733_S_SPI_PM
};
struct a733_spi_route_plan { unsigned port,bank,function,clock,mosi,miso,cs; };
/* Source-proven single-lane routes. SPI4 has a controller descriptor but no
 * admitted package route: published A733 sources lack PI signal assignments. */
int a733_spi_route_plan(enum a733_spi_route,unsigned cs,struct a733_spi_route_plan *);
enum a733_spi_platform_result {
    A733_SPI_PLATFORM_PENDING,A733_SPI_PLATFORM_BUSY,A733_SPI_PLATFORM_READY,
    A733_SPI_PLATFORM_COMPLETE,A733_SPI_PLATFORM_CLOSED,A733_SPI_PLATFORM_RETAINED,
    A733_SPI_PLATFORM_INVALID
};
enum a733_spi_platform_admission { A733_SPI_PLATFORM_START,A733_SPI_PLATFORM_STOP };
struct a733_spi_platform_config {
    uint64_t owner,handoff,deadline;
    enum a733_spi_route route;
    struct a733_spi_format format;
    /* Clock, MOSI, MISO, selected CS. Initial mux must be input or disabled.
     * The handoff proves electrical CS inactivity, rails, external bias and
     * absence of active peripheral producers. Desired function follows route. */
    struct a733_pin_config expected[4],desired[4];
};
struct a733_spi_platform_ops {
    int (*now)(void *,uint64_t *);
    int (*current)(void *); /* Exactly 1 for the designated running owner task. */
    /* Authenticated boot/electrical authority, not software lease substitutes.
     * START excludes boot storage, remote firmware, other handlers and every
     * DMA user except, with format.dma, this controller's own request channels.
     * It also excludes EVERY alternate pad route and unselected CS output of
     * this controller before enabling it. Every subsequent SPI mux writer
     * must honor the same exclusive controller lease. The four-pin lease
     * alone does not establish whole-controller pad exclusion. START
     * establishes stable24MHz source, rail/PIO mappings and operation-specific qualified recovery. Supervised
     * permanent retention/manual power removal is permitted; a software deadline
     * cannot recover stalled MMIO or establish reset of an independently powered peer.
     * STOP excludes submissions/peer activity and authorizes original pin-state
     * restoration. Every route, including SPI0 storage pins and SPI1 display
     * pins, requires specific authority. 0 pending; 1 granted; other uncertain. */
    int (*authorize)(void *,const struct a733_spi_platform_config *,
        const struct a733_clock_token *,enum a733_spi_platform_admission);
    int (*enter)(void *,uint64_t); /* One common register mutex, 1 held, 0 busy. */
    void (*leave)(void *);
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
    int (*read8)(void *,uintptr_t,uint8_t *);
    int (*write8)(void *,uintptr_t,uint8_t);
};
struct a733_spi_platform {
    struct a733_spi_platform_config config;
    struct a733_spi_platform_ops ops;
    struct a733_spi_route_plan route;
    struct a733_spi spi;
    struct a733_clock_leases *registry;
    struct a733_pins *pins;
    struct a733_clock_token clock;
    struct a733_pin_lease_banked pin_lease;
    struct a733_pin_request_banked requests[4];
    void *context;
    atomic_uint busy;
    uint64_t deadline,last_now,wait_since;
    uint32_t initial_clock,initial_bus;
    unsigned initialized,stage,fault,held,retained,core_call,pins_released;
    unsigned interrupt_armed,interrupt_service;
};
/* Private zero-once state; objects/managers/input spans are disjoint and live.
 * Callbacks are bounded and cannot reenter. All providers share one monotonic
 * microsecond clock, pin manager, widened registry and register mutex. */
int a733_spi_platform_init(struct a733_spi_platform *,const struct a733_spi_platform_config *,
    const struct a733_spi_platform_ops *,void *,struct a733_clock_leases *,struct a733_pins *);
/* Poll initialization, current transaction, or shutdown. budget1..64. Startup
 * configures only an idle controller before routing pins; it sends no bytes.
 * Idle READY/COMPLETE returns cached status without extending an old deadline.
 * Every submit/reap/close supplies a fresh deadline and revalidates ownership,
 * pin/native health and actual clock/interrupt state before acting. */
enum a733_spi_platform_result a733_spi_platform_poll(struct a733_spi_platform *,unsigned budget);
enum a733_spi_platform_result a733_spi_platform_submit(struct a733_spi_platform *,
    const struct a733_spi_transfer *,uint64_t deadline);
enum a733_spi_platform_result a733_spi_platform_reap(struct a733_spi_platform *,uint64_t deadline);
/* Fixed FIFO-sized interrupt completion: call after SETUP reaches ACTIVE and
 * all TX bytes are queued. RX must fit the192-byte FIFO. Both functions run
 * only in the owner task with the source masked/inactive at the GIC.
 * start enables source-defined TC/RX-overflow/RX-underflow/TX-overflow IRQs.
 * The caller then unmasks its configured source and waits for a real ISR claim.
 * interrupt_poll requires that claim. It disables IER before bounded deferred
 * FIFO/status/hold work; it never acknowledges a GIC interrupt or resets DMA.
 * Polling without a claim is not an interrupt completion path. All leases and
 * buffers remain held after uncertainty. */
enum a733_spi_platform_result a733_spi_platform_interrupt_start(struct a733_spi_platform *);
enum a733_spi_platform_result a733_spi_platform_interrupt_poll(struct a733_spi_platform *,unsigned budget);
/* DMA transfer abandonment: gate both FIFO requests under the register mutex
 * with a fresh cleanup deadline, then retain every lease. Check spi.dma_requests:
 * zero permits the caller to cancel its DMA channels; otherwise the requests are
 * uncertain and the channels need external recovery. Never returns READY. */
enum a733_spi_platform_result a733_spi_platform_dma_stop(struct a733_spi_platform *,uint64_t deadline);
/* Only READY with no borrowed buffers can close. Mask/drain, restore pins with
 * CS inactive, disable controller, assert reset, restore clocks, release tokens.
 * No forced abort. Uncertainty retains every unreleased token and owned mutex. */
enum a733_spi_platform_result a733_spi_platform_close(struct a733_spi_platform *,uint64_t deadline);
#endif
