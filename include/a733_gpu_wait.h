#ifndef SUNSTONE_A733_GPU_WAIT_H
#define SUNSTONE_A733_GPU_WAIT_H
#include <stdint.h>
/* Native controller waits are returned to the unlocked service task. The
 * vendor firmware-start polling interval is 50 us (rgxpower.c:1039-1052).
 * The same observation interval replaces status-read spins. Minimum holds
 * retain their own intervals and never refresh after expiry. */
#define A733_GPU_OBSERVE_US UINT64_C(50)
enum a733_gpu_wait_kind { A733_GPU_WAIT_FIXED, A733_GPU_WAIT_RETRY, A733_GPU_WAIT_EVENT };
struct a733_gpu_wait {
    uint64_t next_wait_us;
    enum a733_gpu_wait_kind kind;
};
static inline void a733_gpu_wait_set(struct a733_gpu_wait *w,uint64_t now,
    uint64_t interval,uint64_t deadline,enum a733_gpu_wait_kind kind)
{
    w->next_wait_us=now>=deadline || deadline-now<interval?deadline:now+interval;
    w->kind=kind;
}
static inline uint64_t a733_gpu_wait_until(struct a733_gpu_wait *w,uint64_t now,uint64_t deadline)
{
    /* Planning and queue inspection can consume a small retry interval.
     * Refresh an unsatisfied observation after releasing the transport lock,
     * so a matured compare cannot become a repeated status-read loop. */
    if(w->kind==A733_GPU_WAIT_RETRY && w->next_wait_us && w->next_wait_us<=now && now<deadline)
        a733_gpu_wait_set(w,now,A733_GPU_OBSERVE_US,deadline,A733_GPU_WAIT_RETRY);
    return w->next_wait_us;
}
#endif
