/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register sequencing adapted from the published display driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2016 Allwinner.
 * Copyright (C) 2023 Allwinner.
 */
#include "a733_dsi_phy.h"
#include <stddef.h>
#include <string.h>

#define FACTORS UINT32_C(0x3f0fff3f)
#define PLL_ENABLE UINT32_C(0x00700000)
#define PLL_GATES UINT32_C(0x00601000)
#define LOCK UINT32_C(0x04000000)

int a733_dsi_pll_plan(uint32_t hs, uint32_t ls, struct a733_dsi_pll_plan *out)
{
    if (!out || !hs || !ls || hs > 1000000000u || ls > 1200000000u) return 0;
    uint32_t hdiv = (uint32_t)((UINT64_C(1260000000) + hs - 1) / hs);
    if (!hdiv || hdiv > 16) return 0;
    uint64_t vco = (uint64_t)hs * hdiv;
    if (vco < 1260000000u || vco > 2400000000u || vco % 24000000u || vco % ls) return 0;
    uint32_t ldiv = (uint32_t)(vco / ls);
    uint32_t m2 = !(ldiv % 3) ? 3 : !(ldiv % 2) ? 2 : 1;
    uint32_t m3 = ldiv / m2;
    if (!m3 || m3 > 16) return 0;
    struct a733_dsi_pll_plan p = {.hs_hz = hs, .ls_hz = ls, .vco_hz = (uint32_t)vco,
        .factors = (hdiv - 1) | ((uint32_t)(vco / 24000000u) << 8) |
                   ((m3 - 1) << 24) | ((m2 - 1) << 28)};
    *out = p;
    return 1;
}

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y || (x < y + bn && y < x + an);
}

int a733_dsi_phy_init(struct a733_dsi_phy *p, const struct a733_dsi_phy_config *c)
{
    struct a733_dsi_pll_plan plan;
    if (!p || !c || overlap(p, sizeof(*p), c, sizeof(*c)) || p->initialized ||
        !c->lease || !c->deadline || c->controller > 1 || !c->lanes || c->lanes > 4 ||
        !c->ops.admit || !c->ops.read32 || !c->ops.write32 ||
        !c->ops.barrier || !c->ops.now_us || !a733_dsi_pll_plan(c->hs_hz, c->ls_hz, &plan)) return 0;
    memset(p, 0, sizeof(*p));
    p->config = *c;
    p->pll = plan;
    p->initialized = 1;
    return 1;
}

enum space { PHY, HOST, CCU };
struct field { uint16_t offset; uint8_t space; uint32_t mask, value; };
static const struct field baseline[] = {
    {0x1580, CCU, 0x8700001f, 0x82000000}, /* PERI0_150M, divide by one. */
    {0x1584, CCU, 0x00010001, 0x00010001},
    {0x104, PHY, FACTORS | PLL_ENABLE | 0x40, PLL_ENABLE}, /* Add exact factors. */
    {0x108, PHY, PLL_GATES, PLL_GATES},
    {0x10c, PHY, 0xe0000000, 0},
    {0x130, PHY, 0x80000000, 0},
    {0x134, PHY, 0x00020000, 0},
    {0x138, PHY, 0x80000000, 0},
    {0, PHY, 1, 0},
    {4, PHY, 0x331f1f1f, 0},
    {0xe4, PHY, 3, 0},
    {0x54, PHY, 0x0fff6f77, 0},
    {0x58, PHY, 0xff077fff, 0},
    {0x5c, PHY, 0x80000000, 0},
    {0x110, PHY, 0x3f, 0},
    {0x60, PHY, UINT32_MAX, 0},
    {0x64, PHY, UINT32_MAX, 0},
    {0x68, PHY, UINT32_MAX, 0},
    {0, HOST, 1, 0},
    {0x10, HOST, 0x1431, 0},
    {4, HOST, 0xffff, 0},
    {0x0c, HOST, 0x100, 0},
};

/* Preserve the source's analog power ordering. Consecutive fields in the same
 * word are combined. The one-microsecond delay follows entry 8. */
