#ifndef SUNSTONE_A733_UART_H
#define SUNSTONE_A733_UART_H

#include <stddef.h>
#include <stdint.h>

/* UART0..6 are 0..6; S_UART0..1 are 7..8. */
struct a733_uart_format {
    unsigned port, data_bits, stop_half_bits, parity, auto_flow;
    uint32_t clock_hz, baud, max_error_ppm;
};
/* parity: 0 none, 1 odd, 2 even. stop_half_bits: 2 for one stop bit,
 * 3 for 1.5 (five data bits only), 4 for two (six through eight bits). */
struct a733_uart_plan {
    uintptr_t base;
    uint32_t lcr, mcr, divisor, actual_baud;
    unsigned fifo_bytes;
};
int a733_uart_plan(const struct a733_uart_format *, struct a733_uart_plan *);

enum a733_uart_state {
    A733_UART_CLOSED, A733_UART_CONFIGURING, A733_UART_READY,
    A733_UART_DRAINING, A733_UART_DRAINED, A733_UART_FAULT
};
/* Return 1 on success; any other result means uncertain access and retains
 * FAULT. All register callbacks use ordered 32-bit Device accesses. Deadline
 * units are monotonic nonwrapping microseconds; callbacks must be bounded.
 * An absolute deadline cannot interrupt a stalled bus access: independent
 * watchdog recovery is a physical prerequisite. */
struct a733_uart_ops {
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*write32)(void *, uintptr_t, uint32_t);
    int (*now_us)(void *, uint64_t *);
};
/* Zero-initialize once. Caller holds an exclusive port lease, stable clock,
 * authorized pin/rail ownership, fresh local reset, quiescent peer and disabled
 * GIC source before begin. Exclude console/firmware/other CPU access and DMA.
 * Serialize task/ISR callers without a blocking mutex in interrupt context. Keep every
 * lease on FAULT; this object never resets, gates or releases shared resources.
 * All objects, callback context, buffers and outputs must have disjoint storage
 * unless explicitly described otherwise. Do not modify public state directly. */
struct a733_uart {
    struct a733_uart_plan plan;
    struct a733_uart_ops ops;
    void *context;
    uint64_t deadline, last_now;
    enum a733_uart_state state;
    uint32_t ier, errors;
};
/* Errors retain LSR bits 1..4 (overrun, parity, framing, break) and bit 7
 * (an error remains in RX FIFO). Per-byte status accompanies received data. */
struct a733_uart_byte { uint8_t value, status; };
struct a733_uart_result {
    size_t received, transmitted;
    uint32_t errors, modem;
    unsigned cause; /* IIR low nibble; 1 means no interrupt. */
};
/* Invalid arguments return -1 without I/O. Accepted operations return current
 * state (FAULT is sticky). begin/poll do no busy waiting. READY means local
 * configuration completed; it establishes neither a connected peer nor HCI. */
int a733_uart_begin(struct a733_uart *, const struct a733_uart_ops *, void *,
                    const struct a733_uart_format *, uint64_t deadline);
int a733_uart_poll(struct a733_uart *);
/* Each call moves at most budget RX and budget TX bytes, with budget 1..256.
 * Counts report accesses completed within the deadline. On a failed or late
 * access, the final byte may already have crossed the hardware interface.
 * TX completion here means accepted by FIFO. Retain/retry only the unconsumed suffix. An uncertain
 * write faults the object; do not retransmit it automatically.
 * exchange services FIFO status directly; interrupt services one IIR cause.
 * Caller retains pending TX and supplies its remaining span on every service.
 * THRE IRQ is armed while that span remains, disabled when it is consumed.
 * No IRQ is a successful zero-work result. A zero RX capacity leaves data in
 * the FIFO and preserves unread line status; the caller must arrange
 * backpressure or accept possible overrun.
 * All supplied buffers/results must be disjoint from the controller object
 * and from each other. The controller stores no buffer pointers. */
int a733_uart_exchange(struct a733_uart *, struct a733_uart_byte *, size_t,
                       const uint8_t *, size_t, unsigned budget,
                       uint64_t deadline, struct a733_uart_result *);
/* Arm/disarm THRE without transmitting. READY and exclusive task ownership
 * required; used to start an interrupt-driven transfer without FIFO polling. */
int a733_uart_tx_irq(struct a733_uart *,unsigned enabled,uint64_t deadline);
int a733_uart_interrupt(struct a733_uart *, struct a733_uart_byte *, size_t,
                        const uint8_t *, size_t, unsigned budget,
                        uint64_t deadline, struct a733_uart_result *);
/* Before drain, stop submissions, quiesce the peer and mask/synchronize GIC.
 * Poll until TX FIFO and shift register empty, RX empty and USR idle. Pending
 * RX is preserved and will cause deadline failure unless consumed first.
 * DRAINED permits the external owner to reset/release; it is not restoration
 * of inherited console state. GIC acknowledgement remains external. */
int a733_uart_drain(struct a733_uart *, uint64_t deadline);

#endif
