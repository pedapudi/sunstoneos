# PCIe starts a cold direct endpoint under retained native ownership

The initial native profile implements the A733 COMB1 internal 100 MHz PHY
sequence, PCIe root-complex preparation, an x1 link and read-only
configuration access to one directly attached endpoint. Both applications
select Gen1 (2.5 GT/s). FreeRTOS owns the
controller. Linux and U-Boot supply register semantics and comparison helpers;
neither is a runtime dependency. The [supervised native application](pcie-application.md)
connects these providers to authenticated entry and copied USB results. Physical
PCIe operation remains unqualified.

The controller has sixteen inbound and sixteen outbound translation windows,
four embedded DMA read channels and four write channels. The separate
[endpoint test application](pcie-test-application.md) implements one selected
BAR, one MSI and one endpoint-DMA COPY offline. Configuration-only READY does
not authorize those operations.

A general function client configures any cold type-0 endpoint function: it
assigns every memory BAR and delivers interrupts by polling, INTx or MSI-X. It
also moves the function between D0 and D3hot and stops it in order. A
read-only NVMe driver is its first consumer, and the port itself can stop by
holding the endpoint in PERST. Host models cover these paths and both new
units compile for AArch64. No image links them yet, and none of them has run
on hardware. Bridges and switches, embedded root DMA, ASPM, physical
Gen2/Gen3 links and physical power transitions remain required.

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

The fixed native profile reserves DCDC2 for the 0.8 V core supply and BLDO1
for the 1.8 V PD bank. The detailed power drawing on sheet 6 identifies U1B
pin P6 as BLDO1, with C19 at 2.2 µF. Its lower-left branch connects VCC-PD,
VCC-PL, VCC-IO and VCC-PE. The summary on sheet 2 instead lists VCC-PD under
BLDO2. It is not sufficient evidence of fitted-board routing. Admission requires
the actual BLDO1-to-PD route and stable 1.8 V separately from the core route.

The runtime obtains page/identity/enable/selector/page snapshots through the
shared PMIC provider. BLDO1 uses enable register 0x20 bit 6 and selector register
0x2a; selector 13 programs 1.8 V. BLDO2's bit 7 and selector 0x2b cannot establish
PD-bank stability. Programmed settings do not establish measured voltage or
actual board wiring. Shared BLDO1 reservations coexist with stable PD/PL/PE
readers; an exclusive writer blocks PCIe before pin, PCK or controller mutation.
No regulator value is written.

The core and analog consumers are separate. Sheet 2 names VDD-COMB1 under
DCDC2; sheet 6's detailed DCDC2 branch names VDD-COMB0. The fitted core route
and stable 0.8 V therefore remain explicit physical admission requirements.
The selected DTS names BLDO1 as PCIe 1.8 V, DCDC1 as PCIe 3.3 V and CLDO5 as
SERDES 1.8 V. Sheet 6 names CLDO5's analog net AVDD-H-COMB0. None of these
labels identifies a separate COMB1 analog consumer of BLDO2. The application
requires the fitted COMB1 analog net, actual source, voltage and exclusion of
writers for the retained lifetime. It does not guess an analog rail or infer
external FPC endpoint power from a device-tree supply name.

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
least 100 ms after successful PHY initialization before raising PERST. After
raising PERST, it waits at least another 100 ms before the first configuration
request. The pinned U-Boot `pcie_sunxi_rc.c` (SHA-256
`15424d757efafbf79e0b68d48da526cedb7ec9fcc30f03f6668f66ced52ef2bd`,
lines 375–379) has the same two 100 ms delays, around the PERST release. No
running inherited endpoint is converted into a cold endpoint by an unapproved
reset.

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

Local PCIe AUX uses SYS24M and AXI uses PERI0 400 MHz. SYS24M is the normalized
24 MHz PLL_REF output, and admission checks it with the shared
`a733_clock_ref24_valid()` decoder. The COMB1 PHY also takes the raw DCXO
oscillator through the DCXO_SERDES1 gate (0x0709016c bit 5), and its fixed
91-entry register table is verified only for a 24 MHz oscillator. Admission
therefore also requires a 24 MHz DCXO and refuses a 19.2 MHz or 26 MHz board,
even when its PLL_REF factors produce a valid SYS24M. The inherited parent
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

