/* A CPU-entry checkpoint of the frozen finite GPU image, before providers.
 * This state proves that no device was opened; it is not a shutdown proof. */
#include "checkpoint.h"
#include "application.h"
#include "session.h"
#include "a733_memory_reservations.h"
#include "a733_memory_uncached.h"
#include "a733_device_memory_layout.h"
#include "platform.h"
extern unsigned char __gpu_private_start[], __gpu_private_end[];
extern const struct a733_smp_contract *sd_single_core_contract(void);
struct gpu_entry_checkpoint gpu_checkpoint;
static int private_span(const void *p, size_t bytes)
{
    uintptr_t a = (uintptr_t)p, lo = (uintptr_t)__gpu_private_start, hi = (uintptr_t)__gpu_private_end;
    return bytes && a >= lo && a < hi && bytes <= hi - a;
}
static int zero(const void *p, size_t bytes)
{
    const unsigned char *bytes_at = p;
    unsigned value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= bytes_at[i];
    return value == 0;
}
static int empty(const struct device_memory_pool *p, uintptr_t start, size_t bytes)
{
    return private_span(p, sizeof(*p)) && p->initialized == 1 && p->backing.cpu == start &&
        p->backing.physical == start && p->backing.bytes == bytes && p->next_generation == 1 &&
        zero(p->allocations, sizeof(p->allocations));
}
/* EL1 before publication and EL3 afterward: scalar reads of published private
 * state only. In particular, no RTOS, provider, translation or device call. */
int gpu_checkpoint_ready(void)
{
    const struct a733_board_session *s = gpu_application_session();
    const struct a733_smp_contract *c = sd_single_core_contract();
    return private_span(&gpu_checkpoint, sizeof(gpu_checkpoint)) &&
        private_span(s, sizeof(*s)) && private_span(c, sizeof(*c)) &&
        private_span(&app, sizeof(app)) && private_span(a733_cached_runtime, sizeof(a733_cached_runtime[0])) &&
        gpu_checkpoint.complete == GPU_ENTRY_MEMORY_READY && zero(&app, sizeof(app)) &&
        gpu_checkpoint.epoch == s->epoch && gpu_checkpoint.generation == s->generation &&
        gpu_checkpoint.lease == s->lease && c->epoch == s->epoch && c->generation == s->generation &&
        c->lease == s->lease && a733_cached_runtime[0].epoch == c->epoch &&
        a733_cached_runtime[0].startup_deadline == c->startup_deadline &&
        a733_cached_runtime[0].diagnostic_deadline == c->diagnostic_deadline &&
        gpu_checkpoint.wb != gpu_checkpoint.nc &&
        empty(gpu_checkpoint.wb, 0x48000000, 0x30000000) &&
        empty(gpu_checkpoint.nc, 0x78000000, 0x01000000);
}
_Noreturn void gpu_entry_checkpoint(void)
{
    const struct a733_board_session *s = gpu_application_session();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED || !s ||
        !a733_smp_device_preparation_admitted() || !a733_device_memory_ready() ||
        gpu_checkpoint.complete) platform_panic("GPU entry checkpoint admission", 1);
    gpu_checkpoint.wb = a733_memory_reservations_init();
    gpu_checkpoint.nc = a733_memory_uncached_init();
    gpu_checkpoint.epoch = s->epoch;
    gpu_checkpoint.generation = s->generation;
    gpu_checkpoint.lease = s->lease;
    gpu_checkpoint.complete = GPU_ENTRY_MEMORY_READY;
    if (!gpu_checkpoint_ready()) platform_panic("GPU entry checkpoint state", 2);
    platform_puts("GPU_CHECKPOINT=ENTRY_MEMORY_READY; providers never constructed\n");
    platform_exit(0);
}
