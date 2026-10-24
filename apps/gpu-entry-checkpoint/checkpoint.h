#ifndef SUNSTONE_GPU_ENTRY_CHECKPOINT_H
#define SUNSTONE_GPU_ENTRY_CHECKPOINT_H
#include "device_memory_pool.h"
#define GPU_ENTRY_MEMORY_READY 1u
struct gpu_entry_checkpoint {
    struct device_memory_pool *wb, *nc;
    uint64_t epoch, generation, lease;
    unsigned complete;
};
extern struct gpu_entry_checkpoint gpu_checkpoint;
int gpu_checkpoint_ready(void);
_Noreturn void gpu_entry_checkpoint(void);
#endif
