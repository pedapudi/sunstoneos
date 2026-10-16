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
struct ov5640_mode {
    uint32_t xclk_hz, link_hz, pixel_hz;
    uint16_t width, height, htotal, vtotal;
    uint8_t lanes, virtual_channel, csi_data_type, bytes_per_pixel;
};
extern const struct ov5640_mode ov5640_vga_mode;

struct ov5640_ops {
    int (*now_us)(void *context, uint64_t *now);
    /* Acquire reserves clocks, rails, GPIO, sensor and I2C ownership. Before
     * any hardware change it publishes a nonzero token, retained on failure.
     * READY means stable 24 MHz XCLK, correct module rails, physical PWDN high
     * and RESETB low. The module adapter must establish the actual pinout and
     * voltage levels; the board's connector 3.3 V is not a bare-sensor rail.
     * Subsequent calls continue the same token. No changing token on retry.
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
};
struct ov5640 {
    struct ov5640_ops ops;
    void *context;
    uint64_t token, receiver_token, deadline, now, wait_until;
    enum ov5640_state state;
    uint16_t index;
    uint8_t phase, saved, test_pattern, initialized;
};
/* All invalid requests return ERROR without callbacks or changes. */
int ov5640_init(struct ov5640 *, const struct ov5640_ops *, void *context,
                int color_bars);
int ov5640_open(struct ov5640 *, uint64_t deadline);
int ov5640_start(struct ov5640 *, uint64_t receiver_token, uint64_t deadline);
int ov5640_stop(struct ov5640 *, uint64_t deadline);
int ov5640_close(struct ov5640 *, uint64_t deadline);
/* At most one platform/I2C operation per poll, plus pre/post clock reads.
 * A masked register update consumes separate read and write polls.
 * Fault is terminal and performs no further callbacks.
 */
int ov5640_poll(struct ov5640 *);
#endif
