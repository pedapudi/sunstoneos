#ifndef CPU_NPU_CONCURRENCY_RECEIPT_H
#define CPU_NPU_CONCURRENCY_RECEIPT_H
#include <stdint.h>
#define CPU_NPU_MAGIC UINT64_C(0x315550434e555043)
#define CPU_NPU_BYTES 1728u
#define CPU_NPU_CORES 8u
#define CPU_NPU_JOBS 4u
struct cpu_npu_cpu {
    uint64_t core,chunks,digest,first,last,stop,error,ack;
};
struct cpu_npu_job {
    uint64_t ticket,token,client,model,trigger,first_busy,a_end,b_start,last_busy,irq;
    uint64_t first_idle,last_idle,first_state,last_state,a[8],b[8];
    uint64_t output_bytes,match,irq_count,output;
};
struct cpu_npu_receipt {
    uint64_t magic,version,bytes,mode,core_mask,edge,target,completed,start,stop;
    uint64_t stop_epoch,ack_mask,sdk_closed,overlap_mask,initial_top,final_top;
    struct cpu_npu_cpu cpu[8];
    struct cpu_npu_job job[4];
};
_Static_assert(sizeof(struct cpu_npu_cpu)==64,"CPU receipt");
_Static_assert(sizeof(struct cpu_npu_job)==272,"job receipt");
_Static_assert(sizeof(struct cpu_npu_receipt)==CPU_NPU_BYTES,"payload receipt");
#endif
