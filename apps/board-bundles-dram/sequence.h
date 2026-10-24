/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BOARD_BUNDLES_DRAM_SEQUENCE_H
#define SUNSTONE_BOARD_BUNDLES_DRAM_SEQUENCE_H
/* Bundle 8 of docs/designs/board-bundles-next.md: the gates of the NSI
 * access, then the reads, table write and readback of NSI ports 2, 14 and 15
 * through a733_dram_priority(), with the USB_PCIE port fields read before and
 * after. Every access goes through the caller's register pair; the sequence
 * itself touches no hardware, so the host model runs it unchanged. */
#include "a733_dram.h"

#define BB8_R_CCU_PPU UINT32_C(0x070101ac)       /* PPU bus clock gate, bit 0 */
#define BB8_DOMAIN1_STATUS UINT32_C(0x07061008)  /* PCK600 domain 1 PPU_PWSR */
#define BB8_USB_PCIE_PORT 13u                     /* MBUS_PMU_USB_PCIE, sunxi-nsi.h */
#define BB8_FIELDS (3u*A733_DRAM_PORTS)

/* The stage that stopped the run before any NSI access; 0 when every gate is
 * open. A skipped run is inconclusive and admits no NSI comparison. */
enum bb8_skip { BB8_READY, BB8_SKIP_PPU_CLOCK, BB8_SKIP_DOMAIN1, BB8_SKIP_NSI_CLOCK, BB8_SKIP_NSI_CONFIG };
enum bb8_result {
    BB8_WRITTEN,          /* table written and read back; USB_PCIE fields unchanged */
    BB8_MISMATCH,         /* a field read back another value; block.failed names it */
    BB8_GATE_CHANGED,     /* the hook found a gate closed after the reads */
    BB8_USB_PCIE_CHANGED, /* a USB_PCIE field differs after the table write */
    BB8_ACCESS_FAILED     /* the register pair refused an access, as when its trace is full */
};
struct bb8_record {
    uint32_t ppu_clock, domain1_status, nsi_clock, nsi_config, mbus_clock;
    uint32_t inherited[BB8_FIELDS];          /* whole words before the hook */
    uint32_t usb_pcie_before[3], usb_pcie_after[3];
    struct a733_dram_block block;            /* the hook's own record */
    unsigned domain1_read, skipped, possibly_protected;
};
/* The address of field i (port 2, 14, 15; +0x10, +0x14, +0x18) and its mask. */
uintptr_t bb8_field(unsigned i);
uint32_t bb8_mask(unsigned i);
uintptr_t bb8_usb_pcie(unsigned i);
/* R_CCU 0x1ac; domain 1 status only when its bit 0 reads 1; CCU +0x580,
 * +0x584 and +0x588. Returns 1 when every gate is open and domain 1 reads ON,
 * 0 with skipped set otherwise, and -1 when the pair refused an access. */
int bb8_gates(const struct a733_dram_io *, struct bb8_record *);
/* After bb8_gates() returned 1: the nine field reads, the USB_PCIE reads, the
 * hook in table mode, and the USB_PCIE reads again. */
enum bb8_result bb8_nsi(const struct a733_dram_io *, struct bb8_record *);
#endif
