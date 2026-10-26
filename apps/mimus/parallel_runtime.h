#ifndef SUNSTONE_MIMUS_PARALLEL_RUNTIME_H
#define SUNSTONE_MIMUS_PARALLEL_RUNTIME_H
#ifndef SUNSTONE_MIMUS_WORKERS
#define SUNSTONE_MIMUS_WORKERS 2
#endif
// The integrated OS launch metadata and service placement admit a pair only.
// This guard is target-local: standalone native/QEMU eight-worker builds remain
// available even when the OS target exists in the same CMake configuration.
#if defined(SUNSTONE_A733_OS) && SUNSTONE_MIMUS_WORKERS != 2
#error Integrated OS currently requires two inference workers
#endif
// Initialize before the scheduler; bind and report from the recognition task,
// which runs on the owner core. Index 1 uses worker_core; an eight-worker
// build assigns indices 2..7 to the remaining cores in ascending order.
void mimus_parallel_init(unsigned owner_core = 0, unsigned worker_core = 1);
void mimus_parallel_bind();
void mimus_parallel_report();
#endif
