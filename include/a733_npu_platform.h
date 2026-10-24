#ifndef SUNSTONE_A733_NPU_PLATFORM_H
#define SUNSTONE_A733_NPU_PLATFORM_H
#include "a733_clock_leases.h"

/* The NPU fabric record and the result and register-callback types of the
 * startup sequencer (docs/npu-platform.md). */
enum a733_npu_platform_result { A733_NPU_PLATFORM_READY, A733_NPU_PLATFORM_BUSY,
    A733_NPU_PLATFORM_PENDING, A733_NPU_PLATFORM_RETAINED };
/* AHB, NSI and MBUS selector, divisor and enable fields, and their exact
 * integer rates. The register pass compares the live words with them. */
struct a733_npu_platform_config {
    uint32_t fabric[3], rate_hz[3];
};
/* Per fabric word (AHB, NSI, MBUS): the documented fields, and the parent
 * rate in MHz of each selector value, 0 where the selector is refused. */
extern const uint32_t a733_npu_fabric_masks[3];
extern const uint16_t a733_npu_parent_mhz[3][8];
/* Pure validation of the fabric record. Writes the shared clock scopes the
 * NPU needs (fabric, reference, oscillator, and PERI0 when a fabric clock
 * selects it). Refusal leaves the output unchanged. */
int a733_npu_platform_scopes(const struct a733_npu_platform_config *, a733_clock_mask_t *);
/* Read and write callbacks of one register sequence; each returns 1 after an
 * ordered 32-bit access. */
struct a733_npu_register_ops {
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
};
#endif
