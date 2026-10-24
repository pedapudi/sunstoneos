#ifndef SUNSTONE_MIMUS_PARALLEL_RUNTIME_H
#define SUNSTONE_MIMUS_PARALLEL_RUNTIME_H
// Initialize before the scheduler; bind and report from the recognition task,
// which runs on the owner core. The worker runs on the worker core.
void mimus_parallel_init(unsigned owner_core = 0, unsigned worker_core = 1);
void mimus_parallel_bind();
void mimus_parallel_report();
#endif
