/* SPDX-License-Identifier: MIT
 * Firmware event layouts adapted from pvr_rogue_fwif.h and related headers.
 * Copyright (c) 2023 Imagination Technologies Ltd. See LICENSE.pvr.
 */
#include "pvr_gpu_events.h"
#include "a733_range.h"
#include <string.h>

static uint64_t le(const unsigned char *p, unsigned n)
{ uint64_t v = 0; for (unsigned i = 0; i < n; ++i) v |= (uint64_t)p[i] << (i * 8); return v; }
enum pvr_gpu_event_decode_result pvr_gpu_event_decode(const void *command, size_t bytes,
                                                      struct pvr_gpu_event *out)
{
    if (bytes != PVR_CCB_COMMAND_BYTES || !a733_span_excl_nonempty(command, bytes) ||
        !a733_span_excl_nonempty(out, sizeof(*out)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)command, bytes, (uintptr_t)out, sizeof(*out))) return PVR_GPU_EVENT_DECODE_INVALID;
    const unsigned char *b = command;
    uint32_t type = (uint32_t)le(b, 4);
    if (type < (PVR_CCB_MAGIC | 101u) || type > (PVR_CCB_MAGIC | 108u))
        return PVR_GPU_EVENT_DECODE_TYPE;
    struct pvr_gpu_event e = {.kind = (enum pvr_gpu_event_kind)(type & 0xffff),
                              .flags = (uint32_t)le(b + 4, 4)};
    b += 8;
    switch (e.kind) {
    case PVR_GPU_EVENT_BACK:
    case PVR_GPU_EVENT_UNBACK:
    case PVR_GPU_EVENT_GROW: e.data.id = (uint32_t)le(b, 4); break;
    case PVR_GPU_EVENT_RECONSTRUCT:
        e.data.reconstruct.count = (uint32_t)le(b, 4);
        e.data.reconstruct.recovery_counter = (uint32_t)le(b + 4, 4);
        if (e.data.reconstruct.count > PVR_GPU_EVENTS_RECONSTRUCT) return PVR_GPU_EVENT_DECODE_PAYLOAD;
        for (unsigned i = 0; i < e.data.reconstruct.count; ++i)
            e.data.reconstruct.ids[i] = (uint32_t)le(b + 8 + 4 * i, 4);
        break;
    case PVR_GPU_EVENT_RESET:
        e.data.reset.context = (uint32_t)le(b, 4);
        e.data.reset.reason = (uint32_t)le(b + 4, 4);
        e.data.reset.engine = (uint32_t)le(b + 8, 4);
        e.data.reset.job = (uint32_t)le(b + 12, 4);
        e.data.reset.flags = (uint32_t)le(b + 16, 4);
        e.data.reset.page_catalog = le(b + 24, 8);
        e.data.reset.fault_address = le(b + 32, 8);
        if ((e.data.reset.reason > 5 && e.data.reset.reason < 12) || e.data.reset.reason > 16 ||
            e.data.reset.engine > 8 || (e.data.reset.flags & ~3u)) return PVR_GPU_EVENT_DECODE_PAYLOAD;
        break;
    case PVR_GPU_EVENT_DEBUG: break;
    case PVR_GPU_EVENT_STATS: {
        e.data.stats.statistic = (uint32_t)le(b, 4);
        e.data.stats.process = (uint32_t)le(b + 4, 4);
        uint32_t word = (uint32_t)le(b + 8, 4);
        e.data.stats.adjustment = word <= INT32_MAX ? (int32_t)word : (int32_t)((int64_t)word - INT64_C(4294967296));
        if (!e.data.stats.statistic || e.data.stats.statistic > 6) return PVR_GPU_EVENT_DECODE_PAYLOAD;
        break;
    }
    case PVR_GPU_EVENT_CLOCK: e.data.clock_hz = (uint32_t)le(b, 4); break;
    }
    *out = e; return PVR_GPU_EVENT_DECODE_OK;
}
int pvr_gpu_events_init(struct pvr_gpu_events *s, uint64_t epoch,
    const struct pvr_gpu_event_resource *resources, unsigned count,
    const struct pvr_gpu_event_handlers *handlers, void *context)
{
    if (!a733_span_excl_nonempty(s, sizeof(*s)) || s->phase != PVR_GPU_EVENTS_NEW || !epoch ||
        count > PVR_GPU_EVENTS_RESOURCES || !a733_span_excl_nonempty(handlers, sizeof(*handlers)) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)handlers, sizeof(*handlers)) ||
        (count && (!a733_span_excl_nonempty(resources, count * sizeof(*resources)) ||
                   !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)resources, count * sizeof(*resources))))) return 0;
    for (unsigned i = 0; i < count; ++i) {
        if (resources[i].kind < PVR_GPU_RESOURCE_ZS || resources[i].kind > PVR_GPU_RESOURCE_PROCESS ||
            !resources[i].owner || !resources[i].token) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (resources[i].kind == resources[j].kind && resources[i].id == resources[j].id) return 0;
    }
    struct pvr_gpu_events result = {0};
    result.epoch = epoch; result.handlers = *handlers; result.context = context; result.count = count;
    if (count) memcpy(result.resources, resources, count * sizeof(*resources));
    result.phase = PVR_GPU_EVENTS_READY; *s = result; return 1;
}
static int fail(struct pvr_gpu_events *s, enum pvr_gpu_events_error error)
{ s->phase = PVR_GPU_EVENTS_FAILED; s->error = error; return 0; }
static const struct pvr_gpu_event_resource *lookup(const struct pvr_gpu_events *s,
    enum pvr_gpu_event_resource_kind kind, uint32_t id)
{
    for (unsigned i = 0; i < s->count; ++i)
        if (s->resources[i].kind == kind && s->resources[i].id == id) return &s->resources[i];
    return NULL;
}
int pvr_gpu_events_dispatch(struct pvr_gpu_events *s, uint64_t epoch, const void *command, size_t bytes)
{
    if (!s || s->phase == PVR_GPU_EVENTS_NEW || s->phase == PVR_GPU_EVENTS_FAILED) return 0;
    if (s->phase == PVR_GPU_EVENTS_DISPATCHING) return fail(s, PVR_GPU_EVENTS_REENTRY);
    if (s->phase != PVR_GPU_EVENTS_READY) return 0;
    s->last_bytes = bytes;
    if (!a733_span_excl_nonempty(command, bytes) ||
        !a733_extents_disjoint_prechecked((uintptr_t)s, sizeof(*s), (uintptr_t)command, bytes))
        return fail(s, PVR_GPU_EVENTS_INVALID);
    memset(s->last_raw, 0, sizeof(s->last_raw));
    memcpy(s->last_raw, command, bytes < sizeof(s->last_raw) ? bytes : sizeof(s->last_raw));
    if (epoch != s->epoch) return fail(s, PVR_GPU_EVENTS_EPOCH);
    s->decode_error = pvr_gpu_event_decode(command, bytes, &s->last_event);
    if (s->decode_error != PVR_GPU_EVENT_DECODE_OK) return fail(s, PVR_GPU_EVENTS_DECODE);
    const struct pvr_gpu_event *e = &s->last_event;
    const struct pvr_gpu_event_resource *targets[PVR_GPU_EVENTS_RESOURCES];
    unsigned count = 0;
    pvr_gpu_event_handler handler = NULL;
    switch (e->kind) {
    case PVR_GPU_EVENT_BACK:
    case PVR_GPU_EVENT_UNBACK:
        handler = e->kind == PVR_GPU_EVENT_BACK ? s->handlers.back : s->handlers.unback;
        targets[count++] = lookup(s, PVR_GPU_RESOURCE_ZS, e->data.id); break;
    case PVR_GPU_EVENT_GROW:
        handler = s->handlers.grow;
        targets[count++] = lookup(s, PVR_GPU_RESOURCE_FREELIST, e->data.id); break;
    case PVR_GPU_EVENT_RECONSTRUCT:
        handler = s->handlers.reconstruct;
        for (unsigned i = 0; i < e->data.reconstruct.count; ++i)
            targets[count++] = lookup(s, PVR_GPU_RESOURCE_FREELIST, e->data.reconstruct.ids[i]);
        break;
    case PVR_GPU_EVENT_RESET:
        handler = s->handlers.reset;
        if (e->data.reset.flags & 2) {
            for (unsigned i = 0; i < s->count; ++i)
                if (s->resources[i].kind == PVR_GPU_RESOURCE_CONTEXT) targets[count++] = &s->resources[i];
        } else targets[count++] = lookup(s, PVR_GPU_RESOURCE_CONTEXT, e->data.reset.context);
        break;
    case PVR_GPU_EVENT_DEBUG: handler = s->handlers.debug; break;
    case PVR_GPU_EVENT_STATS:
        handler = s->handlers.stats;
        targets[count++] = lookup(s, PVR_GPU_RESOURCE_PROCESS, e->data.stats.process); break;
    case PVR_GPU_EVENT_CLOCK: handler = s->handlers.clock; break;
    }
    for (unsigned i = 0; i < count; ++i) if (!targets[i]) return fail(s, PVR_GPU_EVENTS_OWNERSHIP);
    if (!handler) return fail(s, PVR_GPU_EVENTS_UNSUPPORTED);
    s->phase = PVR_GPU_EVENTS_DISPATCHING;
    int ok = handler(s->context, e, targets, count);
    if (s->phase != PVR_GPU_EVENTS_DISPATCHING) return 0;
    if (ok != 1) return fail(s, PVR_GPU_EVENTS_ACTION);
    s->phase = PVR_GPU_EVENTS_READY; return 1;
}
