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
#include <string.h>
#include "ov5640_tables.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define ADDRESS 0x3cu
const struct ov5640_mode ov5640_vga_mode = {
    24000000, 192000000, 48000000, 640, 480, 1600, 1000, 2, 0, 0x1e, 2,
    OV5640_VGA, OV5640_UYVY
};
/* CSI-2 timings of the reference mode table: analog crop, visible crop
 * offset, line length and default vertical blanking. Scaling modes disable
 * binning. Pixel rates are the reference starting rates. */
static const struct size {
    uint16_t width, height, left, top, analog_width, analog_height;
    uint16_t crop_left, crop_top, htot, vblank;
    uint32_t pixel_rate;
    uint8_t scaling;
    const struct ov5640_reg *table;
    uint8_t count;
} sizes[OV5640_SIZES] = {
#define LOW ov5640_setting_low_res, COUNT(ov5640_setting_low_res)
    {160, 120, 16, 14, 2592, 1944, 2, 4, 1600, 878, 48000000, 0, LOW},
    {176, 144, 16, 14, 2592, 1944, 2, 4, 1600, 854, 48000000, 0, LOW},
    {320, 240, 16, 14, 2592, 1944, 2, 4, 1600, 760, 48000000, 0, LOW},
    {640, 480, 16, 14, 2592, 1944, 2, 4, 1600, 520, 48000000, 0, LOW},
    {720, 480, 16, 14, 2592, 1944, 56, 60, 1896, 1206, 96000000, 0, LOW},
    {720, 576, 16, 14, 2592, 1944, 56, 6, 1896, 1110, 96000000, 0, LOW},
    {1024, 768, 0, 4, 2624, 1944, 16, 6, 1896, 918, 96000000, 0, LOW},
#undef LOW
    {1280, 720, 0, 250, 2624, 1456, 16, 4, 1600, 560, 124000000, 0,
     ov5640_setting_720P_1280_720, COUNT(ov5640_setting_720P_1280_720)},
    {1920, 1080, 336, 434, 1952, 1088, 16, 4, 2234, 24, 148000000, 1,
     ov5640_setting_1080P_1920_1080, COUNT(ov5640_setting_1080P_1920_1080)},
    {2592, 1944, 0, 0, 2624, 1952, 16, 4, 2844, 24, 168000000, 1,
     ov5640_setting_QSXGA_2592_1944, COUNT(ov5640_setting_QSXGA_2592_1944)}
};
/* FORMAT CONTROL00, ISP format mux and CSI-2 data type per format. */
static const uint8_t formats[OV5640_FORMATS][3] = {
    {0x3f, 0, 0x1e}, {0x30, 0, 0x1e}, {0x00, 3, 0x2a}
};
/* ov5640_calc_sys_clk: the first PLL output at or above rate, preferring the
 * closest, with prediv 3, multiplier 4..252 (even above 127) and sysdiv
 * 1..16, keeping the PLL at or below 1 GHz. Differences use the reference
 * driver's signed arithmetic. */
static uint64_t gap(uint64_t a, uint64_t b)
{
    int64_t d = (int64_t)(a - b);
    return d < 0 ? 0 - (uint64_t)d : (uint64_t)d;
}
static void sys_clk(uint64_t rate, uint8_t *mult, uint8_t *sysdiv)
{
    uint64_t best = UINT64_MAX;
    *mult = 1; *sysdiv = 1;
    for (unsigned d = 1; d <= 16; ++d)
        for (unsigned m = 4; m <= 252; ++m) {
            uint64_t pll = 24000000u / 3 * m, r;
            if (m > 127 && (m & 1)) continue;
            if (pll / 1000000 > 1000) break;
            r = pll / d;
            if (r < rate) continue;
            if (gap(rate, r) < gap(rate, best)) {
                best = r; *sysdiv = (uint8_t)d; *mult = (uint8_t)m;
            }
            if (r == rate) return;
        }
}
/* The reference link-rate choice (ov5640_update_pixel_rate), clock tree
 * (ov5640_set_mipi_pclk), timing clock (ov5640_get_sysclk) and banding
 * filter (ov5640_set_bandingfilter), for two lanes and 24 MHz XCLK. Both
 * YUV422 formats carry 16 bits per pixel and raw Bayer 8. */
