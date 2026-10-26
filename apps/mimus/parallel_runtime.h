#ifndef SUNSTONE_MIMUS_PARALLEL_RUNTIME_H
#define SUNSTONE_MIMUS_PARALLEL_RUNTIME_H
#ifndef SUNSTONE_MIMUS_WORKERS
#define SUNSTONE_MIMUS_WORKERS 2
#endif
// Eight-worker OS support is the admitted recorded composition.
#if defined(SUNSTONE_A733_OS) && SUNSTONE_MIMUS_WORKERS == 8
#if !defined(A733_A76_ADMITTED) || \
    defined(SUNSTONE_A733_OS_AUDIO) || defined(SUNSTONE_A733_OS_RADIO) || \
    defined(SUNSTONE_A733_OS_HEADSET)
#error Eight OS inference workers require admitted recorded recognition
#endif
#endif
inline constexpr unsigned kMimusOwnerPriority = 3;
// Initialize before the scheduler; bind and report from the recognition task,
// which runs on the owner core. Index 1 uses worker_core; an eight-worker
// build assigns indices 2..7 to the remaining cores in ascending order.
void mimus_parallel_init(unsigned owner_core = 0, unsigned worker_core = 1);
void mimus_parallel_bind();
void mimus_parallel_report();
#endif