One core helper programs every outbound translation region. It disables the
region, observes the enable bit clear, writes base, limit and target, and then
enables the region. Region zero accepts only the CFG0 window for bus 1,
device 0 and a function from 0 to 7. Region one accepts one identity-mapped
memory window inside the DTS memory aperture, `0x22000000..0x27ffffff`, and
only after a client has claimed the port. The core records the
last programming of each region. The native write filter admits an outbound
translation write only when its value repeats that record, so no other region
and no other target can be written.

## Link speed is selected by configuration

The configuration field `link_speed` selects 1, 2 or 3, meaning 2.5, 5 or
8 GT/s. The pinned board kernel device tree, `sun60iw2p1.dtsi`, gives this
controller `max-link-speed = <3>`, which is the source for admitting speeds
above 1. Root
preparation writes the selected speed into the maximum-speed field of Link
Capabilities and the target-speed field of Link Control 2. The link first
trains at 2.5 GT/s.

For a speed above 1, the port then follows the board kernel's
`sunxi_pcie_host_speed_change()`. It confirms the Link Control 2 target, clears
and then sets bit 17 of DBI+0x80c to direct a speed change, and polls until the
controller clears that bit. Only then does it program the configuration window.
A change that never completes retains the port at the operation deadline.

The negotiated link must be x1 at a speed from 1 to the selected target, so an
endpoint limited to 2.5 GT/s is accepted at that speed. The first identity read
records the link status. Every later identity check and the endpoint test
client require that same status, and a retrain to another speed retains the
port. The kernel also raises an NSI bus bandwidth limit for faster links. The
port leaves that limit unchanged, because the NSI interconnect is shared and
outside PCIe ownership; a faster link may therefore be throttled. No speed has
run on hardware.

PCIe Base Specification section 6.6.1 allows an endpoint up to 1 s after
PERST release to return a successful configuration completion. Until then,
the first vendor-ID read can return FFFFh (no successful completion) or 0001h
(a Configuration Request Retry Status completion when CRS Software Visibility
is enabled). Because the link has trained, a device is present, so the port
treats either value as not ready and reads again on later polls until 1 s after
release. After that window either value retains the port, and a vendor ID of
zero retains it at once. The operation deadline still applies: the session's
link timeout must cover the readiness window, or a slow endpoint retains the
port at the deadline. The port does not enable CRS Software Visibility, because
the native write filter refuses Root Control writes.

Absolute operation and lifetime deadlines use the common monotonic clock.
One poll writes the 91-entry COMB1 PHY sequence under a single register-mutex
hold and returns. Later polls, at least 100 µs apart, read the PHY PLL lock bit
(bit 0 of 0x06c02900). The first later poll that observes PLL lock writes the
four finishing entries in the same mutex hold. Each access keeps its own
deadline check. Link observation and endpoint readiness advance through
repeated bounded polls.
A deadline bounds software progress; it cannot bound a hung physical MMIO
transaction. Qualified independent recovery is required before physical use.
An uncertain access, late result, resource fault or failed invariant retains
leases and any held register mutex. There is no automatic rollback, retry reset,
resource release or memory reclamation.

## One client claims the port for its lifetime

After READY, only one client can claim the port: the endpoint test client or
one general function client. The claim repeats the cold function-zero checks
under the register mutex. From then on the default poll, identity and
configuration-read calls refuse, and every register access belongs to the
client's locked sessions. The native write filter applies the claimed
client's exact value rules to every later write, including the core's own
region-zero and stop writes.

## The function client configures one cold endpoint function

The general function client, `a733_pcie_function.c`, names one function of
the direct endpoint by vendor, device and 24-bit class code. It is not a PCI
bus framework: it serves one trusted function behind the x1 port.

- **Scan.** Function 0 is read first; functions 1 to 7 are read only when
  function 0 reports a multifunction header. Every present function must have
  a type-0 header, memory, I/O and bus mastering clear, MSI and MSI-X disabled,
  and power state D0 with PME disabled. A type-1 header refuses the scan,
  because a bridge or switch needs CFG1 routing and bus-number assignment that
  the port does not implement. Only the claimed function is configured.
