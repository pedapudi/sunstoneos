/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register sequences adapted from the published display driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2016 Allwinner.
 */
#include "a733_dsi_video.h"
#include "a733_dsi_phy.h"
#include <stddef.h>
#include <string.h>

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y ||
           (x < y + bn && y < x + an);
}
static uint32_t header(unsigned type, unsigned count)
{
    static const uint32_t masks[] = {0xf12cb7, 0xf2555b, 0x749a6d,
                                     0xb8e38e, 0xdf03f0, 0xeffc00};
    uint32_t h = type | (count << 8), e = 0;
    for (unsigned i = 0; i != 6; ++i) {
        uint32_t x = h & masks[i];
        x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
        e |= (x & 1) << i;
    }
    return h | (e << 24);
}
static uint32_t zero_crc(unsigned count)
{
    uint32_t c = 0xffff;
    for (unsigned i = 0; i != count; ++i)
        for (unsigned j = 0; j != 8; ++j)
            c = (c >> 1) ^ ((c & 1) ? 0x8408 : 0);
    return c << 16;
}
static void word(struct a733_dsi_video_plan *p, unsigned offset, uint32_t mask, uint32_t value)
{
    p->words[p->count++] = (struct a733_dsi_video_word){offset, mask, value};
}
int a733_dsi_video_plan(const struct a733_dsi_video_timing *t, struct a733_dsi_video_plan *out)
{
    if (!t || !out || overlap(t, sizeof(*t), out, sizeof(*out)) || t->lanes != 4 ||
        (t->format != A733_DSI_RGB888 && t->format != A733_DSI_RGB666_LOOSE) ||
        (t->mode != A733_DSI_SYNC_PULSE && t->mode != A733_DSI_BURST) ||
        !t->pixel_hz || !t->width || t->width > 1920 || !t->height || t->height > 1200 ||
        !t->vsync || !t->vback || !t->vfront || t->vsync > 4095 || t->vback > 4095 ||
        t->hsync < 4 || t->hback < 5 || t->hfront < 20) return 0;
    uint32_t ht = (uint32_t)t->width + t->hsync + t->hback + t->hfront;
    uint32_t vt = (uint32_t)t->height + t->vsync + t->vback + t->vfront;
    if (ht > 8192 || vt > 8191) return 0;
    unsigned burst = t->mode == A733_DSI_BURST;
    uint64_t transport = t->pixel_hz;
    if (burst) {
        transport *= 10;
        if (transport % 9) return 0;
        transport /= 9;
    }
    struct a733_dsi_pll_plan pll;
    if (transport != t->ls_hz || transport * 6 != t->hs_hz || t->hs_hz % 8 ||
        !a733_dsi_pll_plan(t->hs_hz, t->ls_hz, &pll)) return 0;
    uint32_t counts[5] = {t->hsync * 3u - 10, t->hback * 3u - 10,
        t->hfront * 3u - 12, (ht - t->hsync) * 3u - 10, 4};
    uint32_t line = 0, edge0 = 0, edge1 = 0, drq = 0, tri = 0;
    if (burst) {
        counts[0] = counts[2] = counts[4] = 0;
        counts[1] = t->hback * 3u - 14;
        counts[3] = counts[1] + 6 + t->width * 3u;
        /* Published padding adds 1..4 bytes, including four when aligned. */
        unsigned pad = 4 - (10 + counts[3]) % 4;
        counts[1] += pad; counts[3] += pad;
        line = ht * 24 / 32 * 10 / 9;
        edge1 = 40 + (t->width + t->hback + t->hsync + 20u) * 24 / 32;
        if (edge1 > line) edge1 = line;
        edge0 = edge1 + (t->width + 40u) * 6 / 8;
        edge0 = edge0 > line ? edge0 - line : 1;
        if (edge0 > line || counts[3] + 10 > line * 4 || line <= 40) return 0;
        tri = ht / 10 * 2;
        drq = 1u << 28;
    } else {
        drq = (t->hfront - 20u) * 24 / 32;
        if (drq > 1023) return 0;
        drq |= 1u << 28;
    }
    for (unsigned i = 0; i != 5; ++i) if (counts[i] > 65535) return 0;
    struct a733_dsi_video_plan p = {.byte_hz = t->hs_hz / 8, .htotal = ht, .vtotal = vt};
    word(&p, 0x0c, 0x1ff, burst ? 3 : 0);
    word(&p, 0x10, 0x70000, 0x30000);
    word(&p, 0x14, 0xffff0ff7, 0x17 | (tri << 16));
    word(&p, 0x18, 0x0fff0fff, t->vsync | ((uint32_t)t->vback << 16));
    word(&p, 0x1c, 0x1fff0fff, t->height | (vt << 16));
    static const uint32_t instructions[8] = {0x1f, 0x10000001, 0x20000010,
        0x2000000f, 0x30100001, 0x40000010, 0x0f, 0x5000001f};
    for (unsigned i = 0; i != 8; ++i) word(&p, 0x20 + i * 4, 0xffff001f, instructions[i]);
    word(&p, 0x40, UINT32_MAX, 0x30000002);
    word(&p, 0x44, 0x0fff0fff, 49 | (burst ? 0 : 49u << 16));
    word(&p, 0x54, 0x0fff0fff, 49 | (burst ? 0 : 49u << 16));
    word(&p, 0x4c, 0x10ffffff, 0x00560001);
    word(&p, 0x50, 0x10000000, 0);
    word(&p, 0x60, 0x1fff, 10);
    word(&p, 0x78, 0xffff, 0);
    word(&p, 0x7c, 0x100003ff, drq);
    word(&p, 0x80, 0x1001f, 0x10008 | t->format);
    word(&p, 0x90, UINT32_MAX, header(0x3e - 0x10 * t->format, t->width * 3));
    word(&p, 0x98, 0xffff, 0xffff);
    word(&p, 0x9c, UINT32_MAX, UINT32_MAX);
    word(&p, 0xb0, UINT32_MAX, header(0x21, 0));
    word(&p, 0xb4, UINT32_MAX, header(0x31, 0));
    word(&p, 0xb8, UINT32_MAX, header(0x01, 0));
    word(&p, 0xbc, UINT32_MAX, header(burst ? 0x21 : 0x11, 0));
    static const uint8_t blank_offsets[5] = {0xc0, 0xc8, 0xd0, 0xe0, 0xe8};
    for (unsigned i = 0; i != 5; ++i) {
        word(&p, blank_offsets[i], UINT32_MAX, header(0x19, counts[i]));
        word(&p, blank_offsets[i] + 4, 0xffff00ff, zero_crc(counts[i]));
    }
    word(&p, 0xf0, UINT32_MAX, line | (burst ? 40u << 16 : 0));
    word(&p, 0xf4, UINT32_MAX, edge0 | (edge1 << 16));
    word(&p, 0x2f8, 0xffffff, 0xff);
    *out = p;
    return 1;
}

