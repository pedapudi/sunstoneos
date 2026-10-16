#ifndef SUNSTONE_A733_I2S_H
#define SUNSTONE_A733_I2S_H

#include "a733_dma_controller.h"

#define A733_I2S0_BASE UINT32_C(0x02532000)
#define A733_I2S0_RXFIFO (A733_I2S0_BASE + 0x10u)
#define A733_I2S0_DMA_REQUEST 3u

/* Normal I2S, two wire slots, MSB first, CPU supplies BCLK and LRCK.
 * container_bits is 16 or 32; slot_bits is 16 or 32 and >= container_bits.
 * Mono selects first_slot 0 or 1; stereo must start at slot 0.
 * A 24-bit microphone uses a raw 32-bit container and 32-bit slot. Its data
 * alignment, valid bits, sign, and padding require the sensor's contract.
 * The core performs no sample conversion or implicit sign extension. */
struct a733_i2s_format {
    uint32_t module_hz, rate_hz, mclk_hz; /* Actual leased module clock; 0 disables MCLK. */
    unsigned container_bits, slot_bits, channels, first_slot;
};
struct a733_i2s_plan {
    struct a733_i2s_format format;
    uint32_t ctl, fmt0, clkdiv, chcfg, rxchsel, rxmap3;
    uint32_t pb_mask; /* PB5/6/8, optionally PB4; every selected pin uses function 3. */
    unsigned dma_width, frame_bytes;
};

/* No I/O or output mutation on refusal. Exact integer clock ratios only. */
int a733_i2s_plan(const struct a733_i2s_format *, struct a733_i2s_plan *);

enum a733_i2s_status {
    A733_I2S_CLOSED, A733_I2S_READY, A733_I2S_PREPARING, A733_I2S_ACTIVE,
    A733_I2S_STOPPING, A733_I2S_COMPLETE, A733_I2S_CANCELED, A733_I2S_FAILED,
    A733_I2S_OPENING, A733_I2S_CLOSING,
    A733_I2S_INVALID = -1, A733_I2S_CONFLICT = -2, A733_I2S_RECOVERY = -3
};

/* Resource results are distinct from capture states. BUSY on the first acquire
 * has no effects. PENDING admits resources and requires acquire_poll. RETAINED
 * means uncertain state: preserve every token and perform no further I/O.
 * BUSY while opening/closing retains already admitted resources. */
enum a733_i2s_resource_result {
    A733_I2S_RESOURCE_READY, A733_I2S_RESOURCE_BUSY,
    A733_I2S_RESOURCE_PENDING, A733_I2S_RESOURCE_RETAINED
};
/* All resource deadlines are absolute, nonwrapping monotonic microseconds;
 * DMA capture timeout uses the DMA controller's separate tick unit.
 * Callbacks are bounded, ordered and non-reentrant. READY acquire guarantees
 * leased pins, stable exact clocks and a fresh reset I2S0. release requires
 * producer/DMA quiescence. Parent clock leases can outlive this local capture.
 * Hardware watchdog protection remains mandatory for native access. */
struct a733_i2s_operations {
    enum a733_i2s_resource_result (*acquire)(void *, const struct a733_i2s_plan *, uint64_t);
    enum a733_i2s_resource_result (*acquire_poll)(void *, uint64_t);
    enum a733_i2s_resource_result (*release)(void *, uint64_t);
    int (*now_us)(void *, uint64_t *);
    int (*read)(void *cookie, uintptr_t address, uint32_t *value);
    int (*write)(void *cookie, uintptr_t address, uint32_t value);
};

/* Zero-initialize once. Serialize this object AND every operation on its DMA
 * controller. Do not modify fields, reinitialize failures, or operate its DMA
 * channel independently. A controller uses an explicit, already authorized
 * DMA0 channel; this service never changes global DMA clocks or routing. */
struct a733_i2s_capture {
    struct a733_i2s_plan plan;
    struct a733_i2s_operations operations;
    void *cookie;
    struct a733_dma_controller *dma;
    const struct a733_dma_transfer *transfer;
    uint64_t started, timeout, resource_deadline, resource_time;
    enum a733_i2s_status status, outcome;
    unsigned channel, platform_held, dma_held, buffers_held;
};

/* OPENING and CLOSING continue through poll() with the originally admitted
 * deadline. An unfinished open cannot be canceled; expiry retains resources.
 * owns() concerns capture buffers only, not platform/clock/pin ownership. */
enum a733_i2s_status a733_i2s_open(struct a733_i2s_capture *,
    const struct a733_i2s_format *, const struct a733_i2s_operations *, void *cookie,
    struct a733_dma_controller *, unsigned channel, uint64_t acquisition_deadline_us);

/* One finite capture per open. Metadata and complete buffer allocations are
 * exclusive/immutable from accepted start until owns() is false, even if the
 * underlying DMA controller releases them sooner. timeout uses DMA ticks and
 * covers preparation and capture; DMA has its separate bounded drain timeout.
 * Segments must read I2S0 RXFIFO, request 3, burst 8, with container-sized bus
 * accesses, and write ordinary RAM with the same width/burst. Whole frames only.
 * PREPARING polls wait for FIFO flush; no call spins. Format/route refusals
 * have no I/O. DMA validates mappings before arming and can then report FAILED.
 * COMPLETE proves finite DMA completion and drain, not valid microphone audio.
 * CANCELED/FAILED buffers can be reused but contain no accepted audio result. */
enum a733_i2s_status a733_i2s_start(struct a733_i2s_capture *,
    const struct a733_dma_transfer *, uint64_t timeout);
enum a733_i2s_status a733_i2s_poll(struct a733_i2s_capture *);
enum a733_i2s_status a733_i2s_cancel(struct a733_i2s_capture *);
enum a733_i2s_status a733_i2s_close(struct a733_i2s_capture *, uint64_t release_deadline_us);
int a733_i2s_owns(const struct a733_i2s_capture *);

#endif