static int derive(struct ov5640_mode *mode, uint8_t pll[3], uint8_t band[6],
                  unsigned size, unsigned format, unsigned vtotal)
{
    static const uint32_t rates[] = {
        168000000, 148000000, 124000000, 96000000, 48000000
    };
    if (size >= OV5640_SIZES || format >= OV5640_FORMATS) return 0;
    const struct size *z = &sizes[size];
    unsigned bpp = format == OV5640_SBGGR8 ? 8 : 16, id = 0, lanes = 2;
    if (!vtotal) vtotal = z->height + z->vblank;
    if (vtotal < z->height + 24u || vtotal > 3375) return 0;
    while (rates[id] != z->pixel_rate) ++id;
    uint64_t link;
    do link = (uint64_t)rates[id] * bpp / (2 * lanes);
    while (link >= 1000000000u && ++id < COUNT(rates));
    if (id == COUNT(rates)) return 0;
    unsigned mipi_div = link > 490000000u ? 1 : 2;
    uint8_t mult, sysdiv;
    sys_clk(link * mipi_div, &mult, &sysdiv);
    uint32_t sample = (uint32_t)(link * mipi_div * lanes * 2 / 16);
    pll[0] = (uint8_t)((sysdiv << 4) | mipi_div);
    pll[1] = mult;
    pll[2] = (uint8_t)(2000000000u / sample);
    /* Bit divider 2x = 4 (8-bit MIPI), PLL root divider 2, SCLK divider 2,
     * all in 10 kHz units as the reference computes them. */
    uint32_t vco = 2400u * mult / 3, sysclk = vco / sysdiv / 2 * 2 / 4 / 2;
    uint32_t step60 = sysclk * 100 / z->htot * 100 / 120, step50 = sysclk * 100 / z->htot;
    uint32_t vts = z->height + z->vblank;
    if (!step60 || !step50) return 0;
    band[0] = (uint8_t)(step60 >> 8); band[1] = (uint8_t)step60;
    band[2] = (uint8_t)((vts - 4) / step60);
    band[3] = (uint8_t)(step50 >> 8); band[4] = (uint8_t)step50;
    band[5] = (uint8_t)((vts - 4) / step50);
    /* With MIPI divider 1 the lanes run at half the computed link rate. */
    *mode = (struct ov5640_mode){
        24000000, (uint32_t)(mipi_div == 1 ? link / 2 : link), sysclk * 10000u, z->width, z->height, z->htot,
        (uint16_t)vtotal, (uint8_t)lanes, 0, formats[format][2], (uint8_t)(bpp / 8),
        (uint8_t)size, (uint8_t)format
    };
    return 1;
}
int ov5640_mode_get(struct ov5640_mode *mode, enum ov5640_size size,
                    enum ov5640_format format, unsigned vtotal)
{
    uint8_t pll[3], band[6];
    return mode && derive(mode, pll, band, size, format, vtotal) ? 1 : -1;
}
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
    return derive(&s->mode, s->pll, s->band, OV5640_VGA, OV5640_UYVY, 0) ? 1 : -1;
}
int ov5640_pwdn_only(struct ov5640 *s)
{
    if (!s || !s->initialized || s->state != OV5640_NEW) return -1;
    s->pwdn_only = 1;
    return 1;
}
int ov5640_select(struct ov5640 *s, const struct ov5640_mode *mode)
{
    struct ov5640_mode m;
    uint8_t pll[3], band[6];
    if (!s || !s->initialized || s->state != OV5640_NEW || !mode ||
        !derive(&m, pll, band, mode->size, mode->format, mode->vtotal) ||
        m.xclk_hz != mode->xclk_hz || m.link_hz != mode->link_hz ||
        m.pixel_hz != mode->pixel_hz || m.width != mode->width ||
        m.height != mode->height || m.htotal != mode->htotal ||
        m.lanes != mode->lanes || m.virtual_channel != mode->virtual_channel ||
        m.csi_data_type != mode->csi_data_type ||
        m.bytes_per_pixel != mode->bytes_per_pixel) return -1;
    s->mode = m;
    memcpy(s->pll, pll, sizeof(pll)); memcpy(s->band, band, sizeof(band));
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
/* Entry i of the configuration sequence. local reports whether it belongs
 * to a table of this file, whose clock, timing and format values follow
 * the selected mode; reference tables are written unchanged. */
static const struct ov5640_reg *entry(const struct ov5640 *s, unsigned i,
                                      unsigned *local)
{
    const struct size *z = &sizes[s->mode.size];
#define PART(a, n, l) if (i < (n)) { *local = (l); return &(a)[i]; } i -= (n)
    PART(prepare, COUNT(prepare), 1);
    PART(ov5640_init_setting, COUNT(ov5640_init_setting), 0);
    PART(clocks, COUNT(clocks), 1); PART(z->table, z->count, 0);
    PART(timings_controls, COUNT(timings_controls), 1);
#undef PART
    *local = 0;
    return NULL;
}
static unsigned entries(const struct ov5640 *s)
{
    return COUNT(prepare) + COUNT(ov5640_init_setting) + COUNT(clocks) +
           sizes[s->mode.size].count + COUNT(timings_controls);
}
/* Two bytes of a sixteen-bit timing register pair, high byte first. */
static uint8_t timing(const struct ov5640 *s, uint16_t reg)
{
    const struct size *z = &sizes[s->mode.size];
    uint16_t v = 0;
    switch (reg & ~1u) {
    case 0x3800: v = z->left; break;
    case 0x3802: v = z->top; break;
    case 0x3804: v = (uint16_t)(z->left + z->analog_width - 1); break;
    case 0x3806: v = (uint16_t)(z->top + z->analog_height - 1); break;
    case 0x3808: v = s->mode.width; break;
    case 0x380a: v = s->mode.height; break;
    case 0x380c: v = s->mode.htotal; break;
    case 0x380e: v = s->mode.vtotal; break;
    case 0x3810: v = z->crop_left; break;
    case 0x3812: v = z->crop_top; break;
    }
    return (uint8_t)(reg & 1 ? v : v >> 8);
}
/* The selected mode's value for a clock, timing, binning, banding or
 * format entry of a local table; every other entry keeps its own. */
static uint8_t value_of(const struct ov5640 *s, const struct ov5640_reg *e,
                        unsigned local)
{
    static const uint16_t bands[] = {0x3a0a, 0x3a0b, 0x3a0d, 0x3a08, 0x3a09, 0x3a0e};
    const struct size *z = &sizes[s->mode.size];
    if (!local) return e->value;
    if (e->reg == 0x3035) return s->pll[0];
    if (e->reg == 0x3036) return s->pll[1];
    if (e->reg == 0x4837) return s->pll[2];
    if (e->reg >= 0x3800 && e->reg <= 0x3813) return timing(s, e->reg);
    if ((e->reg == 0x3820 || e->reg == 0x3821) && e->mask == 1) return !z->scaling;
    for (unsigned i = 0; i < COUNT(bands); ++i)
        if (e->reg == bands[i]) return s->band[i];
    if (e->reg == 0x4300) return formats[s->mode.format][0];
    if (e->reg == 0x501f) return formats[s->mode.format][1];
    return e->value;
}
/* Only an unmasked, undelayed entry with a fixed value may join a block. */
static int plain(const struct ov5640_reg *e)
{
    return e && !e->mask && !e->delay_ms && e->reg != 0x503d &&
           e != &timings_controls[COUNT(timings_controls) - 1];
}
/* Length of the run of plain entries at consecutive registers from index. */
static unsigned run(const struct ov5640 *s, unsigned index)
{
    unsigned local;
    const struct ov5640_reg *first = entry(s, index, &local), *e = first;
    unsigned n = 0;
    while (n < OV5640_BLOCK_BYTES && plain(e) && e->reg == first->reg + n)
        e = entry(s, index + ++n, &local);
    return n;
}
static int configure(struct ov5640 *s)
{
    unsigned local;
    const struct ov5640_reg *e = entry(s, s->index, &local);
    uint8_t value;
    int r;
    unsigned n = s->ops.write_block ? run(s, s->index) : 0;
    if (n > 1) {
        uint8_t values[OV5640_BLOCK_BYTES];
        for (unsigned i = 0; i < n; ++i) {
            const struct ov5640_reg *b = entry(s, s->index + i, &local);
            values[i] = value_of(s, b, local);
        }
        r = s->ops.write_block(s->context, s->token, ADDRESS, e->reg, values, n,
                               s->deadline);
        if (r != 1 || time_ok(s) != 1) return fault(s);
        s->index = (uint16_t)(s->index + n);
        return 0;
    }
    if (!e) {
        unsigned i = s->index - entries(s);
        if (i == COUNT(verify)) {
            s->state = OV5640_READY;
            return 1;
        }
        e = &verify[i];
        r = read_reg(s, e->reg, &value);
        if (r != 1 || time_ok(s) != 1) return fault(s);
        if ((value & e->mask) != (e->reg == 0x503d ?
                (s->test_pattern ? 0x84 : 0) : value_of(s, e, 1))) return fault(s);
        ++s->index;
        return 0;
    }
    if (e->mask && !s->phase) {
        r = read_reg(s, e->reg, &value);
        if (r != 1 || time_ok(s) != 1) return fault(s);
        s->saved = (uint8_t)((value & ~e->mask) | (value_of(s, e, local) & e->mask));
        s->phase = 1;
        return 0;
    }
    value = e->mask ? s->saved : value_of(s, e, local);
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
                            &s->mode, s->deadline);
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
