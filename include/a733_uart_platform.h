#ifndef SUNSTONE_A733_UART_PLATFORM_H
#define SUNSTONE_A733_UART_PLATFORM_H
#include "a733_uart.h"
#include "a733_pins.h"
#include "a733_clock_leases.h"
#include <stdatomic.h>

/* Each route has a source-backed TX/RX pair and, where provided, RTS/CTS.
 * The platform still authenticates electrical and peripheral ownership. */
enum a733_uart_route {
    A733_UART0_PB0, A733_UART0_PB9, A733_UART0_PF,
    A733_UART1_BLUETOOTH, A733_UART1_HEADER, A733_UART2_PB,
    A733_UART3_PH, A733_UART4_PG, A733_UART5_PH, A733_UART6_PE,
    A733_S_UART0_PL, A733_S_UART1_PL
};
struct a733_uart_route_plan {
    unsigned port, bank, function, tx, rx, rts, cts, flow;
};
int a733_uart_route_plan(enum a733_uart_route,struct a733_uart_route_plan *);
enum a733_uart_platform_result {
    A733_UART_PLATFORM_PENDING, A733_UART_PLATFORM_BUSY, A733_UART_PLATFORM_READY,
    A733_UART_PLATFORM_CLOSED, A733_UART_PLATFORM_RETAINED, A733_UART_PLATFORM_INVALID
};
enum a733_uart_admission { A733_UART_ADMIT_START, A733_UART_ADMIT_STOP };
struct a733_uart_platform_config {
    uint64_t owner,handoff,startup_deadline_us;
    enum a733_uart_route route;
    struct a733_uart_format format;
    /* 1: application owns a mask-only ISR and deferred owner-task IIR service.
     * External root-owned route setup must precede initialization. Default 0
     * keeps the polling/always-masked contract used by Bluetooth. */
    unsigned deferred_irq;
    /* TX, RX, optional RTS, CTS. Function is selected by route; the desired
     * pull/drive values require platform approval. Expected functions may be
     * GPIO input, disabled, or that same UART route. No inherited output. */
    struct a733_pin_config expected[4],desired[4];
};
/* Callbacks are bounded and non-reentrant: they must not invoke this binding,
 * its borrowed UART, or callbacks into their interrupted caller. */
struct a733_uart_platform_ops {
    int (*now_us)(void *,uint64_t *);
    /* Exactly 1 only for the one designated running EL1 task, outside ISR,
     * critical sections and IRQ masking. Native adapter checks real task state. */
    int (*current)(void *);
    /* Exact token/owner/handoff authentication and exclusive console, remote
     * firmware, DMA and handler exclusion; pin voltages and physical mappings;
     * secure-register accessibility and independent watchdog recovery. START
     * also proves a quiescent peer. STOP proves the peer stopped and excludes
     * pending software handlers/submissions. 0 pending; any other non-1 result
     * uncertain. Numeric identities and idle bits do not prove this authority. */
    int (*authorize)(void *,const struct a733_uart_platform_config *,
        const struct a733_clock_token *,enum a733_uart_admission);
    int (*enter)(void *,uint64_t); /* Common register mutex: 1 held, 0 busy. */
    void (*leave)(void *);
    void (*retain)(void *); /* Retain an owned common mutex after uncertainty. */
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_uart_platform {
    struct a733_uart_platform_config config;
    struct a733_uart_platform_ops ops;
    struct a733_uart_route_plan route;
    struct a733_uart uart;
    struct a733_clock_leases *registry;
    struct a733_pins *pins;
    struct a733_clock_token clock;
    struct a733_pin_lease_banked pin_lease;
    struct a733_pin_request_banked requests[4];
    void *cookie;
    atomic_uint busy;
    uint64_t deadline,last_now,wait_since;
    uint32_t initial_bus;
    unsigned count,intid,stage,initialized,fault,held,retained,core_call;
};
/* No MMIO. Zero private storage once; all inputs, registry and pin manager
 * remain disjoint and live. Callbacks use the same clock as both managers.
 * Every pin/clock/GIC writer uses these authorities and the common mutex.
 * Parent source0/divider0 is read-only: main normalized SYS24M or raw S_UART DCXO at 24MHz.
 * Clock registry and pin manager changes do not establish physical admission. */
int a733_uart_platform_init(struct a733_uart_platform *,
    const struct a733_uart_platform_config *,const struct a733_uart_platform_ops *,
    void *,struct a733_clock_leases *,struct a733_pins *);
enum a733_uart_platform_result a733_uart_platform_poll(struct a733_uart_platform *);
/* Owner-only metadata retention after external authority loss; no device I/O. */
enum a733_uart_platform_result a733_uart_platform_retain(struct a733_uart_platform *);
/* One owner task may borrow the actual UART for existing task-polled protocol
 * workers after READY. Their guards call live before service. No interrupt
 * handler may access it. With deferred_irq=0 the GIC source stays masked.
 * With deferred_irq=1, an exclusive external ISR masks and publishes pending;
 * the owner calls irq_sync before task-context IIR service or close.
 * The pointer and generation remain borrowed until close or external recovery.
 * Direct begin/drain/reinitialization through the borrowed pointer is forbidden. */
struct a733_uart *a733_uart_platform_controller(struct a733_uart_platform *);
/* Task-only disabled/inactive/RWP completion proof for deferred IRQ mode.
 * Application must mask the owned source first and exclude rearm until service
 * finishes. PENDING means propagation or handler completion is still pending.
 * READY permits only owner-task controller service; never call from an ISR. */
enum a733_uart_platform_result a733_uart_platform_irq_sync(struct a733_uart_platform *,uint64_t deadline);
enum a733_uart_platform_result a733_uart_platform_live(struct a733_uart_platform *,uint64_t deadline);
/* A successful live guard also installs its admitted next-operation bound on
 * the READY core clock callback. Higher protocol operation deadlines do not
 * extend; callers must retain an independent fixed lifetime/session cap. */
/* Requires stopped submissions and peer_quiescent=1. Poll advances real UART
 * drain, reset assertion, gate removal, pin restoration and clock release.
 * Any uncertain step retains every unreleased real token. No auto-retry of data
 * or rollback; terminal fault only retries registry-retention metadata. */
enum a733_uart_platform_result a733_uart_platform_close(struct a733_uart_platform *,
    unsigned peer_quiescent,uint64_t deadline);
#endif
