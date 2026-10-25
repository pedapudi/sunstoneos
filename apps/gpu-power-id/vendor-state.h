/* SPDX-License-Identifier: Apache-2.0
 * Declarations shared by the units of the vendor-state diagnostic only
 * (apps/gpu-power-id/vendor-state*.c). No default image includes this file.
 */
#ifndef SUNSTONE_GPU_VENDOR_STATE_H
#define SUNSTONE_GPU_VENDOR_STATE_H
#include <stdint.h>

/* Errors of the vendor-state controller beyond enum a733_gpu_power_error,
 * which the shared header fixes for every other image. Each names a GPU
 * clock step that did not take, so the run stops before the identity read
 * with a cause the log names. */
#define VENDOR_STATE_ERROR_PLL 8u     /* a PLL_GPU0 write did not read back */
#define VENDOR_STATE_ERROR_LOCK 9u    /* PLL_GPU0 did not lock within 100 reads */
#define VENDOR_STATE_ERROR_UPDATE 10u /* the GPU clock update bit did not clear */

/* The operating point of the vendor's sunxi_platform_init on this board:
 * 1008 MHz from PLL_GPU0 at DCDC4 960 mV. */
#define VENDOR_STATE_SUPPLY_UV 960000u
#define VENDOR_STATE_DCDC4_ENTRY 0x1eu  /* 800 mV, the selector every board run read */
#define VENDOR_STATE_DCDC4_TARGET 0x2eu /* 960 mV */
#define VENDOR_STATE_PLL_ENTRY UINT32_C(0x41104500) /* disabled, 840 MHz factors, boot0's bit 24 */
#define VENDOR_STATE_PLL_1008 UINT32_C(0xe9105300)  /* enabled, locked bit excluded, N 84, P0 2 */

/* What the controller saw at its PLL and clock-update waits, for the log.
 * It holds observations only; no step reads it back. */
struct vendor_state_clock {
    uint32_t lock_reads[2], lock_run[2]; /* reads of each lock wait; consecutive locked reads at its end */
    uint32_t update_reads[2];            /* reads until each 0xb20 update bit cleared */
    uint32_t pll_last;                   /* the last PLL_GPU0 word the controller read */
    uint32_t pll_steps;                  /* PLL_GPU0 read-backs that matched */
};
extern struct vendor_state_clock vendor_state_clock;

/* The EL3 record of the vendor-state monitor (vendor-state-monitor.c): for
 * each of BL31's security-setup writes it makes, in order, the address,
 * the word read before the write and the word read after it; done counts
 * the completed writes, and magic is set after the last. It lies in the
 * image's 2 MiB result block beyond the 16 KiB that the host reads, in the
 * block EL1 maps normal non-cacheable, so the EL1 client reads what the
 * cache-off monitor wrote and prints it to the log. A table row stores
 * value (STORE) or ORs it into the word read (OR). */
#define VENDOR_STATE_EL3_RECORD UINT64_C(0x42010000)
#define VENDOR_STATE_EL3_MAGIC UINT64_C(0x3130455356555047) /* "GPUVSE01" */
#define VENDOR_STATE_EL3_WRITES 28u
#define VENDOR_STATE_CCU_SWITCH UINT32_C(0x02003f00) /* CCMU_SEC_SWITCH_REG */
struct vendor_state_el3_write { uint32_t address, value, kind; };
enum vendor_state_el3_kind { VENDOR_STATE_EL3_STORE, VENDOR_STATE_EL3_OR };
struct vendor_state_el3 {
    uint64_t magic;
    uint32_t done, reserved;
    uint32_t address[VENDOR_STATE_EL3_WRITES], entry[VENDOR_STATE_EL3_WRITES], after[VENDOR_STATE_EL3_WRITES];
};
#endif