int a733_dsi_video_init(struct a733_dsi_video *d, const struct a733_dsi_video_config *c,
                         uint64_t deadline)
{
    if (!d || !c || overlap(d, sizeof(*d), c, sizeof(*c)) || d->initialized || !deadline ||
        c->controller > 1 || !c->lease || !c->ops.admit || !c->ops.read32 ||
        !c->ops.write32 || !c->ops.barrier || !c->ops.now_us) return 0;
    struct a733_dsi_video_plan p;
    if (!a733_dsi_video_plan(&c->timing, &p)) return 0;
    memset(d, 0, sizeof(*d));
    d->config = *c; d->plan = p; d->deadline = deadline; d->initialized = 1;
    return 1;
}
int a733_dsi_video_request(struct a733_dsi_video *d, enum a733_dsi_video_action action,
                            uint64_t deadline)
{
    if (!d || !d->initialized || d->entered || deadline <= d->last_now ||
        !((action == A733_DSI_VIDEO_START && d->state == A733_DSI_VIDEO_READY) ||
          ((action == A733_DSI_VIDEO_STOP || action == A733_DSI_VIDEO_OBSERVE) &&
           d->state == A733_DSI_VIDEO_ACTIVE))) return 0;
    d->action = action; d->deadline = deadline; d->state = A733_DSI_VIDEO_PENDING;
    d->step = d->index = d->substep = 0;
    return 1;
}
static void fail(struct a733_dsi_video *d, enum a733_dsi_video_error e)
{
    d->state = A733_DSI_VIDEO_RETAINED; d->error = e;
}
static int time_ok(struct a733_dsi_video *d)
{
    uint64_t now = 0;
    if (d->config.ops.now_us(d->config.context, &now) != 1 || now < d->last_now || now >= d->deadline) {
        fail(d, A733_DSI_VIDEO_TIME); return 0;
    }
    d->last_now = now;
    return d->state == A733_DSI_VIDEO_PENDING;
}
static const struct a733_dsi_video_word baseline[] = {
    {0, 1, 1}, {0x10, 0x71431, 0x30000}, {4, 0xffff, 0}, {0xc, 0x07ff01ff, 0},
    {0x80, 0x10, 0}, {0x2f0, 0xff000002, 0}, {0x50, 0x10000000, 0},
    {0x2f8, 0x10000000, 0}
};
#define BASELINES (sizeof(baseline) / sizeof(*baseline))
#define HS_JUMP 0x63f07602u

