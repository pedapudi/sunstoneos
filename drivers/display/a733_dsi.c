/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register and packet definitions adapted from the published display driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2016 Allwinner.
 */
#include "a733_dsi.h"
#include "a733_range.h"
#include <string.h>

static uint8_t ecc(uint32_t header)
{
    static const uint32_t masks[6] = {
        0xf12cb7, 0xf2555b, 0x749a6d, 0xb8e38e, 0xdf03f0, 0xeffc00
    };
    uint8_t result = 0;
    for (unsigned i = 0; i != 6; ++i) {
        uint32_t x = header & masks[i];
        x ^= x >> 16; x ^= x >> 8; x ^= x >> 4;
        x ^= x >> 2; x ^= x >> 1;
        result |= (uint8_t)((x & 1) << i);
    }
    return result;
}

static uint16_t crc(const uint8_t *bytes, size_t size)
{
    uint16_t value = 0xffff;
    for (size_t i = 0; i != size; ++i) {
        value ^= bytes[i];
        for (unsigned j = 0; j != 8; ++j)
            value = (uint16_t)((value >> 1) ^ ((value & 1) ? 0x8408 : 0));
    }
    return value;
}

int a733_dsi_encode(struct a733_dsi_packet *out, unsigned type, unsigned channel,
                    const void *payload, size_t size)
{
    unsigned expected;
    int is_long = 0;
    switch (type) {
    case 0x03: expected = 0; break;
    case 0x05: case 0x13: expected = 1; break;
    case 0x15: case 0x23: expected = 2; break;
    case 0x29: case 0x39: expected = 0; is_long = 1; break;
    default: return 0;
    }
    if (!out || channel > 3 || (size && !payload) ||
        (is_long ? (!size || size > A733_DSI_LONG_PAYLOAD_MAX) : size != expected) ||
        a733_extents_overlap_nonempty_or_wrap((uintptr_t)out, sizeof(*out),
                                              (uintptr_t)payload, size)) return 0;
    const uint8_t *p = payload;
    uint8_t a = is_long ? (uint8_t)size : size ? p[0] : 0;
    uint8_t b = is_long ? (uint8_t)(size >> 8) : size > 1 ? p[1] : 0;
    uint32_t header = type | (channel << 6) | ((uint32_t)a << 8) | ((uint32_t)b << 16);
    uint16_t checksum = is_long ? crc(p, size) : 0;
    memset(out, 0, sizeof(*out));
    out->bytes[0] = (uint8_t)header;
    out->bytes[1] = a;
    out->bytes[2] = b;
    out->bytes[3] = ecc(header);
    if (is_long) {
        memcpy(out->bytes + 4, p, size);
        out->bytes[4 + size] = (uint8_t)checksum;
        out->bytes[5 + size] = (uint8_t)(checksum >> 8);
    }
    out->size = (uint16_t)(is_long ? size + 6 : 4);
    return 1;
}

int a733_dsi_init(struct a733_dsi *d, const struct a733_dsi_config *c)
{
    if (!d || !c ||
        a733_extents_overlap_nonempty_or_wrap((uintptr_t)d, sizeof(*d), (uintptr_t)c, sizeof(*c)) ||
        d->initialized ||
        c->controller > 1 || !c->lanes || c->lanes > 4 || !c->lease ||
        !c->ops.admit || !c->ops.read32 || !c->ops.write32 ||
        !c->ops.barrier || !c->ops.now_us) return 0;
    memset(d, 0, sizeof(*d));
    d->config = *c;
    d->initialized = 1;
    return 1;
}

int a733_dsi_submit(struct a733_dsi *d, unsigned type, unsigned channel,
                    const void *payload, size_t size, uint64_t deadline)
{
    if (!d || !d->initialized || d->entered || d->state != A733_DSI_IDLE ||
        deadline <= d->last_now ||
        a733_extents_overlap_nonempty_or_wrap((uintptr_t)d, sizeof(*d), (uintptr_t)payload, size)) return 0;
    if (!a733_dsi_encode(&d->packet, type, channel, payload, size)) return 0;
    d->deadline = deadline;
    d->step = d->position = d->writes_attempted = 0;
    d->error = A733_DSI_OK;
    d->state = A733_DSI_PENDING;
    return 1;
}

static void fail(struct a733_dsi *d, enum a733_dsi_error error)
{
    d->state = A733_DSI_RETAINED;
    d->error = error;
}

