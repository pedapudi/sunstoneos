# PCIe starts a cold direct endpoint under retained native ownership

The initial native profile implements the A733 COMB1 internal 100 MHz PHY
sequence, PCIe root-complex preparation, an x1 Gen1 link and read-only
configuration access to one directly attached endpoint. FreeRTOS owns the
controller. Linux and U-Boot supply register semantics and comparison helpers;
neither is a runtime dependency. The [supervised native application](pcie-application.md)
connects these providers to authenticated entry and copied USB results. Physical
PCIe operation remains unqualified.

The controller has sixteen inbound and sixteen outbound translation windows,
four embedded DMA read channels and four write channels. Gen2/Gen3 operation,
bridge enumeration, BAR allocation, INTx/MSI service, DMA, endpoint drivers,
power transitions and shutdown remain required follow-on work. The current
READY result does not authorize those operations.

## The board handoff identifies the actual endpoint circuit

The V1.2 schematic exposes a sixteen-pin FPC connector on sheet 16. Pins 1–2
carry 5 V; pins 4–5 carry the reference clock, 7–8 receive data and 10–11 transmit
data. Pins 13–16 are power enable, WAKE, CLKREQ and PERST. Sheet 9 routes the
sidebands to PD23, PD21, PD20 and PD22 respectively.

This profile requires external endpoint power already stable and PD22 already
driving active-low PERST. It adopts PD20, PD21 and PD23 as inherited input or
disabled pins. It never drives PD23 or infers connector power from a device-tree
supply name. The board owner authenticates the exact carrier, endpoint, signal
voltage, reset polarity and power sequence before admission.

The initial profile follows sheet 2's VDD-COMB1 assignment to DCDC2 at 0.8 V
and its PD-bank assignment to BLDO2 at 1.8 V. The later fan/LED routing review
identifies VCC-PD on BLDO1 in the detailed power sheet. These assignments conflict;
the current PCIe profile is not ready for physical use until the fitted board's
PD supply and the profile's rail ownership agree. A BLDO2 snapshot cannot establish
BLDO1 stability. Reconcile the wiring and update the application contract before
an attempt; do not infer a regulator substitution from a device-tree name.

The current runtime acquires DCDC2 and BLDO2 stable rail reservations and obtains
page/identity/enable/selector/page snapshots through the shared PMIC provider.
BLDO2 uses enable register 0x20 bit 7 and selector register 0x2b; selector 13
programs 1.8 V. These observations prove programmed settings, not measured
voltage or board wiring.

The board device tree names BLDO1 as PCIe 1.8 V and DCDC1 as PCIe 3.3 V, and
names CLDO5 as SERDES 1.8 V. The schematic instead places BLDO1 on PE/PK/camera
supplies and labels CLDO5-derived analog power for COMB0. Those names do not
establish the external FPC power circuit or every COMB1 analog supply. The
authenticated electrical handoff must resolve the actual board topology.

## Shared providers retain one lifetime

The port reserves PCIE0 and COMB1 exclusively. It holds SERDES_COMMON and
USB_PCIE_DOMAIN shared, together with PERI0, REF, OSC and FABRIC. These are
actual tokens in the common clock registry; a token alone proves no frequency
or electrical state. The selected AHB/NSI/MBUS profiles are supplied explicitly
and checked against source-backed rate calculations.

The actual global resource manager grants a PCK bus-clock lease. The port
observes domain 7 policy/status ON, disabled autonomous PCIe power control and
effective ON status. The SERDES global clock must already select PERI0 600 MHz
divided by six, with its reset deasserted and AHB/MBUS gates enabled. The port
does not reset or retune shared SERDES, change domain policy or write a PMIC
voltage. Future global transitions must exclude COMB0, USB3 and every other
affected consumer.

The pin manager owns PD20–23 for the entire port lifetime. Acquisition checks
the inherited GPIO-output-low condition before mutation under the pin manager's
shared lock. The port holds PERST low while it initializes COMB1, then waits at
least 100 ms after successful PHY initialization before raising PERST. This
matches the source ordering of PHY preparation followed by the endpoint reset
hold. No running inherited endpoint is converted into a cold endpoint by an
unapproved reset.

Every register operation uses the actual common FreeRTOS register mutex. Pin
operations and resource health checks occur outside that mutex, avoiding nested
provider locks. The native adapter requires the designated EL1 task, a running
scheduler, enabled task interrupts and no interrupt/critical-section nesting.
The boot handoff excludes competing secure firmware, remote processors and
interrupt handlers for INTIDs 168–175, 184 and 185.

