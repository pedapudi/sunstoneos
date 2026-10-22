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
    uint32_t fcr; /* FIFO enable and RX trigger, without the reset bits. */
};
/* Fixed RX FIFO trigger. The 256-byte FIFOs of UART1..6 trigger at half full,
 * as the Linux BSP does above 9600 baud, so automatic RTS does not throttle
 * the peer after every character and IRQ mode gets one RX interrupt per burst
 * (a receive timeout, IIR cause 12, ends a shorter burst). The 64-byte FIFOs of
 * UART0 and S_UART0..1 keep the one-character trigger. */
static inline uint32_t a733_uart_fcr(unsigned port)
{ return port && port < 7 ? 0x81u : 1u; }
int a733_uart_plan(const struct a733_uart_format *, struct a733_uart_plan *);
/* DMA0 request line shared by RX and TX (manual table 13-2): UART0..6 use
 * 14..20 and S_UART0..1 use 50..51. Returns 0 for an invalid port. */
static inline unsigned a733_uart_dma_request(unsigned port)
{ return port < 7 ? 14u + port : port < 9 ? 43u + port : 0u; }
/* Line modes selected by a733_uart_mode after configuration. IRDA is IrDA 1.0
 * SIR (MCR function 1) without flow control, at most 115200 baud within the
 * 0.87% SIR rate tolerance. RS485 is the
 * hardware half-duplex 9-bit mode (MCR function 2 with automatic RTS driver
 * enable) on UART1..6; it requires eight data bits and parity, whose odd/even
 * selection supplies the transmitted ninth bit. LOOPBACK is the internal
 * diagnostic loop (MCR LOOP): TX is held idle and RTS/DTR loop to CTS/DSR. */
enum a733_uart_mode {
    A733_UART_MODE_UART, A733_UART_MODE_IRDA, A733_UART_MODE_RS485,
    A733_UART_MODE_LOOPBACK
};

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
    unsigned dma_owned, dma_requests, dma_released, mode;
};
/* Errors retain LSR bits 1..4 (overrun, parity, framing, break) and bit 7
 * (an error remains in RX FIFO). Per-byte status accompanies received data.
 * In RS485 mode status bit 0 carries the received ninth (address) bit. */
#define A733_UART_NINTH_BIT 1u
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
 * The deadline is checked before and after each RX or TX FIFO batch, not
 * around every access. Counts report every byte the batch moved; a failed
 * access or a late batch then faults the object, and the final byte may
 * already have crossed the hardware interface.
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
/* Select a line mode in READY without a DMA lease or armed THRE. The line must
 * be quiet: TX FIFO and shift register empty, RX empty and USR idle; otherwise
 * return READY unchanged (0 is never returned). MCR and, for RS485, the RS485
 * control register are written and read back; a mismatch faults. The mode
 * persists until another call or the external reset. Physical IrDA and RS-485
 * transceivers are outside this object. Invalid or unsupported modes return -1. */
int a733_uart_mode(struct a733_uart *, enum a733_uart_mode, uint64_t deadline);
/* Finite DMA lease on any port, same exclusive owner as FIFO/IRQ calls. prepare
 * requires both DMA channels stopped; requests starts only after both active.
 * FIFO/IRQ service refuses while leased. status samples aggregate LSR only,
 * never RBR: DMA cannot preserve per-byte error flags. stop disables/readbacks
 * requests but preserves the lease. release requires externally proved DMA
 * channel quiescence and true TEMT/RX-empty/USR-idle. All uncertainty retains.
 * Release is terminal: IER stays 0 and FCR stays in DMA mode, so exchange,
 * interrupt, tx_irq and another prepare refuse; only drain and close follow.
 * DMA mode uses the one-character RX trigger on every port. With automatic
 * flow control, RTS therefore deasserts while a received byte waits for DMA.
 * Return READY, FAULT or -1 for invalid calls; release returns 0 while draining.
 * The caller retains both DMA buffers until release, copy and matched REAP. */
int a733_uart_dma_prepare(struct a733_uart *,uint64_t deadline);
int a733_uart_dma_requests(struct a733_uart *,unsigned enabled,uint64_t deadline);
int a733_uart_dma_status(struct a733_uart *,uint64_t deadline);
int a733_uart_dma_release(struct a733_uart *,unsigned channels_quiet,uint64_t deadline);

#endif