enum a733_dsi_video_state a733_dsi_video_poll(struct a733_dsi_video *d)
{
    if (!d || !d->initialized) return A733_DSI_VIDEO_RETAINED;
    if (d->state != A733_DSI_VIDEO_PENDING) return d->state;
    if (d->entered) { fail(d, A733_DSI_VIDEO_REENTRY); return d->state; }
    d->entered = 1;
    if (!time_ok(d)) goto out;
    uintptr_t base = 0x05506000u + 0x2000u * d->config.controller;
    void *ctx = d->config.context;
    uint32_t value = 0, offset = 0, data = 0;
    unsigned step = d->step, operation = 0; /* 0 admit, 1 read, 2 write, 3 barrier */
    const struct a733_dsi_video_word *w = NULL;
    if (step) {
        if (d->action == A733_DSI_VIDEO_PREPARE) {
            if (step == 1) { w = &baseline[d->index]; operation = 1; offset = w->offset; }
            else if (step == 2) {
                w = &d->plan.words[d->index]; offset = w->offset;
                operation = d->substep == 1 ? 2 : 1; data = d->expected;
            } else operation = 3;
        } else {
            switch (step) {
            case 1: operation = 1; offset = 0x10; break;
            case 2: operation = 1; offset = 0x2f0; break;
            case 3: operation = 1; offset = 0x4c; break;
            case 4: operation = 2; offset = d->action == A733_DSI_VIDEO_START ? 0x48 : 0x4c;
                data = d->action == A733_DSI_VIDEO_START ? HS_JUMP : d->expected | 0x10000000; break;
            case 5: operation = 1; offset = d->action == A733_DSI_VIDEO_START ? 0x48 : 0x4c; break;
            case 6: operation = 3; break;
            case 7: case 8:
                if (d->action == A733_DSI_VIDEO_START) {
                    operation = 2; offset = 0x10; data = d->control | (step == 8);
                } else { operation = 1; offset = step == 7 ? 0x10 : 0x2f0; }
                break;
            case 9: operation = 3; break;
            case 10: operation = 1; offset = 0x10; break;
            case 11: operation = 1; offset = 0x2f0; break;
            default: fail(d, A733_DSI_VIDEO_INITIAL_STATE); goto out;
            }
        }
    }
    int ok;
    if (operation == 0) ok = d->config.ops.admit(ctx, &d->config, d->action);
    else if (operation == 1) ok = d->config.ops.read32(ctx, base + offset, &value);
    else if (operation == 2) {
        ++d->writes_attempted;
        ok = d->config.ops.write32(ctx, base + offset, data);
    } else ok = d->config.ops.barrier(ctx);
    if (d->state != A733_DSI_VIDEO_PENDING) goto out;
    if (ok != 1) { fail(d, step ? A733_DSI_VIDEO_IO : A733_DSI_VIDEO_AUTHORITY); goto out; }
    if (!time_ok(d)) goto out;
    if (d->action == A733_DSI_VIDEO_PREPARE) {
        if (step == 1) {
            if ((value & w->mask) != w->value) { fail(d, A733_DSI_VIDEO_INITIAL_STATE); goto out; }
            if (++d->index != BASELINES) goto out;
            d->index = 0;
        } else if (step == 2) {
            if (!d->substep) d->expected = (value & ~w->mask) | w->value;
            if (d->substep == 2 && value != d->expected) { fail(d, A733_DSI_VIDEO_VERIFY); goto out; }
            if (++d->substep != 3) goto out;
            d->substep = 0;
            if (++d->index != d->plan.count) goto out;
        } else if (step == 3) d->state = A733_DSI_VIDEO_READY;
    } else {
        if (step == 1) {
            uint32_t active = d->action != A733_DSI_VIDEO_START;
            if ((value & 0x71431) != (0x30000 | active)) { fail(d, A733_DSI_VIDEO_INITIAL_STATE); goto out; }
            d->control = value & ~1u;
        }
        if (step == 2 || step == 11 || (step == 8 && d->action == A733_DSI_VIDEO_STOP)) {
            d->debug_status = value;
            if (value & 0xff000002) { fail(d, A733_DSI_VIDEO_ENGINE); goto out; }
            if (d->action == A733_DSI_VIDEO_OBSERVE || step == 11) d->state = A733_DSI_VIDEO_ACTIVE;
        }
        if (step == 3) {
            if ((value & 0x10ffffff) != 0x560001) { fail(d, A733_DSI_VIDEO_VERIFY); goto out; }
            d->expected = value;
        }
        if (step == 5 && value != (d->action == A733_DSI_VIDEO_START ? HS_JUMP : d->expected | 0x10000000)) {
            fail(d, A733_DSI_VIDEO_VERIFY); goto out;
        }
        if (step == 7 && d->action == A733_DSI_VIDEO_STOP) {
            if ((value & ~1u) != d->control) { fail(d, A733_DSI_VIDEO_VERIFY); goto out; }
            if (value & 1) goto out;
        }
        if (step == 9 && d->action == A733_DSI_VIDEO_STOP) d->state = A733_DSI_VIDEO_STOPPED;
        if (step == 10 && value != (d->control | 1)) { fail(d, A733_DSI_VIDEO_VERIFY); goto out; }
    }
    ++d->step;
out:
    d->entered = 0;
    return d->state;
}
