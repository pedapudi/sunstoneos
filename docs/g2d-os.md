# G2D in the OS image

The G2D 2D engine at 0x05440000 is an OS-image service. An application declares the `g2d` device in its manifest, the session grants it as `SS_DEVICE_G2D`, and the application calls the class interface in include/sunstone_g2d.h. One owner task on core 0 (apps/os/g2d.c, priority 4) makes every G2D, CCU and GIC access for the service. The G2D service writes no PCK600 or PMIC register: the shared power-domain owner (drivers/power/a733_power_domains.c) powers domain 1 (DE_SYS) for it.

Status: open and close are implemented and wait for their first board run. Fill, blit, scale and format conversion, the INTID 83 completion handler and the IOMMU mapping are not implemented yet.

## Vendor sequence and the native diff

The reference is the vendor driver in linux-orangepi 2ac08e8c (bsp/drivers/g2d/g2d_rcq, hashes in the source-identities.json beside the copy), the clock tree in ccu-sun60iw2.c and the device tree node g2d@5440000 in sun60iw2p1.dtsi.

| Vendor step | Reference | OS image |
| --- | --- | --- |
| Attach to PCK600 domain 1 before any clock or reset changes | genpd attach at probe; pck600_domains.c 104-155 | The start entry registers a domain-1 row before the scheduler starts. Open requests ON and blocks on the row's wake, with a deadline of 10,000 + 4,000 µs. The owner adopts an ON domain with no write. |
| Deassert the G2D reset (0xa44 bit 16) | `g2d_clock_enable`, g2d.c 580-586 | Same, under the shared CCU register lock, with readback. |
| Deassert the shared DE_SYS reset (0xa74 bit 16) | g2d.c 587-593; optional shared reset, g2d.c 1534 | Same. |
| Enable the DESYS MBUS and DE AHB gates (0x5e0 bit 11, 0x5c0 bit 5) | g2d.c 599-602; both `CLK_IS_CRITICAL` (ccu-sun60iw2.c 597-600, 673-676) | Not written. The power-domain owner requires both open before it admits the row. |
| Set the module rate to 300 MHz from PERI0_300M | `assigned-clock-rates` in sun60iw2p1.dtsi, applied at probe with the gate off | With the gate off, the open writes the CCU 0xa40 word 0x0 (mux 0, divide by 1). PLL_PERI0 is never programmed; the open refuses unless CCU 0xa0 reads it enabled and locked at 1,200 MHz with the 300M output gated on. |
| Enable the bus gate (0xa44 bit 0), then the module gate (0xa40 bit 31) | g2d.c 607-610 | Same; 0xa40 must then read 0x80000000. |
| Open the engine's own gates and resets | `g2d_bsp_open`, g2d_top.c 62-72 | 0x00 \|= 3, 0x04 \|= 3, 0x08 \|= 3, 0x18 \|= 1, each read back. |
| Close: module gate, bus gate, G2D reset, DE_SYS reset | `g2d_clock_disable`, g2d.c 642-669 | Same order. The DE_SYS reset is asserted only when the DE and DI module and bus gates (0xa00 bit 31, 0xa04 bit 0, 0xa20 bit 31, 0xa24 bit 0) read closed; otherwise it stays deasserted and the close record names the open gates. The critical gates stay open. Domain 1 stays ON. |
| Clear the engine's own gates at close | `g2d_bsp_close`, g2d_top.c 74-84 | Not written. It follows the bus gate in the vendor order, and the asserted G2D reset clears those registers. The close clears the RCQ interrupt enables (0x20 bits 4 and 6) before the gates. |

The open adds checks that the vendor driver does not make, each read before the first write:

- CCU 0xa44 & 0x10001 reads 0: the bus gate is closed and the G2D reset asserted.
- INTID 83 is neither pending nor active (GICD_ISPENDR2 and GICD_ISACTIVER2 bit 19).
- A module clock found gated on already reads 0x80000000.
- The power-domain owner's read-only domain-1 check passes: both critical gates and the PPU bus clock (R_CCU 0x1ac bit 0) open, and policy and status agreeing at 0 or 8.

A refusal at any of these writes nothing and leaves the engine closed. A failure after the first write marks the service FAULT, keeps every lease and gate as it is, and makes the quiesce step report fault site 0x931 with the failing step and word.

## The open record

`ss_g2d_open` returns a `struct ss_g2d_info`: the 23 platform words before the first write and after the open (PLL and PERI0 control and gates, the G2D, DE_SYS, DE and DI clock words, the critical gates, the PPU clock, the domain-1 policy, status and delay words, the IOMMU gates, and the INTID 83 enable, pending and active words), the 11 engine-local words including the version register at 0x10, and the timing of the open. The timing covers the row request, the owner's PCK command (0 when it adopted the domain) and the row's ON answer in the power owner's microsecond clock, and the domain-1 status word read at the request and at each row wake. Bit 8 of the policy and status words is recorded separately and refuses nothing. apps/g2d-check/host.py decodes the record of the g2d-check application.
