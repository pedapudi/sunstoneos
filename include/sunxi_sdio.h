#ifndef SUNSTONE_SUNXI_SDIO_H
#define SUNSTONE_SUNXI_SDIO_H
#include <stddef.h>
#include <stdint.h>

/* The largest CMD53: 511 counted blocks of 512 bytes, as the pinned Linux SDIO
 * helper issues it when the host request limits allow. */
#define SUNXI_SDIO_MAX_BYTES (511u * 512u)
#define SUNXI_SDIO_FIFO_BUDGET 16u
/* Four-bit default speed moves at most one 512-byte block per poll, sized from
 * one FIFO-level sample, as the SD/eMMC core does at the same clock. */
#define SUNXI_SDIO_FIFO_BUDGET_WIDE 128u

enum sunxi_sdio_state { SUNXI_SDIO_UNPREPARED, SUNXI_SDIO_PREPARING,
    SUNXI_SDIO_READY, SUNXI_SDIO_QUEUED, SUNXI_SDIO_ACTIVE,
    SUNXI_SDIO_DONE, SUNXI_SDIO_FAULT, SUNXI_SDIO_WIDENING };
enum sunxi_sdio_error { SUNXI_SDIO_NO_ERROR, SUNXI_SDIO_IO_ERROR,
    SUNXI_SDIO_TIMEOUT, SUNXI_SDIO_CLOCK_ERROR, SUNXI_SDIO_VARIANT_ERROR,
    SUNXI_SDIO_SETUP_ERROR, SUNXI_SDIO_CONTROLLER_ERROR,
    SUNXI_SDIO_RESPONSE_ERROR, SUNXI_SDIO_CARD_ERROR };
struct sunxi_sdio_ops {
    /* Offsets are relative to an exclusively owned, accessible SDC1 aperture.
     * Callbacks return 1 on success, 0 on failure. Writes may have taken effect
     * before failure. Callbacks must be bounded and must not reenter the core.
     */
    int (*read32)(void *, uint32_t offset, uint32_t *);
    int (*write32)(void *, uint32_t offset, uint32_t);
    int (*barrier)(void *);
    int (*now)(void *, uint64_t *);
    /* Optional; required only by sunxi_sdio_widen. Select the PERI0 400 MHz
     * parent and this linear module divisor while the card clock is stopped. */
    int (*module)(void *, unsigned divisor);
};
struct sunxi_sdio_config {
    struct sunxi_sdio_ops ops;
    void *context;
    /* The platform must establish and retain this measured/admitted module
     * rate, clocks/gates, reset release, PG/PM pin ownership, voltage and radio
     * power settling before prepare. 0 < module_hz <= 800000; SDR output /2.
     * No CCU, pin, regulator, eMMC reset, or DMA ownership is acquired here.
     */
    uint32_t module_hz;
};
struct sunxi_sdio_request {
    uint32_t argument;
    uint8_t command; /* Only CMD3, CMD5, CMD7, CMD52, CMD53. */
    uint16_t block_bytes; /* CMD53 block mode only; 4..512, multiple of 4. */
    uint8_t *data; /* CMD53: borrowed exclusively through completion/fault. */
    size_t bytes; /* 4..SUNXI_SDIO_MAX_BYTES, whole words; buffers may be unaligned. */
};
struct sunxi_sdio_result {
    enum sunxi_sdio_error error;
    uint32_t response, interrupts, status;
    size_t transferred;
};
struct sunxi_sdio {
    struct sunxi_sdio_config config;
    struct sunxi_sdio_request request;
    struct sunxi_sdio_result result;
    enum sunxi_sdio_state state;
    uint64_t last_now, deadline;
    uint64_t progress; /* Owner-only count of polls that changed state or moved FIFO words. */
    uint32_t version, card_hz;
    uint8_t step, response_seen, sdio_pending, entered, initialized, wide, divisor;
};

/* Single serialized owner; no API is safe for concurrent task/ISR use.
 * Context and callback-owned storage must outlive the core and must not overlap
 * it or request buffers. Pure initialization copies config and rejects overlap
 * before writing host state. Storage must be zero-initialized before its first
 * use. Successful init sets a permanent guard; every later init is refused.
 * No disposal API exists. Never clear or reuse storage while a request, fault,
 * callback context, or hardware access remains owned. */
int sunxi_sdio_init(struct sunxi_sdio *, const struct sunxi_sdio_config *);
/* Pure admission; absolute deadlines use ops.now units. No automatic retries. */
int sunxi_sdio_prepare(struct sunxi_sdio *, uint64_t deadline);
int sunxi_sdio_submit(struct sunxi_sdio *, const struct sunxi_sdio_request *, uint64_t deadline);
/* READY only, once, after the owner has set the card's CCCR bus width to four
 * bits. Poll stops the card clock, has ops.module select PERI0 400 MHz divided
 * by 8..32, sets four-bit width, then restarts the clock: card clock is
 * 400 MHz / (2 x divisor), at most 25 MHz. The controller stays in the old
 * timing mode with the drive/sample delays from prepare, which the pinned tm4
 * table also gives for 25 MHz default speed. READY follows readback of clock,
 * width, delays and timing mode. Failure is terminal like any request;
 * the card keeps its four-bit setting and no automatic one-bit fallback exists. */
int sunxi_sdio_widen(struct sunxi_sdio *, unsigned divisor, uint64_t deadline);
/* One bounded step. Pending controller conditions return immediately. */
void sunxi_sdio_poll(struct sunxi_sdio *);
/* DONE becomes READY. FAULT is copied but remains terminal with request retained.
 * Output must not overlap host or borrowed request data. */
int sunxi_sdio_take_result(struct sunxi_sdio *, struct sunxi_sdio_result *);
/* Polling preserves the SDIO raw interrupt bit. The sole owner may acknowledge
 * it only in READY after servicing the function's interrupt source; immediate
 * reassertion remains latched. This is not a GIC interrupt binding. */
int sunxi_sdio_ack_irq(struct sunxi_sdio *);
/* Shared monotonic observation for the card worker; no backend calls in FAULT. */
int sunxi_sdio_time(struct sunxi_sdio *, uint64_t *);
/* Stop admission without further hardware access. Retain the active request,
 * partial read bytes and first error. External recovery owns eventual disposal. */
void sunxi_sdio_stop(struct sunxi_sdio *, enum sunxi_sdio_error);
#endif