static const struct field analog[] = {
    {0x10, PHY, 0xffff00ff, 0x0906000e},
    {0x5c, PHY, 0x77f777ff, 0x64b4327f},
    {0x54, PHY, 0x12, 0x12},
    {0x58, PHY, 0x03040000, 0x03040000},
    {0x4c, PHY, 0x00ffff77, 0x00777777},
    {0x110, PHY, 0x3b, 0x0b},
    {0x5c, PHY, 0x80000000, 0x80000000},
    {0x118, PHY, 0xff, 20},
    {0x58, PHY, 0x0c000000, 0x0c000000},
    {0x54, PHY, 0x10, 0x10},
    {0x50, PHY, 0x80000000, 0x80000000},
    {0, PHY, 1, 1},
    {0, PHY, 1, 0},
    {4, PHY, 0x10000000, 0x10000000},
    {0x14, PHY, UINT32_MAX, 0x0a033207},
    {0x18, PHY, 0x10ffffff, 30},
    {0x1c, PHY, 0x000fffff, 0},
    {0x20, PHY, 0xffff, 0x0303},
    {0, PHY, 1, 1},
    {0x58, PHY, 0xf0000000, 0}, /* Add active lane mask. */
    {0x54, PHY, 0x0f000000, 0},
    {0, PHY, 0x30, 0},
};

/* Only LP command instructions are prepared. Video/HS instructions and pixel
 * registers remain outside this controller's implementation. */
static const struct field host[] = {
    {0x10, HOST, 0x30000, 0x30000},
    {0x14, HOST, 1, 0},
    {0x60, HOST, 0x1fff, 10},
    {0x78, HOST, 0xffff, 0},
    {0x20, HOST, 0xffff001f, 0x10}, /* Add active lane mask. */
    {0x30, HOST, 0xffff001f, 0x30100001},
    {0x40, HOST, 0x000f000f, 2},
    {0x44, HOST, 0xfff, 49},
    {0x54, HOST, 0xfff, 49},
    {0x4c, HOST, 0x10ffffff, 0x10560001},
    {0x50, HOST, 0x10000000, 0},
    {0x2f8, HOST, 0xffffff, 0xff},
    {0, HOST, 1, 1},
};

static uintptr_t address(const struct a733_dsi_phy *p, const struct field *f)
{
    uintptr_t base = f->space == CCU ? 0x02002000u :
                     f->space == PHY ? 0x05507000u : 0x05506000u;
    return base + f->offset + p->config.controller * (f->space == CCU ? 8u : 0x2000u);
}
static void fail(struct a733_dsi_phy *p, enum a733_dsi_phy_error e)
{
    p->state = A733_DSI_PHY_RETAINED;
    p->error = e;
}
static int clock_ok(struct a733_dsi_phy *p)
{
    uint64_t n = 0;
    if (p->config.ops.now_us(p->config.context, &n) != 1 ||
        n < p->last_now || n >= p->config.deadline) {
        fail(p, A733_DSI_PHY_TIME);
        return 0;
    }
    p->last_now = n;
    return p->state == A733_DSI_PHY_PENDING;
}
static void wait_for(struct a733_dsi_phy *p, unsigned us)
{
    if (p->last_now > UINT64_MAX - us) fail(p, A733_DSI_PHY_TIME);
    else p->wait_until = p->last_now + us;
}
static uint32_t field_value(const struct a733_dsi_phy *p, const struct field *f)
{
    uint32_t value = f->value, lanes = (1u << p->config.lanes) - 1;
    if (f == &baseline[2]) value |= p->pll.factors;
    if (f == &analog[19]) value = lanes << 28;
    if (f == &analog[20]) value = lanes << 24;
    if (f == &analog[21]) value = (p->config.lanes - 1) << 4;
    if (f == &host[4]) value |= lanes;
    return value;
}
enum { ADMIT, BASELINE, FILTER_LOCK, LOCK_SETTLE, ANALOG, HOST_SETUP,
       FINISH_CLOCKS, FINISH_LOCK, FINISH_BARRIER };

