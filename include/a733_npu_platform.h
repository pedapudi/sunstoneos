#ifndef SUNSTONE_A733_NPU_PLATFORM_H
#define SUNSTONE_A733_NPU_PLATFORM_H
#include "a733_clock_leases.h"

/* The NPU-local clock and reset tables of the startup sequencer
 * (docs/designs/npu-board.md). The fields follow the pinned board kernel:
 * CCU+0xb00 module divider bits 0-4, selector 24-26, gate 31; CCU+0xb04 bus
 * gate 0, core reset 16, AXI 17, AHB 18, SRAM reset 19. */
#define A733_NPU_INITIAL_HZ UINT32_C(492000000)
#define A733_NPU_INITIAL_UV UINT32_C(800000)
enum a733_npu_platform_result { A733_NPU_PLATFORM_READY, A733_NPU_PLATFORM_BUSY,
    A733_NPU_PLATFORM_PENDING, A733_NPU_PLATFORM_RETAINED };
/* AHB, NSI and MBUS selector, divisor and enable fields, and their exact
 * integer rates. The register pass compares the live words with them. */
struct a733_npu_platform_config {
    uint32_t fabric[3], rate_hz[3];
};
/* Pure validation of the fabric record. Writes the shared clock scopes the
 * NPU needs (fabric, reference, oscillator, and PERI0 when a fabric clock
 * selects it). Refusal leaves the output unchanged. */
int a733_npu_platform_scopes(const struct a733_npu_platform_config *, a733_clock_mask_t *);
/* One register write of a table: the masked field takes value, every other
 * bit keeps its read value. Bits in status are read-only status: they are
 * written as zero and ignored by the readback, as PLL LOCK is. */
struct a733_npu_step { uint32_t address, mask, value, status; };
/* Opened after PLL_NPU locks, in the clock-before-power order of the pinned
 * VIPLite vip_drv_device_platform.c (lines 419-428): PLL output gate, AHB,
 * AXI and core resets released, module selector PLL_NPU at divider 1, bus
 * gate, module gate. */
#define A733_NPU_LOCAL_OPEN_STEPS 7u
extern const struct a733_npu_step a733_npu_local_open[A733_NPU_LOCAL_OPEN_STEPS];
/* npu_clk_uninit in the pinned VIPLite platform: module gate, bus gate, then
 * the AHB, AXI and core resets. PLL_NPU, the SRAM reset and domain 4 stay. */
#define A733_NPU_LOCAL_STOP_STEPS 5u
extern const struct a733_npu_step a733_npu_local_stop[A733_NPU_LOCAL_STOP_STEPS];
/* Read and write callbacks of one register sequence; each returns 1 after an
 * ordered 32-bit access. */
struct a733_npu_register_ops {
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
};
/* Apply steps [first, count) with a readback of each write; a field that
 * already holds its value is not written. Returns the number of steps that
 * completed; *wrote counts the writes. */
unsigned a733_npu_apply(const struct a733_npu_register_ops *, void *,
    const struct a733_npu_step *, unsigned first, unsigned count, unsigned *wrote);
#endif
