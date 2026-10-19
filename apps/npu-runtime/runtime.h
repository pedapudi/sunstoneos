#ifndef SUNSTONE_NPU_RUNTIME_H
#define SUNSTONE_NPU_RUNTIME_H
#if defined(SUNSTONE_NPU_LARGE_GRAPH)
#include "../npu-runtime-large/runtime.h"
#else
#include "a733_npu_application.h"
#define NPU_RUNTIME_SUBMIT 0x34u
#define NPU_RUNTIME_READ 0x35u
#define NPU_RUNTIME_CLOSE 0x36u
#define NPU_RUNTIME_FRAME UINT64_C(0x415337334e505232)
enum npu_runtime_state { NR_IDLE,NR_UPLOAD,NR_QUEUED,NR_RUNNING,NR_DONE,NR_CLOSING,NR_CLOSED,NR_FAILED };
struct npu_runtime_slot {
 atomic_uint state,reason;
 uint64_t id,deadline,start,end,job;
 unsigned operation,bytes,jobs,irq_count;
 uint8_t input[NPU_CONVOLUTION_IO_CAPACITY],output[NPU_CONVOLUTION_IO_CAPACITY];
 uint8_t input_digest[32],output_digest[32];
};
extern struct npu_runtime_slot npu_runtime;
#define NPU_RUNTIME_INPUT npu_runtime.input
#define NPU_RUNTIME_OUTPUT npu_runtime.output
int npu_runtime_live(void);
void npu_runtime_fail(unsigned);
/* USB owner alone, exact header then bounded payload fragments. */
int npu_runtime_header(const uint8_t[64]);
int npu_runtime_payload(const uint8_t *,unsigned);
int npu_runtime_frame(uint8_t[1024],unsigned *,uint64_t *);
void npu_runtime_ack(unsigned,uint64_t);
#endif
#endif
