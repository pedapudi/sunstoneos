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
/* A failed check returns this record in place of the receipt, with the same
 * magic, version and size positions. Thermal fields come from
 * ss_thermal_status at the instant of the first fault. */
#define CPU_NPU_FAIL_MAGIC UINT64_C(0x314c494146555043)
struct cpu_npu_failure {
    uint64_t magic,version,bytes,stage,core,job,observed,expected,cell;
    uint64_t now_ticks,start_ticks,workload_ticks,mode,completed,ack_mask,sdk_closed;
    uint64_t chunks[8];
    uint64_t thermal_cause,thermal_level,thermal_seen,thermal_passive,thermal_stale,thermal_sequence;
    uint64_t thermal_now_us,thermal_deadline_us,thermal_start_us;
    int64_t millicelsius[5];
    uint8_t reserved[CPU_NPU_BYTES-38*8];
};
_Static_assert(sizeof(struct cpu_npu_cpu)==64,"CPU receipt");
_Static_assert(sizeof(struct cpu_npu_job)==272,"job receipt");
_Static_assert(sizeof(struct cpu_npu_receipt)==CPU_NPU_BYTES,"payload receipt");
_Static_assert(sizeof(struct cpu_npu_failure)==CPU_NPU_BYTES,"failure record");
#endif
