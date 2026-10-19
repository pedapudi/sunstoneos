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
profile. PMIC, watchdog, host, DMA start and upstream fabric writes are
refused. The explicit ROM takeover mode also permits interrupt masking,
soft-disconnect and assertion of the USB0 device reset. All accesses use Device mappings and full-system barriers.

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

## Supervised ROM USB0 takeover

The existing `cold_reference` configuration word is an explicit start mode:
0 adopts the reference with a cold controller, 1 initializes a cold reference
and controller, and 2 takes over the ROM device controller. The board session
layout remains unchanged. Native session admission allows mode 2 only for the
supervised single-core SD workload; host preparation must bind its takeover
authority to that image and target.

Mode 2 requires USB0 PHY/reset controls `0x40000000` under mask `0xc0000000`
and device bus/reset controls `0x01000100` under mask `0x01110111`. The USB0
host gates and resets must be clear. It reserves the common reference as a
shared consumer and preserves USB1 and the reference registers.

Before mutation, the provider requires device session, host request and host
mode clear, and all sixteen internal DMA start bits clear. FIFO bus selection
is allowed in either state: the pinned `USBC_SelectBus` sets it even for PIO
on newer controllers. It masks endpoint, bus, DMA and interface interrupts,
removes soft-connect, and allows the host 150 ms to observe disconnection.
It then asserts only the device reset for 1 ms, keeping its bus gate enabled,
deasserts reset, waits 40 microseconds and verifies the detached core before
continuing through the existing PHY preparation and TinyUSB consumer.
The 150 ms and 1 ms intervals reuse the physically exercised
`drivers/usb/a733_usb.c` sequence; the 40 microsecond interval comes from the
pinned platform initialization.

Clear DMA start bits alone do not establish completed memory writes: the
pinned DMA disable routine can clear a channel start bit in software. This
mode therefore still requires evidence that previous USB DMA is drained or
that the inspected ROM transfer path used PIO throughout this lifetime, plus
external DMA exclusion/drain. The disconnect interval supplies no bus-drain
proof. GIC exclusion, exclusive controller access, supplies and supervised
power-removal recovery remain admission requirements. Any uncertainty retains
ownership and prevents further MMIO.

The sanitized composition in
`~/.cache/sunstoneos/evidence/usb-rom-handoff-20261018-final/` passes the
existing cold cases and actual second-board clock/control inputs, ordered
ROM takeover, active DMA/host refusal and every failed or ineffective write.
Native MMIO guards pass in `usb-rom-handoff-20261018-native/`, reusing those
host results after correcting the linker path. The focused review accepted
the corrected sequence. Separate session cases accept mode 2 only for the
supervised single-core SD workload and preserve refusal in other builds.
These checks model register behavior. The ROM capture below establishes the
second board's PIO implementation and selector; host admission and physical
native takeover remain required evidence.

## ROM transfer provenance required for takeover

The captured secure ROM under `rom-copy-20261016-084055/` has SHA-256
`a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa` and
covers addresses `0` through `0x10fff`. Its FEL dispatcher loads `0x17000`
at `0x2580` and branches through the `MOV pc,r0` instruction at `0x1061c`.
The capture therefore does not contain the USB/FEL implementation.

The separately pinned nonsecure-ROM reference occupies `0x17000` through
`0x21fff` and has SHA-256
`95be69c916846a8259578565f3193a6b9ab0625294d5374f3dc6c2f667fc6169`.
The following offsets apply only after the target's complete nonsecure-ROM
capture matches that hash. At offset `0x850c`, the mode function reads the
word at `0xf355c`; zero selects USB0, while nonzero selects the separate USB3
implementation. Main dispatch at offsets `0x5a8` through `0x5bc` and transfer
dispatch at `0x27f0` through `0x2838` use that selection.

For USB0, receive processing calls the CPU FIFO-copy helper at offset `0x1de4`;
its byte loop reads a fixed FIFO address and stores to CPU memory at `0x1e38`
and `0x1e3c`. Word and halfword variants perform the same transfer at `0x1e70`
and `0x1e84`. Transmit processing calls the helper at `0x1ed8`, whose loop
loads CPU memory and writes a fixed FIFO address at `0x1f30` and `0x1f34`.
The ordinary FEL read/write commands reach these helpers through transfer
submission at `0x27e0`. Endpoint initialization at `0x18f0` writes CSR constants
with DMA request enable clear. Execute processing sends its acknowledgment at
`0x3118`, waits for TX packet-ready to clear at `0x312c`, and calls the target
at `0x3134`. This path preserves the USB controller. These anchors support a
PIO handoff for the matched implementation and reviewed FEL command history;
they do not establish the effects of arbitrary executed payloads.

Prior storage and DDR operations need separate external DMA evidence. The
captured secure boot path can call storage dispatch at `0xbd0` before entering
FEL. Its DMA disable helper at `0x1090` clears channel controls without polling
the DMA status register. The pinned DDR wrapper calls board initialization at
`0x48fad` and DRAM initialization at `0x49bdd`; the wrapper model substitutes
both calls and cannot establish their DMA effects. The A733 manual section
13.1.7.3 defines external DMAC status at `0x04601030`: bit 31 reports a nonempty
MBUS FIFO, and bits 15 through 0 report busy channels. Observing those fields
clear, channel enables clear and writer exclusion supplies concrete current
state evidence after the DMAC clock/reset access conditions are admitted.
It does not follow from a host authority boolean.

The second board's returning capture on port `3-3` matches both ROM hashes
above, and the subsequently read mode word at `0xf355c` is zero. The record is
`second-board-20261018-3-3/rom-usb-capture/runtime/session.json`. Independent
hash comparisons and byte comparisons confirm the captured ROM identities
and exact restoration of the DDR buffer and 128-byte helper workspace.
Detailed findings and input identities are preserved in
`usb-rom-handoff-20261018-native/rom-provenance.json`.

Normal USB0 entry calls offset `0xa20`: offset `0x948` asserts the device reset
and disables its gate, `0x9a8` asserts PHY reset, and `0x968` deasserts the device
reset and enables the gate. PHY and endpoint PIO setup follows at `0x1cf0`.
This initialization and the selected USB0 transfer loop do not program internal
DMA channels. They create no undrained internal USB DMA during ordinary FEL
reads, writes and execute acknowledgments. The existing native checks of all
sixteen channel start bits therefore supply the smallest current internal DMA
state check for this recorded, trusted PIO history. A previous unreviewed payload
that enabled USB DMA would require its own completion evidence; a clear start
bit would not establish its drain. The matched PIO path does not establish
external DMAC completion after storage or DDR initialization.

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

## Bounded native observation and return

The [finite USB diagnostic](../apps/usb-returning/README.md) composes the existing
PMIC and USB providers with the accepted returning FreeRTOS platform. It observes
one complete PIO response within a fixed window, parks PMIC work, masks controller
interrupt generation, detaches, and verifies quiescence before the monitor enters
the source-backed forced-FEL initialization path. Every resource stays retained
through reset. Observation expiry reports a controlled negative; it does not
establish a successful transfer. One supervised image returned status 1 before
TinyUSB initialization; its original report omitted the failing platform stage
and write history. The provider now preserves the first failed predicate and
separates no-access metadata BUSY from I/O failure. It retries only polls without
a successful write; partial write/reset state stays retained. Focused software
checks pass. The corrected image remains physically unqualified and requires
fresh admission after the failed lifetime's cold recovery.
