#ifndef SUNSTONE_NPU_FINITE_RETURN_H
#define SUNSTONE_NPU_FINITE_RETURN_H
#include <stdint.h>
#define NPU_FINITE_RECEIPT_MAGIC UINT64_C(0x4e50555245543031)
/* Trusted fixed-workload receipt. This is not replacement physical admission.
 * EL1 cleans its complete fixed interval before the EL3 monitor consumes it. */
struct npu_finite_receipt {
    uint64_t magic,epoch,generation,lease,status,complete;
    uint64_t clean_start,clean_end,clean_lines,deadline,log_length;
};
extern struct npu_finite_receipt npu_finite_receipt;
extern unsigned char __npu_clean_start[],__device_pool_end[];
_Noreturn void npu_finite_smc(unsigned status);
#endif
