#ifndef SUNSTONE_A733_SPI_H
#define SUNSTONE_A733_SPI_H
#include <stddef.h>
#include <stdint.h>

/* Ports 0..4 select SPI0..4; port 5 selects S_SPI. */
struct a733_spi_plan {
    uintptr_t base, clock, reset;
    uint32_t clock_word, divisor, actual_hz, intid;
};
/* Select the fastest 24 MHz / ((M+1)*(N+1)) rate no greater than max_hz.
 * Normal sampling accepts caps from 24194 Hz through 24 MHz here.
 * The exact rate is 24000000/divisor; actual_hz is its integer floor. The platform must reserve
 * the 24 MHz parent and program the plan under its clock-register mutex. */
int a733_spi_plan(unsigned port, uint32_t max_hz, struct a733_spi_plan *);
struct a733_spi_format {
    unsigned port, cs, mode, lsb_first, active_high;
    uint32_t max_hz, setup_us, hold_us;
};
enum a733_spi_state {
    A733_SPI_CLOSED, A733_SPI_RESETTING, A733_SPI_FIFO_RESET,
    A733_SPI_READY, A733_SPI_SETUP, A733_SPI_ACTIVE, A733_SPI_HOLD,
    A733_SPI_COMPLETE, A733_SPI_FAULT
};
enum a733_spi_transfer_kind {
    A733_SPI_FULL_DUPLEX, A733_SPI_WRITE, A733_SPI_READ, A733_SPI_WRITE_READ
};
struct a733_spi_transfer {
    enum a733_spi_transfer_kind kind;
    const uint8_t *tx;
    uint8_t *rx;
    uint32_t tx_bytes, rx_bytes;
};
/* Every callback is bounded and returns exactly 1 on success. Access failure
 * or expiration after a side effect retains FAULT, buffers and all leases.
 * read8/write8 are byte FIFO accesses, never widened to 32-bit accesses.
 * admit revalidates the owned controller, stable clock and masked interrupt.
 * It does not acquire ownership. now_us is monotonic and nonwrapping. */
struct a733_spi_ops {
    int (*admit)(void *, const struct a733_spi_format *, const struct a733_spi_plan *);
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*write32)(void *, uintptr_t, uint32_t);
    int (*read8)(void *, uintptr_t, uint8_t *);
    int (*write8)(void *, uintptr_t, uint8_t);
    int (*now_us)(void *, uint64_t *);
};
struct a733_spi {
    struct a733_spi_plan plan;
    struct a733_spi_format format;
    struct a733_spi_ops ops;
    struct a733_spi_transfer transfer;
    void *context;
    uint64_t deadline, last_now, wait_since;
    uint32_t tcr, sent, received, errors;
    enum a733_spi_state state;
};
/* Zero-initialize once. The caller holds exclusive controller, clock/reset,
 * pin, rail and interrupt ownership, excludes DMA/remote firmware and supplies
 * a freshly reset, disabled controller. Reserve only the routed CS and data
 * pins. Boot storage and alternate functions must be excluded by the platform.
 * Serialize all calls; callbacks cannot reenter. Keep callback context and
 * controller storage immutable to other tasks. All objects and buffers must
 * be disjoint. Independent watchdog recovery is required for stalled MMIO.
 * Invalid arguments return -1 without I/O. Otherwise return current state. */
int a733_spi_begin(struct a733_spi *, const struct a733_spi_ops *, void *,
                   const struct a733_spi_format *, uint64_t deadline);
/* Borrow both buffers until reap. FULL_DUPLEX requires equal nonzero lengths;
 * WRITE/READ require only their corresponding span; WRITE_READ holds CS over
 * the command and response. Maximum wire length is 0xffffff bytes.
 * Incoming bytes overwrite rx as they arrive, but are valid only on COMPLETE.
 * Transmitted counts mean FIFO acceptance, not peripheral acknowledgement. */
int a733_spi_submit(struct a733_spi *, const struct a733_spi_transfer *,
                    uint64_t deadline);
/* At most budget RX bytes plus budget TX bytes, budget 1..64. FIFO progress
 * can stretch the clock; this interface makes no continuous-clock guarantee.
 * Only error-free transfer completion, empty FIFO/internal buffers, and XCH
 * clear permit CS deassertion. setup_us/hold_us are minimum CS times. */
int a733_spi_poll(struct a733_spi *, unsigned budget);
/* COMPLETE -> READY, releasing borrowed buffers. No automatic retry or abort.
 * A fault needs platform recovery; never reset an active transaction here. */
int a733_spi_reap(struct a733_spi *);
/* READY -> CLOSED after idle readback and disabling the controller. Platform
 * leases remain held until its interrupt/pin/clock release protocol finishes. */
int a733_spi_close(struct a733_spi *, uint64_t deadline);
#endif