static int time_ok(struct a733_dsi *d)
{
    uint64_t now = 0;
    if (d->config.ops.now_us(d->config.context, &now) != 1 ||
        !a733_deadline_admit(now, &d->last_now, d->deadline)) {
        fail(d, A733_DSI_TIME);
        return 0;
    }
    return d->state == A733_DSI_PENDING;
}

/* The adopted profile uses LP11 -> LPDT -> END. Conditional jumps may be
 * disabled or the source's NOP -> HSCEXIT branch, which this path never visits.
 */
static int profile(unsigned step, uint32_t value, unsigned lanes)
{
    switch (step) {
    case 1: return (value & 1) != 0;
    case 2: return (value & 0x30401) == 0x30000;
    case 3: return !(value & 0xffff);
    case 4: return (value & 0xffff001f) == (0x10u | ((1u << lanes) - 1));
    case 5: return (value & 0xffff001f) == 0x30100001;
    case 6: return (value & 0x000f000f) == 2;
    case 7: case 8: return (value & 0xfff) == 49;
    case 9: case 10:
        return !(value & (1u << 28)) || (value & 0xff0000) == 0x560000;
    default: return 0;
    }
}

enum a733_dsi_state a733_dsi_poll(struct a733_dsi *d)
{
    static const uint16_t offsets[11] = {0, 0, 0x10, 4, 0x20, 0x30,
        0x40, 0x44, 0x54, 0x4c, 0x50};
    if (!d || !d->initialized) return A733_DSI_RETAINED;
    if (d->state != A733_DSI_PENDING) return d->state;
    if (d->entered) { fail(d, A733_DSI_REENTRY); return d->state; }
    d->entered = 1;
    if (!time_ok(d)) goto out;
    uintptr_t base = 0x05506000u + d->config.controller * 0x2000u;
    void *context = d->config.context;
    uint32_t value = 0;
    int ok = 0;
    unsigned step = d->step;
    if (!step)
        ok = d->config.ops.admit(context, d->config.controller,
                                d->config.lanes, d->config.lease);
    else if (step <= 10)
        ok = d->config.ops.read32(context, base + offsets[step], &value);
    else switch (step) {
    case 11:
        for (unsigned i = 0; i != 4; ++i)
            value |= (uint32_t)d->packet.bytes[d->position + i] << (8 * i);
        ++d->writes_attempted;
        ok = d->config.ops.write32(context, base + 0x300 + d->position, value);
        break;
    case 12:
        ++d->writes_attempted;
        ok = d->config.ops.write32(context, base + 0x200, d->packet.size - 1u);
        break;
    case 13: ok = d->config.ops.read32(context, base + 0x200, &value); break;
    case 14:
        ++d->writes_attempted;
        ok = d->config.ops.write32(context, base + 0x48, 0x000f0004);
        break;
    case 15: ok = d->config.ops.read32(context, base + 0x48, &value); break;
    case 16: case 20: ok = d->config.ops.barrier(context); break;
    case 17: ok = d->config.ops.read32(context, base + 0x10, &value); break;
    case 18: case 19:
        ++d->writes_attempted;
        ok = d->config.ops.write32(context, base + 0x10, d->control | (step == 19));
        break;
    case 21: ok = d->config.ops.read32(context, base + 0x10, &value); break;
    case 22: ok = d->config.ops.barrier(context); break;
    default: fail(d, A733_DSI_INITIAL_STATE); goto out;
    }
    if (d->state != A733_DSI_PENDING) goto out;
    if (ok != 1) { fail(d, step ? A733_DSI_IO : A733_DSI_AUTHORITY); goto out; }
    if (!time_ok(d)) goto out;
    if (step >= 1 && step <= 10 && !profile(step, value, d->config.lanes)) {
        fail(d, A733_DSI_INITIAL_STATE); goto out;
    }
    if (step == 2) d->control = value;
    if ((step == 13 && (value & 0xff) != d->packet.size - 1u) ||
        (step == 15 && value != 0x000f0004) ||
        (step == 17 && value != d->control) ||
        (step == 21 && (value & ~1u) != d->control)) {
        fail(d, A733_DSI_VERIFY); goto out;
    }
    if (step == 11) {
        d->position += 4;
        if (d->position < d->packet.size) goto out;
    }
    if (step == 21 && (value & 1)) goto out;
    if (step == 22) d->state = A733_DSI_ENGINE_DONE;
    else ++d->step;
out:
    d->entered = 0;
    return d->state;
}

int a733_dsi_reap(struct a733_dsi *d)
{
    if (!d || !d->initialized || d->entered || d->state != A733_DSI_ENGINE_DONE) return 0;
    d->state = A733_DSI_IDLE;
    return 1;
}