## Controller access is bounded and retains uncertain state

The DesignWare configuration base is 0x06000000, the unrolled address
translation unit is at 0x06300000, and the application registers are at
0x06400000. COMB1 uses top registers at 0x06c02000 and PHY registers at
0x06ca0000. The configuration aperture occupies the first 4 KiB of
0x20000000. The native adapter permits the selected registers and access widths
and refuses endpoint configuration writes.

Local PCIe AUX uses SYS24M and AXI uses PERI0 400 MHz. The inherited parent
checks require No-Auto mode, software enable and effective gate status for
every selected branch. The native controller keeps GIC delivery masked and
checks pending/active state, observes both link-up bits, bounds capability-list
walking and refuses cyclic or malformed lists. PCI command accesses use 16-bit
writes so adjacent status write-one-to-clear bits are preserved.

One task serializes configuration access through outbound window zero. It
disables that owned window before retargeting, observes enablement, and emits a
CFG0 transaction for bus 1, device 0. Function numbers above zero require the
observed multifunction header. Each public read checks link and live resource
ownership and leaves caller output unchanged after refusal or uncertainty.
Endpoint bus mastering and MSI/MSI-X enablement must initially be clear.

Absolute operation and lifetime deadlines use the common monotonic clock.
PHY readiness and link observation advance through repeated bounded polls.
A deadline bounds software progress; it cannot bound a hung physical MMIO
transaction. Qualified independent recovery is required before physical use.
An uncertain access, late result, resource fault or failed invariant retains
leases and any held register mutex. There is no automatic rollback, retry reset,
resource release or memory reclamation.

## Translation does not establish hostile-device isolation

Manual section 18.3.5.2 states that unused inbound translation forwards incoming
PCIe addresses unchanged. Disabled inbound windows therefore do not block DMA.
The cold endpoint must obey PERST and disabled bus mastering. This is a trusted
endpoint contract, not a hardware isolation claim.

The pinned `pci-epf-test` endpoint function provides a concrete follow-on
protocol: READ, WRITE and COPY commands specify host addresses, byte counts,
checksums and completion interrupts. A native endpoint client can retain actual
memory reservations and cache ownership for those commands. It must admit the
specific endpoint controller's ordering and completion semantics before treating
MSI/status as completion. The protocol has no cancellation acknowledgement or
transaction identifier, so a timed-out operation retains its buffers and blocks
reuse. A separate A733 embedded-DMA path can target an admitted endpoint scratch
BAR and use its defined nonposted read/flush contract. Generic MSI receipt or
link-down alone is not a drain proof.

## Primary references and validation scope

The register sources are the A733 User Manual V1.00, board schematic V1.2,
board kernel revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` and U-Boot
revision `b791be842935b27268ae3d00e943a9075495f30a`. Research inputs and exact
hashes are retained outside Git in the local `pcie-design-20261016` evidence
directory.

The manual's initialization guide conflicts with its detailed reset fields:
section 18.3.6.1 describes a two-bit SERDES reset and PCIe reset value 2. Detailed
CCU sections 4.1.6.207/209 and both source implementations instead define PCIe
reset bits 17/16 as deasserted at 1 and SERDES reset bit 16 as deasserted at 1.
The implementation follows those detailed fields. The AXI mux-1 parent also
differs between the manual and kernel table; the profile uses their agreed
mux-2 400 MHz selection. SERDES debug bit 29 is named a reference-clock gate
by the manual despite the source helper's automatic-gating name.

The focused check composes the actual clock registry, rail ledger, PMIC provider,
PCK resource manager and pin manager. It compares PHY and CFG0 programming with
executed primary-source helpers, checks BLDO2 decode boundaries and exercises
inherited-state, timing, deadline and retained-fault cases. Hardware responses
and mutex scheduling are modeled. The reviewed batch passed with address and
undefined-behavior sanitizers. The PCIe runtime, shared PMIC provider and changed
pin-manager unit compiled for AArch64. Exact inputs, executables and results are
retained in `~/.cache/sunstoneos/evidence/pcie-20261016/batch-3/results.json`.
The unchanged source-helper execution was reused from its preserved first
batch. Two initial fixture failures, caused by an incorrect GPIO-bank address,
remain archived; the PCIe runtime did not change during validation. No physical
PCIe result is claimed.
