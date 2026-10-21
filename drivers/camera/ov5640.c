/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register programming adapted from Linux drivers/media/i2c/ov5640.c.
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2014-2017 Mentor Graphics Inc.
 */
#include "ov5640.h"
#include "a733_camera_common.h"
#include <stddef.h>
#include <stdint.h>
#include "ov5640_tables.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define ADDRESS 0x3cu
const struct ov5640_mode ov5640_vga_mode = {
    24000000, 192000000, 48000000, 640, 480, 1600, 1000, 2, 0, 0x1e, 2
};
/* Keep packets disabled even while the initialization table exits standby. */
static const struct ov5640_reg prepare[] = {
    {0x3008, 0x42, 0, 0}, {0x300e, 0x40, 0, 0}, {0x4202, 0x0f, 0, 0}
};
/* 24 MHz / 3 * 48 = 384 MHz PLL; sysdiv=1, MIPI divider=2.
 * Root /2, bit divider /2 and SCLK /2 give 48 MHz pixel timing.
 * MIPI link clock is 192 MHz. Each data lane transfers at 384 Mbit/s.
 */
static const struct ov5640_reg clocks[] = {
    {0x3108, 0x01, 0x3f, 0}, {0x3503, 0x03, 0x03, 0},
    {0x3034, 0x08, 0x0f, 0}, {0x3035, 0x12, 0xff, 0},
    {0x3036, 0x30, 0xff, 0}, {0x3037, 0x13, 0x1f, 0},
    {0x3108, 0x01, 0x3f, 0}, {0x4837, 0x14, 0, 0}
};
static const struct ov5640_reg timings_controls[] = {
    /* CSI-2 crop and timing from the reference VGA mode. */
    {0x3800, 0x00, 0, 0}, {0x3801, 0x10, 0, 0},
    {0x3802, 0x00, 0, 0}, {0x3803, 0x0e, 0, 0},
    {0x3804, 0x0a, 0, 0}, {0x3805, 0x2f, 0, 0},
    {0x3806, 0x07, 0, 0}, {0x3807, 0xa5, 0, 0},
    {0x3810, 0x00, 0, 0}, {0x3811, 0x02, 0, 0},
    {0x3812, 0x00, 0, 0}, {0x3813, 0x04, 0, 0},
    {0x3808, 0x02, 0, 0}, {0x3809, 0x80, 0, 0},
    {0x380a, 0x01, 0, 0}, {0x380b, 0xe0, 0, 0},
    {0x380c, 0x06, 0, 0}, {0x380d, 0x40, 0, 0},
    {0x380e, 0x03, 0, 0}, {0x380f, 0xe8, 0, 0},
    {0x3503, 0x00, 0x03, 0}, {0x3821, 0x01, 0x01, 0},
    {0x3820, 0x01, 0x01, 0},
    /* AE target 52: high=56, low=47, fast_high=112, fast_low=23. */
    {0x3a0f, 56, 0, 0}, {0x3a10, 47, 0, 0},
    {0x3a1b, 56, 0, 0}, {0x3a1e, 47, 0, 0},
    {0x3a11, 112, 0, 0}, {0x3a1f, 23, 0, 0},
    /* 60 Hz step=250, max=3; 50 Hz step=300, max=3. */
    {0x3a0a, 0, 0, 0}, {0x3a0b, 250, 0, 0}, {0x3a0d, 3, 0, 0},
    {0x3a08, 1, 0, 0}, {0x3a09, 44, 0, 0}, {0x3a0e, 3, 0, 0},
    {0x4814, 0, 0xc0, 0},
    /* UYVY8, JPEG reset/clocks disabled, sensor automatic white balance. */
    {0x4300, 0x3f, 0, 0}, {0x501f, 0, 0, 0},
    {0x3821, 0, 0x20, 0}, {0x3002, 0x1c, 0x1c, 0},
    {0x3006, 0, 0x28, 0}, {0x3406, 0, 1, 0},
    {0x3821, 6, 6, 0}, {0x3820, 0, 6, 0},
    {0x3c01, 0x80, 0x80, 0}, {0x3c00, 4, 4, 0},
    /* Value at 503d is selected from the reference color-bar control. */
    {0x503d, 0, 0, 0},
    {0x300e, 0x44, 0, 0}, {0x4800, 0x24, 0, 0}, {0x3019, 0x70, 0, 0}
};
static const struct ov5640_reg verify[] = {
    {0x3034, 8, 15, 0}, {0x3035, 0x12, 255, 0},
    {0x3036, 48, 255, 0}, {0x3037, 0x13, 31, 0},
    {0x3108, 1, 63, 0}, {0x4837, 20, 255, 0},
    {0x3808, 2, 255, 0}, {0x3809, 128, 255, 0},
    {0x380a, 1, 255, 0}, {0x380b, 224, 255, 0},
    {0x380c, 6, 255, 0}, {0x380d, 64, 255, 0},
    {0x380e, 3, 255, 0}, {0x380f, 232, 255, 0},
    {0x4300, 0x3f, 255, 0}, {0x501f, 0, 255, 0},
    {0x4814, 0, 0xc0, 0}, {0x300e, 0x44, 255, 0},
    {0x4202, 0x0f, 255, 0}, {0x4800, 0x24, 255, 0},
    {0x3019, 0x70, 255, 0}, {0x503d, 0, 255, 0}
};
static int fault(struct ov5640 *s) { s->state = OV5640_FAULT; return -1; }
static int time_ok(struct ov5640 *s)
{
    return a733_camera_clock(s->ops.now_us, s->context, &s->now, s->deadline) ?
           1 : fault(s);
}
static int delay(struct ov5640 *s, uint64_t us)
{
    if (us > UINT64_MAX - s->now) return fault(s);
    s->wait_until = s->now + us;
    return 0;
}
int ov5640_init(struct ov5640 *s, const struct ov5640_ops *ops, void *context,
                int color_bars)
{
    if (!s || !ops || s->initialized || s->state != OV5640_NEW ||
        (color_bars != 0 && color_bars != 1) ||
        a733_camera_overlap(s, sizeof(*s), ops, sizeof(*ops)) ||
        (context && a733_camera_overlap(s, sizeof(*s), context, 1)) ||
        !ops->now_us || !ops->acquire || !ops->line || !ops->read ||
        !ops->write || !ops->receiver || !ops->release) return -1;
    *s = (struct ov5640){ .ops = *ops, .context = context,
                          .test_pattern = (uint8_t)color_bars, .initialized = 1 };
    return 1;
}
int ov5640_pwdn_only(struct ov5640 *s)
{
    if (!s || !s->initialized || s->state != OV5640_NEW) return -1;
    s->pwdn_only = 1;
    return 1;
}
static int begin(struct ov5640 *s, enum ov5640_state state, uint64_t deadline)
{
    if (!deadline || deadline <= s->now) return -1;
    s->state = state; s->deadline = deadline; s->index = 0; s->phase = 0;
    s->wait_until = 0;
    return 1;
}
int ov5640_open(struct ov5640 *s, uint64_t deadline)
{
    if (!s || !s->initialized || s->state != OV5640_NEW) return -1;
    return begin(s, OV5640_ACQUIRE, deadline);
}
int ov5640_start(struct ov5640 *s, uint64_t receiver, uint64_t deadline)
{
    if (!s || !s->initialized || s->state != OV5640_READY || !receiver ||
        (s->receiver_token && s->receiver_token != receiver) ||
        !deadline || deadline <= s->now) return -1;
    s->receiver_token = receiver;
    return begin(s, OV5640_RECEIVER, deadline);
}
int ov5640_stop(struct ov5640 *s, uint64_t deadline)
{
    if (!s || !s->initialized || s->state != OV5640_STREAMING) return -1;
    return begin(s, OV5640_STOPPING, deadline);
}
int ov5640_close(struct ov5640 *s, uint64_t deadline)
{
    if (!s || !s->initialized || s->state != OV5640_READY) return -1;
    return begin(s, OV5640_RELEASING, deadline);
}
static int write_reg(struct ov5640 *s, uint16_t reg, uint8_t value)
{
    return s->ops.write(s->context, s->token, ADDRESS, reg, value, s->deadline);
}
static int read_reg(struct ov5640 *s, uint16_t reg, uint8_t *value)
{
    return s->ops.read(s->context, s->token, ADDRESS, reg, value, s->deadline);
}
static const struct ov5640_reg *entry(unsigned i)
{
#define PART(a) if (i < COUNT(a)) return &a[i]; i -= COUNT(a)
    PART(prepare); PART(ov5640_init_setting); PART(clocks);
    PART(ov5640_setting_low_res); PART(timings_controls);
#undef PART
    return NULL;
}
/* Only an unmasked, undelayed entry with a fixed value may join a block. */
static int plain(const struct ov5640_reg *e)
{
    return e && !e->mask && !e->delay_ms && e->reg != 0x503d &&
           e != &timings_controls[COUNT(timings_controls) - 1];
}
/* Length of the run of plain entries at consecutive registers from index. */
static unsigned run(unsigned index)
{
    const struct ov5640_reg *first = entry(index), *e = first;
    unsigned n = 0;
    while (n < OV5640_BLOCK_BYTES && plain(e) && e->reg == first->reg + n)
        e = entry(index + ++n);
    return n;
}
static int configure(struct ov5640 *s)
{
    const struct ov5640_reg *e = entry(s->index);
    uint8_t value;
    int r;
    unsigned n = s->ops.write_block ? run(s->index) : 0;
    if (n > 1) {
        uint8_t values[OV5640_BLOCK_BYTES];
        for (unsigned i = 0; i < n; ++i) values[i] = entry(s->index + i)->value;
        r = s->ops.write_block(s->context, s->token, ADDRESS, e->reg, values, n,
                               s->deadline);
        if (r != 1 || time_ok(s) != 1) return fault(s);
        s->index = (uint16_t)(s->index + n);
        return 0;
    }
    if (!e) {
        unsigned i = s->index - (COUNT(prepare) + COUNT(ov5640_init_setting) +
            COUNT(clocks) + COUNT(ov5640_setting_low_res) + COUNT(timings_controls));
        if (i == COUNT(verify)) {
            s->state = OV5640_READY;
            return 1;
        }
        e = &verify[i];
        r = read_reg(s, e->reg, &value);
        if (r != 1 || time_ok(s) != 1) return fault(s);
        if ((value & e->mask) != (e->reg == 0x503d ?
                (s->test_pattern ? 0x84 : 0) : e->value)) return fault(s);
        ++s->index;
        return 0;
    }
    if (e->mask && !s->phase) {
        r = read_reg(s, e->reg, &value);
        if (r != 1 || time_ok(s) != 1) return fault(s);
        s->saved = (uint8_t)((value & ~e->mask) | (e->value & e->mask));
        s->phase = 1;
        return 0;
    }
    value = e->mask ? s->saved : e->value;
    if (e->reg == 0x503d) value = s->test_pattern ? 0x84 : 0;
    r = write_reg(s, e->reg, value);
    if (r != 1 || time_ok(s) != 1) return fault(s);
    ++s->index; s->phase = 0;
    if (e->delay_ms) return delay(s, (uint64_t)e->delay_ms * 1000);
    if (e == &timings_controls[COUNT(timings_controls)-1]) return delay(s, 500);
    return 0;
}
int ov5640_poll(struct ov5640 *s)
{
    int r;
    uint8_t value;
    uint64_t token;
    if (!s || !s->initialized || s->state == OV5640_FAULT) return -1;
    if (s->state == OV5640_READY || s->state == OV5640_STREAMING ||
        s->state == OV5640_CLOSED) return 1;
    if (s->state == OV5640_NEW) return -1;
    if (time_ok(s) != 1) return -1;
    if (s->wait_until) {
        if (s->now < s->wait_until) return 0;
        s->wait_until = 0;
    }
    switch (s->state) {
    case OV5640_ACQUIRE:
        token = s->token;
        r = s->ops.acquire(s->context, s->deadline, &token);
        if (s->token && token != s->token) return fault(s);
        if (!s->token) s->token = token;
        if (r < 0 || r > 1 || (r == 1 && !s->token) ||
            time_ok(s) != 1) return fault(s);
        if (!r) return 0;
        s->state = OV5640_POWER_WAIT;
        return delay(s, 5000);
    case OV5640_POWER_WAIT:
        s->state = OV5640_PWDN;
        return 0;
    case OV5640_PWDN:
        r = s->ops.line(s->context, s->token, OV5640_PWDN_LINE, 0, s->deadline);
        if (r != 1 || time_ok(s) != 1) return fault(s);
        s->state = OV5640_RESET_WAIT;
        return delay(s, 1000);
    case OV5640_RESET_WAIT:
        s->state = OV5640_RESET;
        return 0;
    case OV5640_RESET:
        if (!s->pwdn_only) {
            r = s->ops.line(s->context, s->token, OV5640_RESET_LINE, 0,
                            s->deadline);
            if (r != 1 || time_ok(s) != 1) return fault(s);
        }
        s->state = OV5640_BUS_WAIT;
        return delay(s, 20000);
    case OV5640_BUS_WAIT:
        s->state = OV5640_IDENTIFY;
        return 0;
    case OV5640_IDENTIFY:
        r = read_reg(s, (uint16_t)(0x300a + s->index), &value);
        if (r != 1 || time_ok(s) != 1 || value != (s->index ? 0x40 : 0x56))
            return fault(s);
        if (++s->index == 2) { s->index = 0; s->state = OV5640_CONFIGURE; }
        return 0;
    case OV5640_CONFIGURE:
        return configure(s);
    case OV5640_RECEIVER:
        r = s->ops.receiver(s->context, s->token, s->receiver_token,
                            &ov5640_vga_mode, s->deadline);
        if (r < 0 || r > 1 || time_ok(s) != 1) return fault(s);
        if (!r) return 0;
        s->state = OV5640_STARTING;
        return 0;
    case OV5640_STARTING:
    case OV5640_STOPPING: {
        int on = s->state == OV5640_STARTING;
        uint16_t reg = (s->index & 1) ? 0x4202 : 0x300e;
        uint8_t expected = (s->index & 1) ? (on ? 0 : 15) : (on ? 0x45 : 0x40);
        if (s->index < 2) r = write_reg(s, reg, expected);
        else {
            r = read_reg(s, reg, &value);
            if (r == 1 && value != expected) r = -1;
        }
        if (r != 1 || time_ok(s) != 1) return fault(s);
        if (++s->index == 4) {
            s->state = on ? OV5640_STREAMING : OV5640_READY;
            return 1;
        }
        return 0;
    }
    case OV5640_RELEASING:
        r = s->ops.release(s->context, s->token, s->deadline);
        if (r == 1) s->token = 0; /* A completed release cannot be undone. */
        if (r < 0 || r > 1 || time_ok(s) != 1) return fault(s);
        if (!r) return 0;
        s->state = OV5640_CLOSED;
        return 1;
    default:
        return fault(s);
    }
}
