# Cold USB0 device preparation

The USB platform driver initializes the A733 USB0 reference, device bus clock,
reset, PHY setting and device route. It finishes with the controller detached,
interrupt generation disabled and all sixteen internal DMA channels inactive.
Source comparison, sanitized host composition and native instruction checks
pass. Physical clocks, PHY behavior, connector power and recovery remain
unqualified.

This is cold initialization of the USB0 device hardware under admitted upstream
conditions. It requires established supplies and fabric clocks. It does not
replace the inherited-clock adapter used by the existing [USB runtime](usb.md).
Connecting that runtime requires an adapter that adopts the retained owner and
avoids repeating clock/reset initialization outside the shared registry.

## Board routing constrains the roles

The [V1.2 schematic](https://drive.google.com/file/d/1layhtSvMpQo-z_iMJjNfNVvhPhObpsvW/view),
sheet 11, connects USB0 to the power-input Type-C socket. Both configuration
channel pins have fixed 5.1 kΩ sink resistors. VBUS connects directly to the
board's VCC-5V supply. This socket has no software-controlled VBUS source or
configuration-channel role switch. The driver admits only a device session
with externally established power and cable ownership.

The separate Type-C/DisplayPort socket carries USB2 and the combo-PHY lanes.
Its FUSB302 controller uses S_TWI1; a SY6280 switch controls VBUS through
TYPEC_EN, mapped to PL8 in the board description. That route requires its own
Type-C, power, orientation and USB2/DWC3 drivers. Sheet 10 shows USB1 data pins
without a connector net; a usable physical route remains to be established.

The SoC's host capabilities remain in scope. USB0 host mode cannot be qualified
by treating the fixed-sink socket as a switchable source. USB1 EHCI/OHCI and
USB2 host/device/OTG operation require separate implementations and physical
routes. Publishing the common USB0/1 reference does not establish USB1 PHY or
controller operation.

## Ownership precedes every register access

The driver acquires an exclusive USB0 scope from the common clock registry.
That scope covers the device, EHCI/OHCI companions, shared PHY and role writers.
It also reserves fabric and selected parent clocks against retuning. The common
register mutex serializes writes to shared register words.

Reference initialization additionally acquires exclusive USB01_REF and verifies
that both ports have their bus gates disabled and resets asserted. Native port
users reserve this parent through the registry. The external authority must
exclude remote firmware and other consumers that the registry cannot observe.
The adoption mode requires an enabled 24 MHz PHY reference: selector0
SYS_CLK24M, or selector1 DCXO only when the decoded oscillator is 24 MHz.
Reference initialization preserves selector1 for 24 MHz DCXO and selects
SYS_CLK24M for 19.2 or 26 MHz DCXO. Final verification requires the captured
selector and gate unchanged; selector1 also requires DCXO to remain 24 MHz.
These checks do not authorize taking over an active ROM USB controller: USB0
must still satisfy the disabled/reset baseline before any mutation.

After final readback, reference initialization atomically replaces its exclusive
parent scope with a shared reservation. The new token retains USB0 ownership;
the previous token becomes invalid. USB1 may then acquire its own local scope.
A consumer must adopt the new token before accessing USB0.

An initialized object is bound to its address and refuses byte copies before
callbacks or atomic operations. Completed polls authenticate the retained token
and registry health. They do not repeat hardware observations. The operation
deadline bounds initialization; it does not release completed ownership.

## Native sequence and admission

The native adapter permits only the addresses and fields used by this device
profile. PMIC, watchdog, host, soft-connect, DMA and upstream fabric writes are
refused. All accesses use Device mappings and full-system barriers.

| Register | Operation |
| --- | --- |
| CCU `0x1340` | Disable the common reference, select a 24 MHz source, enable it; preserve other fields |
| CCU `0x1304` | Deassert USB0 device reset, then enable its bus gate; preserve host controls |
| USB0 `0x410` | Clear PHY loopback and SIDDQ; set external VBUS-valid under admitted sink-session ownership |
| CCU `0x1300` | Deassert USB0 PHY reset; preserve the OHCI clock and selector |
| USB0 `0x420` | Select the device controller's PHY route |
| USB0 `0x418` | Apply the board's `phy_range=0x188`, encoded as `0x806` under mask `0xf3f` |
| USB0 `0x400` | Force device ID and valid VBUS; write zero to W1C edge-status bits `0x70` |

CCU is at `0x02002000`; USB0 is at `0x04100000`. The source sequence waits
10 μs after reference enable and 40 μs after device bus enable before PHY
programming. Polling returns while these absolute counter deadlines are pending.
There is no busy wait or additional assumed PHY delay.

Before PHY programming and at final verification, the driver checks soft-connect,
session/request controls, endpoint and USB interrupt enables, DMA interrupt
enables, all sixteen DMA start bits, and the interface-status register's edge
interrupt enables. Readback preserves pending W1C flags. These observations do
not prove completion of previously accepted external memory transactions.

Upstream admission checks AHB, NSI and MBUS selectors and rates, MSI_LITE2 gate
and resets at CCU `0x5a4`, USB_SYS at `0x5c0`, and MSI_LITE2 MBUS at `0x5e0`.
It decodes the detected DCXO rate and requires enabled, stable DCXO and exact
24 MHz PLL_REF, with selected PERI0 outputs checked when used.
The corresponding FABRIC scope remains shared; this driver never changes those
global controls. Complete cold fabric initialization is separate required work.

The platform authority must hold the USB supplies, connector session, reset and
security permissions throughout the operation. USB0 GIC INTIDs 188, 189 and 190
must be disabled without pending or active interrupts or an executing ISR.
Actual rail, interrupt, external-DMA exclusion and independent-recovery bindings
remain required. The driver does not guess PMIC voltages or count authorization
callbacks as physical evidence.

A failed observation, write, deadline or token check retains ownership. An
uncertain register operation also retains the register mutex. If registry fault
retention is busy, later polls retry only that metadata operation. Terminal
fault polls issue no callbacks or MMIO. There is no release API until a device
binding can establish complete shutdown and drain.

## Sources and focused evidence

The pinned [A733 kernel source](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f)
supplies the CCU fields, device-tree clock bindings, USB0 reset/PHY sequence,
register definitions and sixteen-channel DMA layout. The relevant USB files are
`sunxi_udc_board.c`, `sunxi_udc_dma.h`, `usbc.c`, `usbc_phy.c`, `usbc_dev.c` and
`sunxi_usb_bsp.h`. The A733 user manual supplies clock routing and register
semantics. Its USB3 interface-status register is a separate block; USB0's W1C
mask comes from the USB0 source definitions.

Downloaded references and hashes are under
`~/.cache/sunstoneos/sources/usb-platform/`. Evidence is under
`~/.cache/sunstoneos/evidence/usb-platform-20261016/`. The sanitized composition
passed in `batch2`; native adapter execution passed in `native-followup` using
the same reviewed runtime inputs. The latter reused the successful host and
source checks after correcting the linker's command-line option.

The checks execute the published PHY encoding function, exercise ownership and
failure paths with modeled registers, and run native MMIO guards in Unicorn.
They preserve exact executables, sources, commands and logs. No USB application,
mimus workload, board operation or performance benchmark was repeated.
