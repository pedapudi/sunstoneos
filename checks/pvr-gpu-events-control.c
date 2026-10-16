/* Reuse the established queue/control fixture without running its test suite. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main gpu_control_fixture_main
#include "pvr-gpu-control.c"
#undef main
#pragma clang diagnostic pop
#include "pvr_gpu_events.h"

static struct pvr_gpu_events events;
static unsigned actions;
static int grow_resource(void *ctx, const struct pvr_gpu_event *e,
                         const struct pvr_gpu_event_resource *const *targets, unsigned count)
{
    assert(ctx == &m && e->kind == PVR_GPU_EVENT_GROW && count == 1);
    assert(targets[0]->id == 42 && targets[0]->owner == 3 && targets[0]->token == 123);
    ++actions; return 1;
}
static int unexpected_reconstruct(void *ctx, const struct pvr_gpu_event *e,
                                  const struct pvr_gpu_event_resource *const *targets, unsigned count)
{ (void)ctx; (void)e; (void)targets; (void)count; assert(0); return 0; }
static int dispatch(void *ctx, const unsigned char data[88])
{ assert(ctx == &m); return pvr_gpu_events_dispatch(&events, q.epoch, data, 88); }
int main(void)
{
    prepare();
    const struct pvr_gpu_event_resource resource = {PVR_GPU_RESOURCE_FREELIST, 42, 3, 123};
    const struct pvr_gpu_event_handlers handlers = {.grow = grow_resource, .reconstruct = unexpected_reconstruct};
    assert(pvr_gpu_events_init(&events, q.epoch, &resource, 1, &handlers, &m));
    control.ops.event = dispatch;
    assert(run() == PVR_GPU_CONTROL_OK);
    event(103, 42); assert(run() == PVR_GPU_CONTROL_OK && actions == 1);
    event(104, 2);
    unsigned slot = (word(m.memory[PVR_CCB_FIRMWARE_CONTROL]) - 1) & 31;
    unsigned char *cmd = m.memory[PVR_CCB_FIRMWARE_COMMANDS] + slot * 88;
    put(cmd + 16, 42, 4); put(cmd + 20, 99, 4);
    assert(run() == PVR_GPU_CONTROL_FAULT && actions == 1);
    assert(events.error == PVR_GPU_EVENTS_OWNERSHIP && !memcmp(events.last_raw, control.last_event, 88));
    stopped(PVR_GPU_CONTROL_EVENT);
    puts("PASS GPU event/control integration: owned grow, complete reconstruction lookup, retained controller and queue failure");
    return 0;
}