enum a733_dsi_phy_state a733_dsi_phy_poll(struct a733_dsi_phy *p)
{
    if (!p || !p->initialized) return A733_DSI_PHY_RETAINED;
    if (p->state != A733_DSI_PHY_PENDING) return p->state;
    if (p->entered) { fail(p, A733_DSI_PHY_REENTRY); return p->state; }
    p->entered = 1;
    if (!clock_ok(p)) goto out;
    if (p->wait_until && p->last_now < p->wait_until) goto out;
    p->wait_until = 0;
    if (p->phase == LOCK_SETTLE) { p->phase = ANALOG; p->index = 0; }
    const struct field *f = NULL;
    uint32_t value = 0;
    unsigned phase = p->phase;
    int ok = 0;
    void *ctx = p->config.context;
    if (phase == ADMIT) ok = p->config.ops.admit(ctx, &p->config);
    else if (phase == BASELINE || phase == FINISH_CLOCKS) {
        f = &baseline[p->index];
        ok = p->config.ops.read32(ctx, address(p, f), &value);
    } else if (phase == FILTER_LOCK || phase == FINISH_LOCK) {
        uintptr_t a = 0x055070e0u + p->config.controller * 0x2000u;
        ok = p->config.ops.read32(ctx, a, &value);
    } else if (phase == ANALOG || phase == HOST_SETUP) {
        f = phase == ANALOG ? &analog[p->index] : &host[p->index];
        if (phase == HOST_SETUP && p->index == 12 && p->substep == 0) {
            ok = p->config.ops.barrier(ctx);
        } else if (p->substep == 1) {
            ++p->writes_attempted;
            ok = p->config.ops.write32(ctx, address(p, f), p->expected);
        } else ok = p->config.ops.read32(ctx, address(p, f), &value);
    } else if (phase == FINISH_BARRIER) ok = p->config.ops.barrier(ctx);
    else { fail(p, A733_DSI_PHY_INITIAL_STATE); goto out; }
    if (p->state != A733_DSI_PHY_PENDING) goto out;
    if (ok != 1) { fail(p, phase == ADMIT ? A733_DSI_PHY_AUTHORITY : A733_DSI_PHY_IO); goto out; }
    if (!clock_ok(p)) goto out;
    if (phase == ADMIT) { p->phase = BASELINE; p->index = 0; }
    else if (phase == BASELINE || phase == FINISH_CLOCKS) {
        if ((value & f->mask) != field_value(p, f)) {
            fail(p, phase == BASELINE ? A733_DSI_PHY_INITIAL_STATE : A733_DSI_PHY_VERIFY);
            goto out;
        }
        if (++p->index == (phase == BASELINE ? sizeof(baseline)/sizeof(*baseline) : 8)) {
            p->phase = phase == BASELINE ? FILTER_LOCK : FINISH_LOCK;
            p->locks = 0;
        }
    } else if (phase == FILTER_LOCK || phase == FINISH_LOCK) {
        p->locks = value & LOCK ? p->locks + 1 : 0;
        if (p->locks == 3) {
            p->phase = phase == FILTER_LOCK ? LOCK_SETTLE : FINISH_BARRIER;
            wait_for(p, 20);
        } else wait_for(p, 5);
    } else if (phase == ANALOG || phase == HOST_SETUP) {
        /* The host enable has a separate barrier before its RMW read. */
        if (phase == HOST_SETUP && p->index == 12 && p->substep == 0) {
            p->substep = 3;
        } else if (p->substep == 0 || p->substep == 3) {
            p->previous = value;
            p->expected = (value & ~f->mask) | field_value(p, f);
            p->substep = 1;
        } else if (p->substep == 1) p->substep = 2;
        else {
            if (value != p->expected) { fail(p, A733_DSI_PHY_VERIFY); goto out; }
            p->substep = 0;
            if (phase == ANALOG && p->index == 7) wait_for(p, 1);
            if (++p->index == (phase == ANALOG ? sizeof(analog)/sizeof(*analog) : sizeof(host)/sizeof(*host))) {
                p->phase = phase == ANALOG ? HOST_SETUP : FINISH_CLOCKS;
                p->index = 0;
            }
        }
    } else p->state = A733_DSI_PHY_READY;
out:
    p->entered = 0;
    return p->state;
}
int a733_dsi_phy_ready(const struct a733_dsi_phy *p, unsigned controller,
                       unsigned lanes, uint64_t lease)
{
    return p && p->initialized && !p->entered && p->state == A733_DSI_PHY_READY &&
           p->config.controller == controller && p->config.lanes == lanes &&
           p->config.lease == lease && lease;
}