- **BARs.** Every BAR of the claimed function must start unassigned. Memory
  BARs are sized by writing all ones with decode off, as the pinned U-Boot
  `dm_pciauto_setup_device()` does. They are placed largest first at natural
  alignment from 0x22000000. A 64-bit BAR receives an upper half of zero and
  must be smaller than 4 GiB. A BAR larger than the 96 MiB aperture, or a
  placement beyond it, refuses the function. I/O BARs are recorded and never
  written, and I/O decode stays off.
- **Window.** Region one covers the assigned BARs, rounded up to 1 MiB, which
  is the granularity of the root port's memory base and limit register at
  DBI+0x20. The client programs that register to the same range, sets root
  memory decode and bus mastering (DBI+4 = 0x106), and then sets the
  function's memory decode. Function bus mastering stays clear.
- **Interrupts.** In polling mode the function's INTx disable bit stays set
  and MSI-X stays disabled. In INTx mode the function's interrupt pin selects
  one of the four SII assert bits at APP+0xe00 (mask) and APP+0xe08
  (write-one-to-clear status), which the board kernel demultiplexes from
  INTID 184. The client unmasks only that bit and refuses any other INTx
  assert. In MSI-X mode the client reserves one pool page as the DesignWare
  MSI target (DBI+0x820) and enables one to four vectors at DBI+0x828. It
  enables MSI-X with the whole function masked, writes each table entry
  masked, unmasks it, and finally unmasks the function. Every entry beyond
  the configured vectors must remain masked. A vector bit outside the
  configured set faults the port. An interrupt never establishes completion.
- **Driver sessions.** A driver reads and writes 32-bit words inside an
  assigned BAR only between `a733_pcie_function_begin()` and
  `a733_pcie_function_end()`. The session start checks the command register
  and clears the configured MSI-X or INTx status bits. Each BAR write must pass
  the driver's exact write rule. MSI-X table and PBA writes come only from the
  client. The driver enables bus mastering after it owns every DMA target and
  clears it only after proving the endpoint quiescent.
- **Power states.** With bus mastering clear, a suspend request writes D3hot
  to the PCI power-management control register. The client then waits 10 ms
  before the next access, as the PCI power-management specification requires.
  The link may then enter L1. Resume writes D0 and waits another 10 ms. When
  the function does not report No_Soft_Reset, it has lost its configuration,
  so the client rewrites the BARs, the window, decode and the MSI-X entries.
  BAR access is refused while the function is in D3hot. ASPM stays disabled,
  because CLKREQ handling on PD20 and L1 substates are not implemented.
- **Stop.** With bus mastering clear, the client disables MSI-X or masks its
  INTx bit, clears memory decode, closes the root memory window, disables
  region one and clears root bus mastering. It then marks the port's client
  as stopped.

The native write filter enforces the same rules. Configuration writes reach
only the function that region zero currently targets, and only with the
recorded command, BAR, MSI-X control and power-state values. BAR sizing
writes are accepted only during the BAR stage. Any refusal or failed
readback retains the port, the MSI target page and every resource, with no
rollback. A late fault therefore leaves memory decode set as it was.

## The NVMe driver reads one namespace without write commands

`a733_pcie_nvme.c` is a polled driver for one NVMe controller (class
0x010802), adapted from the pinned U-Boot `drivers/nvme/nvme.c`. It owns six
4 KiB pages from the device memory pool: the admin submission and completion
queues, the I/O submission and completion queues, an identify buffer and a
data buffer. Both queue pairs are two entries deep, so at most one command is
outstanding.

The controller must start disabled: CC and CSTS must both read zero. CAP must
report the NVM command set, a 4 KiB minimum page size, a nonzero ready
timeout and doorbells inside BAR0. The driver then programs the admin queue
registers and moves its memory to device ownership. Next it enables bus
mastering and sets CC.EN, and waits for CSTS.RDY within CAP.TO. A controller
fatal status at any point retains the port. The driver identifies the
controller and the configured namespace. The namespace format must use
512 to 4096-byte blocks without metadata. The driver then requests one I/O
queue pair and creates the I/O completion and submission queues.

