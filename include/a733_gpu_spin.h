#ifndef SUNSTONE_A733_GPU_SPIN_H
#define SUNSTONE_A733_GPU_SPIN_H
#include <stdint.h>

/* The wait inside one GPU power, startup or shutdown step. A step re-reads an
 * unsatisfied register or firmware word for at most A733_GPU_SPIN_US from its
 * first read, in microseconds, and then returns AGAIN to its owner. */
#define A733_GPU_SPIN_US 50u
/* Most consecutive clock samples without an advance inside one wait. A wait
 * whose clock stops advancing then returns AGAIN without advancing the step,
 * instead of holding its owner inside the step. */
#define A733_GPU_SPIN_STALLED_READS 1024u

/* Clock samples of one wait. Zero it when the wait begins. */
struct a733_gpu_spin { uint64_t start, seen; unsigned samples, still; };
/* Record a sample taken during a wait. Return 0 once A733_GPU_SPIN_STALLED_READS
 * consecutive samples show no advance. */
static inline int a733_gpu_spin_advancing(struct a733_gpu_spin *w, uint64_t now)
{
    if (!w->samples++ || now != w->seen) { w->seen = now; w->still = 0; return 1; }
    return ++w->still < A733_GPU_SPIN_STALLED_READS;
}
/* Record a sample taken after an unsatisfied read. Return 1 while the read may
 * repeat: within A733_GPU_SPIN_US of the first sample and with the clock
 * advancing. */
static inline int a733_gpu_spin_again(struct a733_gpu_spin *w, uint64_t now)
{
    if (!w->samples) w->start = now;
    return a733_gpu_spin_advancing(w, now) && now - w->start < A733_GPU_SPIN_US;
}
#endif
