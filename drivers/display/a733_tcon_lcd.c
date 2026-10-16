/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Timing and register sequences adapted from the published display driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2016 Allwinner.
 */
#include "a733_tcon_lcd.h"
#include <stddef.h>
#include <string.h>
#define LCD A733_TCON_LCD1_BASE
#define TOP A733_TCON_TOP_BASE

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y || (x < y + bn && y < x + an);
}
static void word(struct a733_tcon_lcd_plan *p, uintptr_t address, uint32_t mask, uint32_t value)
{
    p->words[p->count++] = (struct a733_tcon_lcd_word){address, mask, value};
}
int a733_tcon_lcd_plan(const struct a733_tcon_lcd_timing *t, struct a733_tcon_lcd_plan *out)
{
    if (!t || !out || overlap(t, sizeof(*t), out, sizeof(*out)) ||
        t->actual_dclk_hz != t->dsi.ls_hz || t->dsi.height > 1080 ||
        t->dsi.vback < 4 || t->dsi.vfront < 2) return 0;
    struct a733_dsi_video_plan dsi;
    if (!a733_dsi_video_plan(&t->dsi, &dsi)) return 0;
    uint32_t hb = t->dsi.hback + t->dsi.hsync - 1u;
    uint32_t vb = t->dsi.vback + t->dsi.vsync - 1u;
    uint32_t blank = dsi.htotal - t->dsi.width;
    uint32_t delay = 3 * dsi.htotal / 8 - 1;
    uint32_t safe = (uint32_t)((uint64_t)t->actual_dclk_hz * 15 / 1000000);
    if (hb > 4095 || vb > 4095 || dsi.vtotal > 4095 || t->dsi.hsync > 1024 ||
        t->dsi.vsync > 1024 || blank <= 60 || blank - 40 > 4095 || delay > 65535 || safe > 8191) return 0;
    struct a733_tcon_lcd_plan p = {.block_space = blank - 40, .start_delay = delay, .safe_fifo = safe};
    word(&p, TOP + 4, 0x10, 0); /* Manual: zero selects LCD1, one selects LCD0. */
    word(&p, TOP + 0xc, 2, 2); /* PHY source bit5 is already authenticated. */
    word(&p, LCD + 0x44, 0x7f, 1);
    word(&p, LCD + 0x48, 0x0fff0fff, ((t->dsi.width - 1u) << 16) | (t->dsi.height - 1u));
    word(&p, LCD + 0x4c, 0x1fff0fff, ((dsi.htotal - 1) << 16) | hb);
    word(&p, LCD + 0x50, 0x1fff0fff, ((2 * dsi.vtotal) << 16) | vb);
    word(&p, LCD + 0x54, 0x03ff03ff, ((t->dsi.hsync - 1u) << 16) | (t->dsi.vsync - 1u));
    word(&p, LCD + 0x40, 0x038001f7, 0x01000020); /* DSI CPU interface, DE source. */
    word(&p, LCD + 0x60, 0xf6030005, 0x10010004); /* Flush/FIFO ready; trigger disabled. */
    word(&p, LCD + 0x160, 0x0fff0fff, ((blank - 40) << 16) | (t->dsi.width - 1u));
    word(&p, LCD + 0x164, 0xffff, t->dsi.height - 1u);
    word(&p, LCD + 0x168, UINT32_MAX, (delay << 16) | 10);
    word(&p, LCD + 0x1f0, 0x1fff0007, (safe << 16) | 3);
    word(&p, LCD + 0x8c, 0x1fffffff, 0);
    *out = p;
    return 1;
}
int a733_tcon_lcd_init(struct a733_tcon_lcd *d, const struct a733_tcon_lcd_config *c, uint64_t deadline)
{
    if (!d || !c || overlap(d, sizeof(*d), c, sizeof(*c)) || d->initialized || !deadline ||
        !c->lease || !c->ops.admit || !c->ops.read32 || !c->ops.write32 || !c->ops.barrier || !c->ops.now_us) return 0;
    struct a733_tcon_lcd_plan p;
    if (!a733_tcon_lcd_plan(&c->timing, &p)) return 0;
    memset(d, 0, sizeof(*d));
    d->config = *c; d->plan = p; d->deadline = deadline; d->initialized = 1;
    return 1;
}
int a733_tcon_lcd_request(struct a733_tcon_lcd *d, enum a733_tcon_lcd_action action, uint64_t deadline)
{
    if (!d || !d->initialized || d->entered || deadline <= d->last_now ||
        !((action == A733_TCON_START && d->state == A733_TCON_READY) ||
          ((action == A733_TCON_OBSERVE || action == A733_TCON_STOP) && d->state == A733_TCON_ACTIVE))) return 0;
    d->action = action; d->deadline = deadline; d->state = A733_TCON_PENDING;
    d->step = d->index = d->substep = 0;
    return 1;
}
static void fail(struct a733_tcon_lcd *d, enum a733_tcon_lcd_error e)
{
    d->state = A733_TCON_RETAINED; d->error = e;
}
static int time_ok(struct a733_tcon_lcd *d)
{
    uint64_t n = 0;
    if (d->config.ops.now_us(d->config.context, &n) != 1 || n < d->last_now || n >= d->deadline) {
        fail(d, A733_TCON_TIME); return 0;
    }
    d->last_now = n;
    return d->state == A733_TCON_PENDING;
}
static const struct a733_tcon_lcd_word cold[] = {
    {LCD, 0xc0000003, 0}, {LCD+4, UINT32_MAX, 0},
    {LCD+0x40, 0x93f001f7, 0}, {LCD+0x44, 0xf0000000, 0},
    {LCD+0x60, 0xf7c3003f, 0}, {LCD+0x10, 0x80000003, 0},
    {LCD+0x3c, 0x80000003, 0}, {LCD+0x84, 0x80000000, 0},
    {LCD+0x88, UINT32_MAX, 0}, {LCD+0xfc, 0x8000c000, 0},
    {LCD+0x100, 0x80000000, 0}, {LCD+0x16c, 0x30000000, 0},
    {LCD+0x170, 0x10000000, 0}, {LCD+0x228, 1, 0},
    {LCD+0x230, 0x111, 0}, {TOP+0xc, 0x20, 0x20}, {TOP+0x20, 0x20000, 0}
};
static const struct a733_tcon_lcd_word ready[] = {
    {LCD, 0x80000000, 0}, {LCD+0x40, 0x93f001f7, 0x01000020},
    {LCD+0x44, 0xf000007f, 1}, {LCD+0x60, 0xf7c3003f, 0x10010004},
    {TOP+4, 0x10, 0}, {TOP+0xc, 0x22, 0x22}, {TOP+0x20, 0x20000, 0},
    {LCD+4, 0xffff0200, 0}
};
static const struct a733_tcon_lcd_word active[] = {
    {LCD, 0x80000000, 0x80000000}, {LCD+0x40, 0x93f001f7, 0x81000020},
    {LCD+0x44, 0xf000007f, 0x10000001}, {LCD+0x60, 0xf703003d, 0x10010005},
    {TOP+4, 0x10, 0}, {TOP+0xc, 0x22, 0x22}, {TOP+0x20, 0x20000, 0x20000},
    {LCD+4, 0xffff0000, 0}
};
static const struct a733_tcon_lcd_word start[] = {
    {LCD, 0x80000000, 0x80000000}, {TOP+0x20, 0x20000, 0x20000},
    {LCD+0x44, 0xf0000000, 0x10000000}, {LCD+0x60, 1, 1},
    {LCD+0x40, 0x80000000, 0x80000000}
};
static const struct a733_tcon_lcd_word stop[] = {
    {LCD+0x40, 0x80000000, 0}, {LCD, 0x80000000, 0},
    {LCD+0x44, 0xf0000000, 0}, {TOP+0x20, 0x20000, 0}
};
#define COUNT(a) (sizeof(a) / sizeof(*(a)))
static uint32_t readback_mask(uintptr_t address)
{
    /* Never reflect read-only counters or R/W1S trigger-start into a write. */
    if (address == LCD + 0x60) return ~UINT32_C(0x00c00002);
    if (address == LCD + 0x164) return 0xffff;
    return UINT32_MAX;
}
enum a733_tcon_lcd_state a733_tcon_lcd_poll(struct a733_tcon_lcd *d)
{
    if (!d || !d->initialized) return A733_TCON_RETAINED;
    if (d->state != A733_TCON_PENDING) return d->state;
    if (d->entered) { fail(d, A733_TCON_REENTRY); return d->state; }
    d->entered = 1;
    if (!time_ok(d)) goto out;
    const struct a733_tcon_lcd_word *checks = d->action == A733_TCON_PREPARE ? cold :
        d->action == A733_TCON_START ? ready : active;
    unsigned nchecks = d->action == A733_TCON_PREPARE ? COUNT(cold) : COUNT(ready);
    const struct a733_tcon_lcd_word *writes = d->action == A733_TCON_PREPARE ? d->plan.words :
        d->action == A733_TCON_START ? start : stop;
    unsigned nwrites = d->action == A733_TCON_PREPARE ? d->plan.count :
        d->action == A733_TCON_START ? COUNT(start) : COUNT(stop);
    uint32_t value = 0;
    const struct a733_tcon_lcd_word *w = NULL;
    void *ctx = d->config.context;
    unsigned step = d->step;
    int ok = 0;
    if (!step) ok = d->config.ops.admit(ctx, &d->config, d->action);
    else if (step == 1) {
        w = &checks[d->index]; ok = d->config.ops.read32(ctx, w->address, &value);
    } else if (step == 2) {
        w = &writes[d->index];
        if (d->substep == 1) {
            ++d->writes_attempted; ok = d->config.ops.write32(ctx, w->address, d->expected);
        } else ok = d->config.ops.read32(ctx, w->address, &value);
    } else if (step == 3 || step == 6) ok = d->config.ops.barrier(ctx);
    else if (step == 4 || step == 5) ok = d->config.ops.read32(ctx, LCD + (step == 4 ? 0xfc : 4), &value);
    else { fail(d, A733_TCON_INITIAL_STATE); goto out; }
    if (d->state != A733_TCON_PENDING) goto out;
    if (ok != 1) { fail(d, step ? A733_TCON_IO : A733_TCON_AUTHORITY); goto out; }
    if (!time_ok(d)) goto out;
    if (step == 1) {
        if ((value & w->mask) != w->value) { fail(d, A733_TCON_INITIAL_STATE); goto out; }
        if (++d->index < nchecks) goto out;
        d->index = 0;
        if (d->action == A733_TCON_OBSERVE) { d->step = 4; goto out; }
    } else if (step == 2) {
        if (!d->substep) d->expected = ((value & ~w->mask) | w->value) & readback_mask(w->address);
        if (d->substep == 2 && ((value ^ d->expected) & readback_mask(w->address))) {
            fail(d, A733_TCON_VERIFY); goto out;
        }
        if (++d->substep < 3) goto out;
        d->substep = 0;
        if (++d->index < nwrites) goto out;
    } else if (step == 3 && d->action == A733_TCON_PREPARE) d->state = A733_TCON_READY;
    else if (step == 4) {
        d->debug_status = value;
        if (value & 0x80000000) { fail(d, A733_TCON_UNDERFLOW); goto out; }
    } else if (step == 5) {
        d->irq_status = value;
        if (value & 0xffff0200) { fail(d, A733_TCON_UNDERFLOW); goto out; }
    } else if (step == 6) d->state = d->action == A733_TCON_STOP ? A733_TCON_STOPPED : A733_TCON_ACTIVE;
    ++d->step;
out:
    d->entered = 0;
    return d->state;
}
