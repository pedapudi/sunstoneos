#ifndef SUNSTONE_MIMUS_PARALLEL_RUNTIME_H
#define SUNSTONE_MIMUS_PARALLEL_RUNTIME_H
#ifndef SUNSTONE_MIMUS_WORKERS
#define SUNSTONE_MIMUS_WORKERS 2
#endif
// Initialize before the scheduler; bind and report from the recognition task,
// which runs on the owner core. Index 1 uses worker_core; an eight-worker
// build assigns indices 2..7 to the remaining cores in ascending order.
void mimus_parallel_init(unsigned owner_core = 0, unsigned worker_core = 1);
void mimus_parallel_bind();
void mimus_parallel_report();
#endif