A read request names one starting block. The request covers 4096 bytes, must
lie inside the namespace and is copied out only after completion. Completion is
the phase-tagged completion entry with the expected command identifier,
queue, submission head and zero status. Between the phase observation and the
entry and data reads, the driver reads CSTS through the native path. The
barriers around that read order the later memory reads, and the read also
detects a controller fatal status. Each command has a deadline; a missing
completion retains the queues, the data memory and bus mastering.

The driver builds the single command that its current stage permits. The
BAR0 write rule admits a submission doorbell only when the queued entry equals
that command word for word. No write, format, firmware or vendor command can
therefore be submitted, even by a corrupted entry. Controller configuration
writes are limited to the admin queue addresses and two CC values.

Shutdown deletes the I/O submission and completion queues and requests a
normal controller shutdown. When CSTS reports shutdown complete, the driver
clears bus mastering, stops the function and stops the port. The queue and
data memory stays retained for the rest of the boot.

## An orderly stop holds the endpoint in PERST

`a733_pcie_stop()` closes the port from READY when no client has claimed it,
or after the function client's stop. The endpoint test client has no
quiescence proof, so its claimed port cannot stop. The stop first disables
outbound region zero, requires region one disabled and function zero's
decode and bus mastering clear, and clears LTSSM enable. The board kernel's
suspend path also clears LTSSM enable before PHY and power teardown. The stop
then drives PD22 low to hold the endpoint in PERST and requires both link-up
bits to fall within the caller's deadline. A link that stays up retains the
port. Every lease stays held after the stop, because no reviewed reverse
sequence exists for the PHY, clock, PCK or rail state. A stopped port cannot
restart within the same boot.

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
executed primary-source helpers, checks PMIC decode boundaries and exercises
inherited-state, timing, deadline and retained-fault cases. Hardware responses
and mutex scheduling are modeled. The reviewed batch passed with address and
undefined-behavior sanitizers. The PCIe runtime, shared PMIC provider and changed
pin-manager unit compiled for AArch64. Exact inputs, executables and results are
retained in `~/.cache/sunstoneos/evidence/pcie-20261016/batch-3/results.json`.
The unchanged source-helper execution was reused from its preserved first
batch. Two initial fixture failures, caused by an incorrect GPIO-bank address,
remain archived; the PCIe runtime did not change during validation. No physical
PCIe result is claimed.

`checks/a733-pcie-trace.py` builds the core and endpoint-test host models with
address and undefined-behavior sanitizers and runs every case of both. The core
model includes a directed change to 5 GT/s, an endpoint that stays at 2.5 GT/s,
a negotiated speed above the target, a change that never completes, a later
retrain and outbound-region write permissions. The script also prints every
register write of the nominal Gen1 core and endpoint-test flows. Given an
earlier revision with `--baseline-rev`, it builds the same trace driver against
that revision and requires identical write sequences. Controller and endpoint
responses, including the speed change, are modeled.

The same script builds `checks/a733-pcie-nvme.c` with the function client and
NVMe driver and runs its 24 cases. The model supplies an NVMe controller with
a 16 KiB 64-bit BAR0, a 4 KiB BAR2, an I/O BAR and an eight-entry MSI-X table.
It also models the DesignWare MSI and SII latches, PCI power-management resets
and link loss after PERST.

- **Nominal.** Polling, INTx, MSI-X, and MSI-X at 5 GT/s each identify the
  controller and read three 4096-byte ranges against a fixed pattern. Each then shuts
  down to PERST, with memory, decode, windows and the MSI target checked
  afterwards.
- **NVMe refusals.** An enabled inherited controller, a fatal status during
  enable, an identify error and a missing completion each retain the port. So
  do an unconfigured MSI-X vector, an INTB assert and a shutdown that never
  completes. A forged write command and an out-of-range read are refused
  before submission.
- **Function cases.** D3hot and resume run both with and without an internal
  reset. A multifunction scan is checked, as are refusals for a bridge, a
  sibling with decode set, a 128 MiB BAR, a preassigned BAR, an MSI-X table
  outside its BAR and a class mismatch.
- **Port stop.** A configuration-only stop succeeds, the endpoint test client
  cannot stop the port, and a link that stays up retains it.

Every modeled DMA requires function bus mastering and device-owned memory,
and every write passes the native filter's decisions. The endpoint, its
controller and interrupt delivery are software models; no NVMe device or
function power transition has run on hardware.
