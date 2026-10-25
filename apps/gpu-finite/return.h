#ifndef SUNSTONE_GPU_RETURN_H
#define SUNSTONE_GPU_RETURN_H
#include <stdint.h>
/* Fixed two-job profile, produced by the extended DMA32 write-back pool.
 * Firmware occupies 34 pages; the 88-page payload needs 64 KiB alignment.
 * The later 12-page table fits in the gap, leaving two free pages before the
 * payload. The coordinator owns the whole pool through publication/return. */
#define GPU_RETURN_WB_START UINT64_C(0x48000000)
#define GPU_RETURN_PAYLOAD_START UINT64_C(0x48030000)
#define GPU_RETURN_TABLE_START UINT64_C(0x48022000)
#define GPU_RETURN_WB_END UINT64_C(0x48088000)
#define GPU_RETURN_WB_BYTES (GPU_RETURN_WB_END - GPU_RETURN_WB_START)
struct gpu_boot_contract;
/* Same inherited timer/GIC snapshot used by the accepted single-core monitor,
 * extended only for the GPU interrupt fields changed by this application. */
struct gpu_return_gic {
    uint32_t ready,banks,ctlr,group,modifier,enabled,configuration;
    uint32_t priorities,spi_enabled[32];
    uint32_t gpu_group,gpu_modifier,gpu_configuration,gpu_priority;
    uint64_t gpu_route;
};
struct gpu_return_result {
    uint64_t magic;
    uint32_t version,bytes;
    uint64_t epoch,generation;
    uint32_t sequence[2],word_count,clean_lines;
    uint32_t words[64];
    uint64_t cleanup_start,cleanup_end;
};
#define GPU_RETURN_MAGIC UINT64_C(0x3154455255504741)
#define GPU_RETURN_RESULT_ADDRESS UINT64_C(0x42000100)
#define GPU_RETURN_RECEIPT_MAGIC UINT64_C(0x4750555245433031)
struct gpu_return_receipt {
    uint64_t magic,epoch,generation,lease,status,complete;
    uint64_t clean_start,clean_end,wb_start,wb_end,clean_lines,deadline,log_length;
};
extern struct gpu_return_receipt gpu_return_receipt;
int gpu_return_allocations(void);
int gpu_return_device_quiet(void);
_Noreturn void gpu_finite_smc(unsigned status);
int gpu_finite_return_ready(void);
_Noreturn void gpu_return_finish(const struct gpu_boot_contract *,const struct gpu_return_gic *);
_Noreturn void gpu_return_reset(void);
#endif
