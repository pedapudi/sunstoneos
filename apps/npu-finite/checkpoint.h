#ifndef SUNSTONE_NPU_ENTRY_CHECKPOINT_H
#define SUNSTONE_NPU_ENTRY_CHECKPOINT_H
#include <stdint.h>
#define NPU_ENTRY_MEMORY_READY UINT64_C(0x4e50554e)
#define NPU_ENTRY_RECEIPT_MAGIC UINT64_C(0x4e505543484b3031)
/* Scalar checks only: safe at EL3 after the complete private interval is clean.
 * The monitor supplies these identities from its protected admitted contract. */
int npu_entry_checkpoint_ready(uint64_t epoch,uint64_t generation,uint64_t lease,
    uint64_t armed_at,uint64_t deadline);
#endif
