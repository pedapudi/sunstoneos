#ifndef SUNSTONE_ACCELERATOR_SESSION_H
#define SUNSTONE_ACCELERATOR_SESSION_H
#include "../npu-convolution/session.h"
#include "pvr_compute_package.h"
#define ACCELERATOR_SESSION_MAGIC UINT64_C(0x4153373341434131)
#define ACCELERATOR_CONTRACT_MAGIC UINT64_C(0x4153373341434331)
#define ACCELERATOR_SESSION_LIMIT UINT64_C(65536)
#define ACCELERATOR_GPU_CLIENT 7u
#define ACCELERATOR_GPU_FIRMWARE_OWNER UINT64_C(9)
#define ACCELERATOR_GPU_COMPUTE_OWNER UINT64_C(10)
/* Uploaded at NPU_SESSION_ADDRESS, authenticated and copied before cache
 * adoption. The complete package exceeds one page. All evidence covers this
 * combined lifetime, including shared providers and both device reservations.
 * Digests identify externally reviewed facts; they cannot establish them. */
struct accelerator_session {
    uint64_t magic,version,bytes;
    struct npu_session npu;
    uint8_t gpu_initial[32],gpu_memory[32],gpu_electrical[32],gpu_supervision[32];
    uint8_t package_digest[32];
    struct pvr_compute_package package;
};
_Static_assert(sizeof(struct accelerator_session)<=ACCELERATOR_SESSION_LIMIT,
    "combined session must remain below protected monitor memory");
_Static_assert(NPU_SESSION_ADDRESS+ACCELERATOR_SESSION_LIMIT<=UINT64_C(0x42000000),
    "combined upload overlaps monitor");
int accelerator_session_valid(const struct accelerator_session *);
const struct accelerator_session *accelerator_session_get(void);
const struct pvr_compute_package *accelerator_gpu_package(void);
extern const uint64_t accelerator_session_layout[8];
#endif
