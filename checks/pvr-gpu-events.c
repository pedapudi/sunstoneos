#include "pvr_gpu_events.h"
#include "pvr_rogue_fwif.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(struct rogue_fwif_fwccb_cmd) == PVR_CCB_COMMAND_BYTES, "event bytes");
_Static_assert(ROGUE_FWIF_MAX_FREELISTS_TO_RECONSTRUCT == PVR_GPU_EVENTS_RECONSTRUCT, "reconstruction bound");
static const struct pvr_gpu_event_resource resources[] = {
    {PVR_GPU_RESOURCE_ZS, 0, 1, 101}, {PVR_GPU_RESOURCE_FREELIST, 0, 1, 201},
    {PVR_GPU_RESOURCE_FREELIST, 42, 2, 202}, {PVR_GPU_RESOURCE_CONTEXT, 8, 1, 301},
    {PVR_GPU_RESOURCE_CONTEXT, 9, 2, 302}, {PVR_GPU_RESOURCE_PROCESS, 23, 1, 401}
};
struct model { struct pvr_gpu_events *state; unsigned calls, effects, fail, recurse; };
static int action(void *context, const struct pvr_gpu_event *e,
                  const struct pvr_gpu_event_resource *const *targets, unsigned count, unsigned kind)
{
    struct model *m = context; ++m->calls;
    assert((unsigned)e->kind == kind && e->flags == 0xfedcba98);
    assert(m->state->phase == PVR_GPU_EVENTS_DISPATCHING);
    if (e->kind == PVR_GPU_EVENT_BACK || e->kind == PVR_GPU_EVENT_UNBACK) {
        assert(count == 1 && targets[0]->kind == PVR_GPU_RESOURCE_ZS && targets[0]->token == 101);
    } else if (e->kind == PVR_GPU_EVENT_GROW) {
        assert(count == 1 && targets[0]->token == 201 && targets[0]->owner == 1);
    } else if (e->kind == PVR_GPU_EVENT_RECONSTRUCT) {
        assert(count == e->data.reconstruct.count);
        for (unsigned i = 0; i < count; ++i) {
            assert(targets[i]->kind == PVR_GPU_RESOURCE_FREELIST);
            assert(targets[i]->id == e->data.reconstruct.ids[i]);
            assert(targets[i]->token == (targets[i]->id == 42 ? 202u : 201u));
        }
    } else if (e->kind == PVR_GPU_EVENT_RESET) {
        assert(count == (e->data.reset.flags & 2 ? 2u : 1u));
        assert(targets[0]->token == 301);
        if (count == 2) assert(targets[1]->token == 302 && targets[1]->owner == 2);
    } else if (e->kind == PVR_GPU_EVENT_STATS) {
        assert(count == 1 && targets[0]->token == 401);
    } else assert(!count);
    ++m->effects; /* Synthetic action, allowed to take effect before failure. */
    if (m->recurse) {
        assert(!pvr_gpu_events_dispatch(m->state, 7, m->state->last_raw, 88));
        assert(m->state->error == PVR_GPU_EVENTS_REENTRY);
    }
    return !m->fail;
}
#define HANDLER(name, kind) \
static int name(void *m, const struct pvr_gpu_event *e, const struct pvr_gpu_event_resource *const *r, unsigned n) \
{ return action(m, e, r, n, kind); }
HANDLER(back, 101) HANDLER(unback, 102) HANDLER(grow, 103) HANDLER(reconstruct, 104)
HANDLER(reset, 105) HANDLER(debug, 106) HANDLER(stats, 107) HANDLER(clock_change, 108)
static const struct pvr_gpu_event_handlers handlers = {back, unback, grow, reconstruct, reset, debug, stats, clock_change};
static struct rogue_fwif_fwccb_cmd command(unsigned type)
{
    struct rogue_fwif_fwccb_cmd c;
    memset(&c, 0xa5, sizeof(c)); /* Firmware padding is not required to be zero. */
    c.cmd_type = (enum rogue_fwif_fwccb_cmd_type)(PVR_CCB_MAGIC | type);
    c.fwccb_flags = 0xfedcba98;
    switch (type) {
    case 101: case 102: c.cmd_data.cmd_zs_buffer_backing.zs_buffer_id = 0; break;
    case 103: c.cmd_data.cmd_free_list_gs.freelist_id = 0; break;
    case 104:
        c.cmd_data.cmd_freelists_reconstruction.freelist_count = 18;
        c.cmd_data.cmd_freelists_reconstruction.hwr_counter = 0xfed123;
        for (unsigned i = 0; i < 18; ++i) c.cmd_data.cmd_freelists_reconstruction.freelist_ids[i] = i & 1 ? 42 : 0;
        break;
    case 105:
        c.cmd_data.cmd_context_reset_notification.server_common_context_id = 8;
        c.cmd_data.cmd_context_reset_notification.reset_reason = ROGUE_CONTEXT_RESET_REASON_FW_PAGEFAULT;
        c.cmd_data.cmd_context_reset_notification.dm = PVR_FWIF_DM_CDM;
        c.cmd_data.cmd_context_reset_notification.reset_job_ref = 0xfedcab12;
        c.cmd_data.cmd_context_reset_notification.flags = ROGUE_FWIF_FWCCB_CMD_CONTEXT_RESET_FLAG_PF;
        c.cmd_data.cmd_context_reset_notification.pc_address = UINT64_C(0xf12345000);
        c.cmd_data.cmd_context_reset_notification.fault_address = UINT64_C(0xabcdef0123);
        break;
    case 107:
        c.cmd_data.cmd_update_stats_data.element_to_update = ROGUE_FWIF_FWCCB_CMD_UPDATE_NUM_CDM_STORES;
        c.cmd_data.cmd_update_stats_data.pid_owner = 23;
        c.cmd_data.cmd_update_stats_data.adjustment_value = INT32_MIN;
        break;
    case 108: c.cmd_data.cmd_core_clk_rate_change.core_clk_rate = 400000000; break;
    }
    return c;
}
static void init(struct pvr_gpu_events *s, struct model *m)
{
    memset(s, 0, sizeof(*s)); memset(m, 0, sizeof(*m)); m->state = s;
    assert(pvr_gpu_events_init(s, 7, resources, sizeof(resources)/sizeof(*resources), &handlers, m));
}
static unsigned failures;
static void fail_event(const void *c, size_t n, uint64_t epoch, unsigned error)
{
    struct pvr_gpu_events s; struct model m; init(&s, &m);
    assert(!pvr_gpu_events_dispatch(&s, epoch, c, n) && s.error == error);
    assert(s.phase == PVR_GPU_EVENTS_FAILED && !m.calls && !m.effects);
    struct pvr_gpu_events saved = s; struct rogue_fwif_fwccb_cmd good = command(103);
    assert(!pvr_gpu_events_dispatch(&s, 7, &good, sizeof(good)));
    assert(!memcmp(&s, &saved, sizeof(s)) && !m.calls);
    ++failures;
}
int main(void)
{
    struct pvr_gpu_events s; struct model m; init(&s, &m);
    for (unsigned type = 101; type <= 108; ++type) {
        struct rogue_fwif_fwccb_cmd c = command(type);
        unsigned char unaligned[89]; memcpy(unaligned + 1, &c, 88);
        struct pvr_gpu_event decoded;
        assert(pvr_gpu_event_decode(unaligned + 1, 88, &decoded) == PVR_GPU_EVENT_DECODE_OK);
        assert((unsigned)decoded.kind == type && decoded.flags == c.fwccb_flags);
        if (type <= 103) assert(decoded.data.id == 0);
        if (type == 104) {
            assert(decoded.data.reconstruct.count == 18 && decoded.data.reconstruct.recovery_counter == 0xfed123);
            for (unsigned i=0;i<18;++i) assert(decoded.data.reconstruct.ids[i] == (i & 1 ? 42u : 0u));
        }
        if (type == 105) {
            assert(decoded.data.reset.context == 8 && decoded.data.reset.reason == 13 && decoded.data.reset.engine == 4);
            assert(decoded.data.reset.job == 0xfedcab12 && decoded.data.reset.flags == 1);
            assert(decoded.data.reset.page_catalog == UINT64_C(0xf12345000));
            assert(decoded.data.reset.fault_address == UINT64_C(0xabcdef0123));
        }
        if (type == 107) assert(decoded.data.stats.statistic == 5 && decoded.data.stats.process == 23 && decoded.data.stats.adjustment == INT32_MIN);
        if (type == 108) assert(decoded.data.clock_hz == 400000000);
        assert(pvr_gpu_events_dispatch(&s, 7, &c, sizeof(c)) && s.phase == PVR_GPU_EVENTS_READY);
    }
    assert(m.calls == 8 && m.effects == 8);
    struct rogue_fwif_fwccb_cmd c = command(105);
    c.cmd_data.cmd_context_reset_notification.flags = 2;
    c.cmd_data.cmd_context_reset_notification.server_common_context_id = UINT32_MAX;
    assert(pvr_gpu_events_dispatch(&s, 7, &c, sizeof(c))); /* ALL_CTXS ignores this ID. */
    c = command(104); c.cmd_data.cmd_freelists_reconstruction.freelist_count = 0;
    assert(pvr_gpu_events_dispatch(&s, 7, &c, sizeof(c)));
    c = command(108); c.cmd_data.cmd_core_clk_rate_change.core_clk_rate = 0;
    assert(pvr_gpu_events_dispatch(&s, 7, &c, sizeof(c)));
    c = command(107); c.cmd_data.cmd_update_stats_data.adjustment_value = INT32_MAX;
    struct pvr_gpu_event e; assert(pvr_gpu_event_decode(&c, 88, &e) == PVR_GPU_EVENT_DECODE_OK && e.data.stats.adjustment == INT32_MAX);
    /* Pure decode refusal is atomic, including aliasing. */
    memset(&e, 0x5a, sizeof(e)); struct pvr_gpu_event before = e;
    assert(pvr_gpu_event_decode(&c, 87, &e) == PVR_GPU_EVENT_DECODE_INVALID && !memcmp(&e, &before, sizeof(e)));
    struct rogue_fwif_fwccb_cmd saved = c;
    assert(pvr_gpu_event_decode(&c, 88, (struct pvr_gpu_event *)&c) == PVR_GPU_EVENT_DECODE_INVALID && !memcmp(&c, &saved, 88));
    /* Catalogue input mutations cannot redirect an initialized dispatcher. */
    struct pvr_gpu_event_resource input[6]; memcpy(input, resources, sizeof(input));
    memset(&s, 0, sizeof(s)); m = (struct model){.state = &s};
    assert(pvr_gpu_events_init(&s, 7, input, 6, &handlers, &m)); input[1].owner = 99; input[1].token = 999;
    c = command(103); assert(pvr_gpu_events_dispatch(&s, 7, &c, 88));
    fail_event(&c, 88, 8, PVR_GPU_EVENTS_EPOCH);
    fail_event(&c, 87, 7, PVR_GPU_EVENTS_DECODE);
    unsigned char oversized[89]; memcpy(oversized, &c, 88); oversized[88] = 0;
    fail_event(oversized, 89, 7, PVR_GPU_EVENTS_DECODE);
    fail_event(NULL, 0, 7, PVR_GPU_EVENTS_INVALID);
    for (unsigned type = 109; type <= 112; ++type) { c = command(type); fail_event(&c, 88, 7, PVR_GPU_EVENTS_DECODE); }
    c = command(103); c.cmd_type = 103; fail_event(&c, 88, 7, PVR_GPU_EVENTS_DECODE);
    c = command(104); c.cmd_data.cmd_freelists_reconstruction.freelist_count = 19;
    fail_event(&c, 88, 7, PVR_GPU_EVENTS_DECODE);
    c = command(104); c.cmd_data.cmd_freelists_reconstruction.freelist_ids[17] = 99;
    fail_event(&c, 88, 7, PVR_GPU_EVENTS_OWNERSHIP); /* No first-resource action. */
    c = command(103); c.cmd_data.cmd_free_list_gs.freelist_id = 8; /* Exists only as a context. */
    fail_event(&c, 88, 7, PVR_GPU_EVENTS_OWNERSHIP);
    c = command(105); c.cmd_data.cmd_context_reset_notification.flags = 4;
    fail_event(&c, 88, 7, PVR_GPU_EVENTS_DECODE);
    c = command(105); c.cmd_data.cmd_context_reset_notification.dm = 9;
    fail_event(&c, 88, 7, PVR_GPU_EVENTS_DECODE);
    const unsigned bad_reasons[] = {6, 11, 17};
    for (unsigned i=0;i<3;++i) { c = command(105); c.cmd_data.cmd_context_reset_notification.reset_reason = bad_reasons[i]; fail_event(&c, 88, 7, PVR_GPU_EVENTS_DECODE); }
    for (unsigned value=0;value<=7;value+=7) { c = command(107); c.cmd_data.cmd_update_stats_data.element_to_update = value; fail_event(&c, 88, 7, PVR_GPU_EVENTS_DECODE); }
    init(&s, &m); s.handlers.grow = NULL; c = command(103);
    assert(!pvr_gpu_events_dispatch(&s, 7, &c, 88) && s.error == PVR_GPU_EVENTS_UNSUPPORTED && !m.calls);
    init(&s, &m); m.fail = 1;
    assert(!pvr_gpu_events_dispatch(&s, 7, &c, 88) && s.error == PVR_GPU_EVENTS_ACTION && m.effects == 1);
    assert(!memcmp(s.last_raw, &c, 88));
    assert(!pvr_gpu_events_dispatch(&s, 7, &c, 88) && m.calls == 1);
    init(&s, &m); m.recurse = 1;
    assert(!pvr_gpu_events_dispatch(&s, 7, &c, 88) && s.error == PVR_GPU_EVENTS_REENTRY && m.calls == 1);
    assert(!memcmp(s.last_raw, &c, 88));
    memset(&s, 0, sizeof(s)); struct pvr_gpu_events initial = s;
    memcpy(input, resources, sizeof(input)); input[1] = input[0];
    assert(!pvr_gpu_events_init(&s, 7, input, 6, &handlers, &m) && !memcmp(&s, &initial, sizeof(s)));
    memcpy(input, resources, sizeof(input)); input[0].owner = 0;
    assert(!pvr_gpu_events_init(&s, 7, input, 6, &handlers, &m) && !memcmp(&s, &initial, sizeof(s)));
    assert(!pvr_gpu_events_init(&s, 0, resources, 6, &handlers, &m));
    assert(!pvr_gpu_events_init(&s, 7, resources, 65, &handlers, &m));
    printf("PASS GPU events: original ABI fixtures for eight types, owned dispatch, %u rejected events, unsupported action, retained side effect and reentry\n", failures);
    return 0;
}
