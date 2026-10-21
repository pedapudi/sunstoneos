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
    A733_I2S_INPUT_ENDED, /* DMA complete/FIFO empty observed; wire unknown, leases retained. */
    A733_I2S_INVALID = -1, A733_I2S_CONFLICT = -2, A733_I2S_RECOVERY = -3
};

/* Resource results are distinct from stream states. BUSY on the first acquire
 * has no effects. PENDING admits resources and requires acquire_poll. RETAINED
 * means uncertain state: preserve every token and perform no further I/O.
 * BUSY while opening/closing retains already admitted resources. */
enum a733_i2s_resource_result {
    A733_I2S_RESOURCE_READY, A733_I2S_RESOURCE_BUSY,
    A733_I2S_RESOURCE_PENDING, A733_I2S_RESOURCE_RETAINED
};
/* All resource deadlines are absolute, nonwrapping monotonic microseconds;
 * DMA period timeouts use the DMA controller's separate tick unit.
 * Callbacks are bounded, ordered and non-reentrant. READY acquire guarantees
 * leased pins, stable exact clocks and a fresh reset I2S0. release requires
 * producer/DMA quiescence. Parent clock leases can outlive this local binding.
 * Native access requires actual IRQ/clock/producer ownership and admitted
 * recovery: qualified watchdog or explicit supervised manual power removal.
 * Supervision retains every uncertain DMA allocation; timeout or reconnection
 * does not establish drain or authorize reuse. */
struct a733_i2s_operations {
    enum a733_i2s_resource_result (*acquire)(void *, const struct a733_i2s_plan *, uint64_t);
    enum a733_i2s_resource_result (*acquire_poll)(void *, uint64_t);
    enum a733_i2s_resource_result (*release)(void *, uint64_t);
    int (*now_us)(void *, uint64_t *);
    int (*read)(void *cookie, uintptr_t address, uint32_t *value);
    int (*write)(void *cookie, uintptr_t address, uint32_t value);
};

#endif
