/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_OV5640_H
#define SUNSTONE_OV5640_H

#include <stdint.h>

/* One serialized privileged owner. Initialize fresh zeroed storage once.
 * All callback contexts and this object remain reserved until CLOSED, or
 * indefinitely after FAULT. No callback may reenter this component.
 * Callbacks are bounded by the absolute microsecond deadline and return
 * 1 on success, 0 only for acquire/release/receiver pending, -1 on uncertainty.
 * A failed operation may have changed hardware: the driver never rolls back.
 */
enum ov5640_state {
    OV5640_NEW, OV5640_ACQUIRE, OV5640_POWER_WAIT, OV5640_PWDN,
    OV5640_RESET_WAIT, OV5640_RESET, OV5640_BUS_WAIT, OV5640_IDENTIFY,
    OV5640_CONFIGURE, OV5640_READY, OV5640_RECEIVER, OV5640_STARTING,
    OV5640_STREAMING, OV5640_STOPPING, OV5640_RELEASING,
    OV5640_CLOSED, OV5640_FAULT
};
enum ov5640_result { OV5640_ERROR = -1, OV5640_PENDING = 0, OV5640_DONE = 1 };
enum ov5640_line { OV5640_PWDN_LINE, OV5640_RESET_LINE };
/* Output sizes of the reference driver's mode table, in its order. */
enum ov5640_size {
    OV5640_QQVGA, OV5640_QCIF, OV5640_QVGA, OV5640_VGA, OV5640_NTSC,
    OV5640_PAL, OV5640_XGA, OV5640_720P, OV5640_1080P, OV5640_QSXGA,
    OV5640_SIZES
};
/* CSI-2 pixel formats: YUV422 8-bit in two byte orders, and raw Bayer
 * 8-bit starting with a blue/green row. */
enum ov5640_format { OV5640_UYVY, OV5640_YUYV, OV5640_SBGGR8, OV5640_FORMATS };
/* pixel_hz is the sensor timing clock; one frame takes htotal * vtotal of
 * its periods. link_hz is the CSI-2 clock lane frequency. */
struct ov5640_mode {
    uint32_t xclk_hz, link_hz, pixel_hz;
    uint16_t width, height, htotal, vtotal;
    uint8_t lanes, virtual_channel, csi_data_type, bytes_per_pixel;
    uint8_t size, format; /* enum ov5640_size, enum ov5640_format */
};
/* The default: 640x480 UYVY at nominal 30 frames/s. */
extern const struct ov5640_mode ov5640_vga_mode;
/* Derive a two-lane mode with a 24 MHz XCLK, using the reference driver's
 * crop, timing, pixel-rate and PLL calculation. vtotal 0 keeps the
 * reference frame length; otherwise it must lie in height+24..3375 and sets
 * the frame rate to pixel_hz / (htotal * vtotal). Automatic exposure limits
 * and banding steps stay those of the reference frame length.
 * Returns 1, or -1 without writing *mode. */
int ov5640_mode_get(struct ov5640_mode *mode, enum ov5640_size size,
                    enum ov5640_format format, unsigned vtotal);

struct ov5640_ops {
    int (*now_us)(void *context, uint64_t *now);
    /* Acquire reserves clocks, rails, GPIO, sensor and I2C ownership. Before
     * any hardware change it publishes a nonzero token, retained on failure.
     * READY means stable 24 MHz XCLK, correct module rails, physical PWDN high
     * and, unless ov5640_pwdn_only was called, RESETB low. The module adapter
     * must establish the actual pinout and voltage levels; the board's
     * connector 3.3 V is not a bare-sensor rail. Subsequent calls continue
     * the same token. No changing token on retry.
     */
    int (*acquire)(void *context, uint64_t deadline, uint64_t *token);
    /* asserted means PWDN high or RESETB low; adapter handles polarity. */
    int (*line)(void *context, uint64_t token, enum ov5640_line line,
                int asserted, uint64_t deadline);
    /* Sensor SCCB/I2C: 7-bit address 0x3c. Registers are 16-bit big endian;
     * reads send two address bytes, then read one byte with a repeated start.
     * Writes send two address bytes and one value. No implicit retries.
     */
    int (*read)(void *context, uint64_t token, uint8_t address,
                uint16_t reg, uint8_t *value, uint64_t deadline);
    int (*write)(void *context, uint64_t token, uint8_t address,
                 uint16_t reg, uint8_t value, uint64_t deadline);
    /* Validate and retain a caller-owned receiver capability. Success proves
     * D-PHY/CSI routing, mode, capture memory and exclusive ownership ready.
     * The caller keeps it reserved after stream stop until receiver drain.
     * No capability or receiver memory is released by this component.
     */
    int (*receiver)(void *context, uint64_t sensor_token,
                    uint64_t receiver_token, const struct ov5640_mode *mode,
                    uint64_t deadline);
    /* Verify receiver isolation/drain, assert PWDN/RESETB, disable module
     * rails/XCLK safely and release resources. Pending retains all resources;
     * failure retains ownership even if shutdown partly succeeded. Success
     * irrevocably releases the lease; an overdue success still faults locally.
     */
    int (*release)(void *context, uint64_t token, uint64_t deadline);
    /* Optional. One SCCB write of count (2..OV5640_BLOCK_BYTES) values to
     * consecutive registers from reg, relying on the sensor's register-address
     * auto-increment. Same retention rules as write. NULL keeps one write per
     * register. Masked, delayed and test-pattern entries always use write.
     */
    int (*write_block)(void *context, uint64_t token, uint8_t address,
                       uint16_t reg, const uint8_t *values, unsigned count,
                       uint64_t deadline);
};
#define OV5640_BLOCK_BYTES 40u
struct ov5640 {
    struct ov5640_ops ops;
    void *context;
    uint64_t token, receiver_token, deadline, now, wait_until;
    struct ov5640_mode mode;
    enum ov5640_state state;
    uint16_t index;
    uint8_t phase, saved, test_pattern, pwdn_only, initialized;
    uint8_t pll[3], band[6]; /* 0x3035, 0x3036, 0x4837; AEC band steps. */
};
/* All invalid requests return ERROR without callbacks or changes. */
int ov5640_init(struct ov5640 *, const struct ov5640_ops *, void *context,
                int color_bars);
/* Before open, select a module whose RESETB has no separate control. The
 * driver then never drives the RESET line; the 1 ms and 20 ms waits after PWDN
 * release are unchanged. Linux uses the same sequence without a reset GPIO. */
int ov5640_pwdn_only(struct ov5640 *);
/* Before open, replace the default VGA mode with one from ov5640_mode_get.
 * The receiver callback is then given this mode. */
int ov5640_select(struct ov5640 *, const struct ov5640_mode *);
int ov5640_open(struct ov5640 *, uint64_t deadline);
int ov5640_start(struct ov5640 *, uint64_t receiver_token, uint64_t deadline);
int ov5640_stop(struct ov5640 *, uint64_t deadline);
int ov5640_close(struct ov5640 *, uint64_t deadline);
/* At most one platform/I2C operation per poll, plus pre/post clock reads.
 * A masked register update consumes separate read and write polls. With
 * write_block, one poll writes a run of plain consecutive table entries.
 * Fault is terminal and performs no further callbacks.
 */
int ov5640_poll(struct ov5640 *);
#endif
