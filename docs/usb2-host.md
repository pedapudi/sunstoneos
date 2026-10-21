# USB host storage reads and copied HID interrupt input

The USB2 host implementation connects the A733 DWC3/xHCI controller to TinyUSB
and copied read-only storage and HID input APIs. It supports one directly attached
high-speed mass-storage device or one low-, full- or high-speed HID interface,
directly or behind one admitted high-speed USB2 hub. A separate supervised
profile adds one SuperSpeed Gen1 Bulk-Only Transport (BOT) mass-storage device
or boot keyboard on TYPEC24, directly or through one self-powered Gen1 hub.
It also supports the USB2 companion root through the same COMB0 preparation. The USB2 profiles keep their existing admission and wire
contracts.
Three further USB2 modes are implemented as admission options, which a session
selects before the first poll. They admit a second tier of high-speed hubs,
bus-powered hubs within the USB 2.0 current budget, and capture from one USB
Audio Class 1 microphone over an isochronous IN stream. No board workload selects them yet;
see [Two hub tiers](#two-usb2-hub-tiers-share-the-three-device-records),
[Bus-powered hubs](#bus-powered-hubs-stay-within-the-upstream-current-budget)
and [Audio capture](#an-isochronous-in-stream-carries-usb-audio-class-1-capture).
The storage profile uses one configuration, interface 0, alternate 0, two bulk
endpoints and LUN 0. Storage configuration descriptors must contain the standard
32-byte configuration/interface/endpoint sequence for USB2, or the 44-byte
sequence with two immediate SuperSpeed endpoint companions for Gen1. The direct
Gen1 keyboard uses one 40-byte configuration with an immediate interrupt
endpoint companion. Blocks
must be 512 bytes.
The composed software check passes with the actual TinyUSB stack, xHCI core and
shared resource providers. Physical operation, connector power and independent
recovery remain unqualified.

The V1.2 board schematic connects TYPEC24 to the independent USB2 PHY and the
COMB0 SuperSpeed lanes. USB1 data pads are unconnected on sheet 10. TYPEC16 is the
fixed-sink power-input connector used by USB0. Its fixed CC resistors and direct
board-power connection do not provide a switchable host source.

## Native startup holds actual shared resources

`a733_usb2_native_init` reserves 128 KiB from the boot-owned uncached pool before
concurrency. It validates every memory page and the complete Device register
apertures. Normal non-cacheable memory may have outer or inner shareability,
matching the existing pool contract. The parent remains permanently DEVICE-owned;
a private child ledger records controller ownership and terminal retention.
Runtime failure never edits the shared allocator without its serialization owner.

The constructor validates the entire native object against the clock, rail,
PMIC, power-domain and Type-C providers before writing private state. Providers
and their private backing remain at fixed addresses. Their native adapters must
share the actual register mutex, counter and resource managers supplied by boot.
One designated FreeRTOS task owns the host stack and all application calls.
Three typed device records keep independent input/output contexts, EP0 stages,
address metadata and endpoint rings. One command ring and event ring remain
controller-owned. Seven endpoint ring/bounce page pairs, three context pairs and
up to eight scratch pages occupy the complete retained 128 KiB reservation.

The platform acquires exclusive USB2 ownership, bit 66 in the shared clock
registry. It also holds OSC, REF, PERI0, FABRIC, SERDES_COMMON and
USB_PCIE_DOMAIN against changes. The scope covers DWC3/xHCI at 0x06a00000,
the independent PHY at 0x06b00000, INTID 187, PCK8 and USB2-local clock/reset
fields. SERDES+8 ownership is limited to bits 21, 20, 17, 16 and 4. No COMB1 or
PCIe analog controls are changed.

The shared PMIC provider reads DCDC1, DC1SW2, DCDC9, ELDO6 and CLDO5 under the
same retained rail lease. Programmed values must match immutable board-qualified
inputs. The snapshots establish register settings; they do not measure voltage,
settling or physical wiring. Cold rail programming remains separate work.

The first profile adopts verified PCK7/PCK8 static-ON state, common SERDES at 100 MHz
and the configured fabric clocks. It proves the selected PERI0 branches and
excludes automatic gating before local programming. SYS24M supplies reference,
microframe and suspend clocks. PERI0 at 480 MHz divided by 4 supplies replacement
PIPE. Cold shared-domain startup remains required for a broader boot profile.

Local reset and gates must initially be disabled under authenticated cold-master
ownership. The provider enables only the source-defined USB2 fields and applies
the board PHY tuning value. It observes DWC_usb31 identity after enabling the
bus, checks the admitted revision, and configures UTMI8, turnaround 9, host mode
and the published 24 MHz reference adjustment. The native adapter limits writes
to these addresses and fields. Admission requires INTID 187 disabled, inactive
and clear. The default profile keeps it masked and polls completion events.
The optional supervised board interrupt profile below configures and enables
only this owned source.

## Type-C power remains independently supervised

The actual Type-C provider owns FUSB302, source-role classification and PL8 VBUS
control. The host pins one provider identity, attachment generation and polarity.
Every controller operation requires a fresh observation from that provider.
A busy publication permits no new MMIO, DMA publication or caller-output copy.
A busy observation is momentary metadata contention, such as another core
holding the PMIC object, a lease registry or the Type-C publication lock. The
xHCI core and the USB2 platform repeat it without any access until it succeeds,
is refused, lasts 2 ms of counter time or reaches the operation deadline. Only
success lets the access proceed. The other outcomes fault the session, except
for the USB2 platform case described next. A poll that observes BUSY before its
first access still returns BUSY.

The PMIC poll releases its object while it waits for REGISTER_LOCK or the I2C
mutex ([PMIC runtime](pmic-runtime.md)). A PMIC BUSY that lasts 2 ms therefore
means a stalled holder. The USB2 platform holds REGISTER_LOCK for its register
phases and observes the providers before the first register access of each
phase. If that first observation stays BUSY for 2 ms, the platform releases
REGISTER_LOCK and returns PENDING; a later poll repeats the phase, and the
startup deadline bounds the repetitions. A BUSY that lasts 2 ms after a
register access faults the session. Like any platform fault after the lock is
taken, that fault retains REGISTER_LOCK. The host check covers both platform
cases and a BUSY that never clears inside an xHCI access; each ends after about
2 ms.
Type-C attachment loss permanently revokes the host session; a new Type-C
attachment generation is not adopted.

Type-C service and its independent power task must continue when the USB worker
fails. xHCI transfer accesses hold no shared register mutex, so a stalled USB
transfer worker does not itself prevent GPIO shutdown. An uncertain platform
access can retain the common register mutex. That case requires independently
qualified watchdog/interconnect recovery; software cannot guarantee a later
GPIO write through an unavailable shared path.

## Completion governs every returned byte

The xHCI core checks capabilities, context size, scratchpad count and the USB2
protocol port before publishing pointers. It refuses an inherited running or
faulted controller. It issues reset only after observing the admitted cold
controller halted. Private command, event and endpoint rings use ordered
non-cacheable memory and explicit cycle ownership.

Enumeration uses actual Enable Slot and Address Device completions. The initial
Address Device operation preserves default address 0; SET_ADDRESS performs the
actual address command and maps the stack's logical address to the hardware
slot. Retiring TinyUSB's address 0 metadata preserves that same assigned slot.
Normal control Setup, Data and Status stages each use their source-defined
transfer-descriptor completion. Opening endpoints does not make them usable
until Configure Endpoint completes. Actual hardware slot IDs are independent
of TinyUSB logical addresses; completion matching checks both the slot and its
owned pending TRB. Retiring address 0 applies separately to each device.

The host controller driver (HCD) validates descriptor length, type and structure before passing a data
completion to TinyUSB. Device and configuration probes must agree with their
full responses. Strings use bounded probe/full lengths. GET_MAX_LUN must return
one actual byte and LUN 0. This guard prevents short or changed configuration
descriptors from reaching the pinned stack's less restrictive parser.

Only the read-only SCSI commands required by enumeration and READ10 are admitted.
Bulk command wrappers and status wrappers have separate ownership. Before the
class callback runs, the HCD checks actual transfer length, status signature,
matching tag, data residue and command status. `a733_usb2_read_result` copies
one sector only after successful data and status completion, within the caller's
absolute deadline and under a fresh attachment observation.

Abort, malformed completion and expiry revoke caller destinations and retain
all private DMA backing. Reset, power-off and a halted status are not claimed
to block and drain all external transactions. A reported disconnect instead
ends the device through the orderly teardown described in
[Detach disables the slot before its pages are reused](#detach-disables-the-slot-before-its-pages-are-reused):
the outstanding read returns -1 without copying a sector, and a later
attachment can mount and read again. The application API has no release or
retry call. A failed command is not reported as a completed read.

The TinyUSB stack uses its single-context queue implementation inside the actual
FreeRTOS owner task. No interrupt or second task may call the stack. Each host
poll advances at most one xHCI event/state and two TinyUSB queue iterations.
In the default polled profile, the board owner polls again without sleeping
while setup or a transfer is outstanding, up to 32 polls per tick, the
interrupt profile's batch size. The repeated polls stop after four consecutive
polls without progress. Progress means a changed platform phase or supply
index, core phase, consumed event, mount state or request state. A HID transfer
that waits for input therefore costs one poll per tick.
All control calls are asynchronous. The optional interrupt handler masks its
source and notifies this same task; it never calls TinyUSB. The native image
must schedule this task within operation deadlines and keep the independent Type-C tasks runnable.

## Interrupt input uses the real HID class and native application

The existing host owner task also consumes copied HID input. It does not send
keyboard or mouse events into the desktop. The pinned TinyUSB HID class parses
the admitted configuration, opens its interrupt endpoint, sends SET_IDLE and,
for boot interfaces, SET_PROTOCOL, and retrieves the report descriptor. The
application retains an owned descriptor copy. Report contents remain raw bytes;
no keyboard, mouse or generic report-descriptor interpretation is claimed.

The admitted HID layout is one configuration, interface 0, alternate 0, one
9-byte HID descriptor and one interrupt-IN endpoint: 34 configuration bytes.
The HID descriptor declares one report descriptor of 1–256 bytes. Interfaces
may use generic report protocol or boot keyboard/mouse interface protocols.
Descriptor probe and full responses must agree before TinyUSB sees them.
Additional interfaces, optional OUT endpoints and richer descriptor layouts
remain unsupported in this profile.

The observed root-port speed is retained in the xHCI Slot Context and checked
throughout the lifetime. Low-speed EP0 requires 8-byte packets; full-speed EP0
accepts 8, 16, 32 or 64 bytes; high-speed EP0 uses 64 bytes. A full-speed packet
change waits for a matching Evaluate Context command completion and output
context readback before the next control transfer reaches its ring.

Interrupt IN accepts one transaction per service interval, with at most 8 bytes
at low speed or 64 bytes at full/high speed. Reserved packet bits and
high-bandwidth encodings are refused. The Endpoint Context uses type 7, the
selected packet limit as Max ESIT Payload, and the source-defined interval:

- High speed: descriptor `bInterval` is 1–16; the context value is `bInterval-1`.
- Full/low speed: descriptor `bInterval` is 1–255 milliseconds; the context value
  is `floor(log2(bInterval*8))`.

These encodings follow Intel xHCI 1.2 section 6.2.3.6 and Table 6-12, including
its required downward rounding for full/low-speed interrupt endpoints. The xHCI
controller schedules periodic bus transactions. CPU completion handling
uses the optional CPU interrupt completion profile below or the existing
polling profile. Physical INTID 187 delivery remains unqualified.

`a733_usb2_hid_receive` queues one report under the owner's absolute deadline.
An idle HID endpoint may NAK until that deadline without inheriting the shorter
enumeration timeout. Caller storage never reaches the controller. The existing
private non-cacheable bounce buffer receives DMA, then the core copies only
completed bytes into TinyUSB-owned storage. Slot, endpoint, TRB pointer,
residual, completion status, attachment and deadline checks precede class
notification. The pinned HID transfer callback ignores its result argument, so
the HCD must suppress failed events before calling it.

`a733_usb2_hid_result` copies an owned report only after a fresh live attachment
check. It returns the actual byte count, monotonic sequence, Type-C attachment
generation, logical device address, speed, interface protocol and endpoint.
Short transfers and zero-length packets retain their actual lengths. They are
raw transfer records and do not imply valid boot keyboard or mouse semantics.
One consumed result permits another receive. Failure, expiry or active
cancellation permanently closes admission and retains all DMA backing. A
reported disconnect ends the outstanding receive without a report; a later
attachment can mount and receive again, and report sequence numbers keep
increasing across attachments.
A report transfer that ends with USB Transaction Error delivers no callback;
the session is retained at most 1 s later, as described in
[Detach disables the slot before its pages are reused](#detach-disables-the-slot-before-its-pages-are-reused).
The application has no endpoint-stall recovery path.

The native board task preserves its storage behavior and selects HID by the
actual enumerated class. It collects four reports into an immutable 512-byte
batch before publishing success. It then continues observing attachment,
controller faults and lifetime without changing the published bytes. The board
wire session remains unchanged; internal native layouts include new typed
periodic/HID metadata and require recompiling their embedding consumers.
Earlier frozen images remain tied to their original compiled layouts.

## One external hub connects one downstream application device

The native path links the pinned TinyUSB hub driver. It admits one directly
attached high-speed USB2 hub with one to seven ports and one downstream device:
a low-, full- or high-speed HID interface, or the existing high-speed MSC profile.
The single application device and hub use separate TinyUSB address ranges:
with `CFG_TUH_DEVICE_MAX=2` and `CFG_TUH_HUB=2`, leaves use logical addresses 1
and 2 and hubs use addresses 3 and 4; a single hub receives address 3. The original frozen single-leaf image used
`CFG_TUH_DEVICE_MAX=1` and hub address 2. Physical xHCI slots are bound from actual Enable Slot
completions and are never inferred from those logical addresses.

A high-speed hub uses a transaction translator (TT) for low/full-speed traffic.
Hub device descriptors must declare the single-TT or multi-TT high-speed
protocol. Configuration descriptors must contain one interface and one interrupt
IN endpoint: 25 bytes for single-TT, or 41 bytes for the canonical two-alternate
multi-TT layout. Both alternates must declare the same endpoint, packet size and
interval. The nine-byte class hub descriptor declares one to seven ports, bounded
power/overcurrent modes and the source-defined transaction-translator think time.
The port limit matches the pinned driver's one-byte status-change transfer.
A hub below this hub closes the entire session unless the session admits two
hub tiers. Richer configurations and additional hub interfaces close the
entire session. Existing single-result sessions still refuse another connected
leaf. The separate combined workload below admits two distinct leaf classes.

The hub must declare self power, report current self-powered status through the
standard device GET_STATUS, and report a present local supply with no overcurrent
through class hub GET_STATUS. A session that admits bus-powered hubs applies the
rules in [Bus-powered hubs](#bus-powered-hubs-stay-within-the-upstream-current-budget)
instead.
These descriptor/status observations do not measure electrical power or establish
that a particular accessory is safe. Physical admission still needs a suitable
self-powered hub, its supply and the reviewed board/Type-C source path.

A narrow patch to the actual pinned hub class sets the hub-descriptor request's
complete type/index value to `0x2900` and inserts both initial power-status reads.
The hub class still owns port power, reset, change-bit requests, status parsing
and child enumeration. The adapter validates every response before its callback.
It publishes the first hub status TD only after the final successful port-power
status stage plus `bPwrOn2PwrGood` in two-millisecond units. The existing owner
clock and absolute deadline govern that delay; no additional timer service exists.

The hub's first Configure Endpoint command sets Hub, Number of Ports, TT Think
Time and Multi-TT fields and verifies their completed output context. The actual
SET_INTERFACE status must complete before multi-TT metadata is admitted. The
child uses TinyUSB's reset-derived bus information to bind the selected hub,
port and observed speed. Its Address Device context contains that route string,
root port and, for low/full speed, the parent Hub Slot ID, TT port and active
Multi-TT selection. No child is enabled before the hub context completion.
These fields follow xHCI 1.2 sections 4.5.2 and 6.2.2.

Descriptor callbacks queue copied hub metadata only. The owner applies it after
the core callback returns and before TinyUSB consumes the queued completion.
This preserves the core's prohibition on reentrant API calls. TinyUSB's existing
control queue serializes hub and leaf requests; each physical device retains
its own descriptor probes and configuration prefix.

Hub interrupt status uses private bounce DMA and a validated one-byte copied
completion. Port status binds the first connected port until that port reports
a disconnect. A port status with connection clear, the port disabled and the
connection-change bit set removes the child: TinyUSB closes it and the adapter
starts the core's child detach, while the hub and its status transfer stay in
service. A later connection on any port can then bind a new child. A second
connected port, a disconnect that leaves the port enabled or reports no
connection change, suspend, overcurrent, failed transfers, an unexpected
slot/TRB or malformed status prevents further callbacks and caller-output copy.
Both slots and the whole DMA parent remain retained. A hub status transfer that
ends with USB Transaction Error or Split Transaction Error is held without a
callback for at most 1 s before that retention. The hub status transfer carries
the session lifetime as its deadline, so this hold limit applies to it. Type-C
VBUS shutdown does not prove that a self-powered hub or its downstream ports
lost external power.

The application continues to return one copied sector or four raw HID reports.
Its wire/session layout and host decoding are unchanged. HID metadata identifies
the actual leaf address, speed, interface and endpoint. The frame alone does not
identify a parent hub; parent topology requires separate controller or qualification evidence.

Focused sanitizer checks use actual TinyUSB hub/HID/MSC parsing and callbacks,
the xHCI rings and the existing shared providers. They cover downstream MSC,
all three HID speeds, multi-TT low speed, port 7, power/descriptor/context
refusals, detach, another leaf, overcurrent, stall, nested hubs and wrong-slot
completion. Direct-device compatibility and the existing interrupt profile use
selected affected cases. The native supervised IRQ image links every changed
embedding. Linked owner and copied host-capture checks use that same image;
source/hardware/kernel boundaries remain explicit. Evidence is retained under
`~/.cache/sunstoneos/evidence/usb-host-hub-20261019/`.

## CPU interrupts wake the existing host owner

The supervised board build can select `SUNSTONE_BOARD_USB2_IRQ=ON`.
Its USB owner task is pinned to CPU0. The pinned A733 device tree routes
xHCI2 at 0x06a00000 through SPI 155, level high: architectural INTID 187.
Private EL3 boot code checks the authenticated workload and inherited interrupt
state, then installs Group1NS, level-high, CPU0 routing and the API-safe priority.
Non-host workloads do not configure this source. The dispatcher retains
acknowledge, EOI and deactivation; only CPU0 dispatches INTID 187 to the handler.

The handler uses the existing kernel ISR critical section to mask the GIC source,
publish a pending notification and wake the owner task. It accesses no xHCI
registers, DMA storage, Type-C provider or TinyUSB state. The owner waits for GIC
mask propagation and deactivation before claiming the matching notification.
Claim and mask checks share one critical section, so a later interrupt remains
pending for a subsequent checked service entry.

The task enables xHCI global interrupt generation and primary-interrupter IE only
after the controller reaches its active state. It owns any startup event backlog
before first GIC rearm. Each service clears USBSTS.EINT before IMAN.IP, with
readback, following xHCI 1.2 sections 4.17, 5.4.2 and 5.5.2 and the pinned Linux
`xhci_irq` ordering. All event TRBs retain their existing cycle, pointer, slot,
endpoint, residual and generation checks. Unknown events, including event-ring
full or lost-event errors, close admission and retain backing.

Service consumes one event per core poll. Intermediate ERDP updates free space
after sixteen events when more remain; the final advancement clears EHB only
after observing an empty ring. This avoids consecutive unchanged ERDP writes.
A post-release ring and IP check captures an event arriving during rearm;
continued service acknowledges a newly asserted EINT/IP before further drain.
The task rearms the GIC only after the empty check, inside a critical section
that excludes a newly published notification. RWP, deactivation and rearm waits
have a fixed retry limit. Every failure stops the binding with its GIC source
masked and retains the controller, contexts, rings and copied backing. Masking
is not evidence of stopped or drained external transactions.

The owner uses FreeRTOS task notifications with a one-tick safety wake. TinyUSB
timers, queued commands and TD submission still progress without another IRQ;
only event consumption is gated by the owned interrupt service. Batches yield
after thirty-two owner iterations, and independent Type-C power tasks remain
runnable. Published HID/storage payloads and their host transport are unchanged.
Derived applications refuse the optional IRQ configure flag and continue using
their separate polling builds.

Focused sanitizer checks run actual xHCI/TinyUSB MSC and all three admitted HID
speeds with modeled IRQ status, EINT/IP acknowledgement, EHB-release races,
queued-event bursts, event-consumption exclusion and retained overflow/failure.
The actual application binding check covers mask propagation, inactive waits,
notification claims and rearm exclusion. Linked instruction checks cover its
handler and GIC mask/rearm code, with kernel boundaries and hardware effects
modeled. Copied host capture reuses the accepted report-object producer and runs
the current native board consumer and serializer. Evidence is retained under
`~/.cache/sunstoneos/evidence/usb-host-cpu-irq-20261019/`.
Physical GIC delivery, interrupt latency, DMA ordering and electrical behavior
remain unqualified. No hardware or desktop input access is part of these checks.

## One HID and one mass-storage device operate together

Supervised board workload 4 admits one self-powered high-speed hub with exactly
one HID leaf and one high-speed MSC leaf. HID may operate at low, full or high
speed. Both enumeration orders are supported. Existing workloads 0–3 keep their
single-result behavior. The board target alone defines `SUNSTONE_BOARD_USB2_DUAL`;
derived applications retain their previous admission limit. The session remains
592 bytes with unchanged offsets and reserved fields. Workload 4 omits radio and
requires the image manifest's `usb2_combined` capability.

Each child permanently binds its actual slot, logical address, hub port and
observed speed. Default-address enumeration remains serialized by TinyUSB's
control queue. A second HID, second MSC, third leaf, removal, bad slot or failed
completion revokes the whole session. Enumeration does not reuse a retired slot
or parent memory. One owner runs the actual TinyUSB hub, HID and MSC classes;
HID and BOT retain separate pending buffers, deadlines and completion credits.

The fixed DMA layout uses 4096-byte pages. Offsets 0–16383 contain DCBAA,
command/event rings, ERST and scratch pointers. Three input/output context pairs
occupy offsets 16384–40959. Seven ring pages occupy 40960–69631 and seven bounce
pages occupy 69632–98303. Eight scratch pages occupy 98304–131071. An input
context requires at most 33 × 64 = 2112 bytes; an output context requires at most
2048 bytes. Each device initially owns EP0 and one data credit. Only the sole MSC
second bulk endpoint may permanently claim the seventh pair. This also preserves
direct MSC support without moving any published pointer.

The native client issues one read-only LUN0/LBA0 sector request and four raw HID
input requests. It copies completed bytes into separate private extents and
publishes success once both are complete and the attachment authority is fresh.
A partial result never publishes success. Published backing is immutable; the
owner continues monitoring faults and the finite session deadline.

Combined success uses outer board frame version 3 with the unchanged 72-byte
header and a 1088-byte payload. Pending/failure frames retain version 1 without
payload. Versions 1 and 2 retain their existing storage and HID meanings. The
payload starts with a 64-byte little-endian header:

| Offset | Field |
| --- | --- |
| 0–15 | `0x4c415544`, version 1, total bytes 1088, leaf count 2; four u32 values |
| 16–23 | Type-C attachment generation, u64 |
| 24–28 | MSC address, speed ID 3, interface 0, bulk IN endpoint, bulk OUT endpoint |
| 29–31 | Zero |
| 32–51 | LBA 0, sector offset 64, sector length 512, HID offset 576, HID length 512; five u32 values |
| 52–63 | Zero |
| 64–575 | Copied sector |
| 576–1087 | Existing HIDR four-report envelope, including HID identity |

The host checks the entire layout before slicing either result. The MSC and HID
addresses must differ and both payloads must share the attachment generation.
Capture saves both `sector.bin` and `hid-input.json`; no desktop input events are
produced. Numerical data correctness and physical power, DMA, interrupt and hub
operation remain unqualified. Neither board has populated eMMC or UFS.

Focused evidence is under
`~/.cache/sunstoneos/evidence/usb-host-dual-leaf-20261019/`. It covers both
leaf discovery orders, HID speeds, separate credits, duplicate class refusal,
wrong-slot/stall/revocation failure, IRQ completion and the actual linked native
client, serializer and host decoder. Kernel/provider/electrical boundaries remain
modeled. Earlier frozen images remain authoritative for their original source
and layout; this image does not replace their evidence.

## HID capture has an explicit transport discriminator

The existing USB0 transport retains its 72-byte header and 512-byte successful
payload extent. Successful HID results use outer frame version 2. Version 1
always identifies storage bytes, even if a sector happens to resemble a HID
batch. Pending and failed frames retain version 1. The serializer selects HID
from the immutable native device kind after acquiring the published result.
The shared USB0 provider is unchanged.

The HID payload starts with little-endian magic `0x52444948`, version 1 and count
4, followed by the logical device address and 64-bit Type-C attachment
generation. Bytes 24–27 contain speed ID, interface number, interface protocol
and endpoint; bytes 28–31 are zero. Speed IDs are 1 full, 2 low and 3 high.

Four 80-byte records begin at offset 32. Each contains a 64-bit sequence,
32-bit actual byte count, four reserved zero bytes and a 64-byte report area.
Unused report tails and bytes 352–511 are zero. Interface protocol values are
0 generic, 1 keyboard and 2 mouse; these identify the interface rather than
asserting report semantics.

`apps/board/observe.py` checks the explicit kind, identity, sequences, extents
and reserved fields. `tools/check-a733-board-supervised.py` saves HID capture as
`hid-input.json` and never reports it as a storage read. The original raw frames
remain durable evidence. The separate `--sector-output` decoder option refuses
a HID result; `--hid-output` writes its decoded records.

## One direct Gen1 storage device uses COMB0

Supervised workload 6 requires a SuperSpeed Gen1 BOT device; it cannot silently
publish a USB2 fallback. Its source capacity must be at least 900 mA, and its
configuration cannot request more than 900 mA even when the admitted source has
more capacity. The existing 592-byte session and 128 KiB DMA reservation remain
unchanged. The board target alone admits workload 6 through
`SUNSTONE_BOARD_USB3`; derived compositions retain their existing selectors.

The platform follows the selected A733 Cadence implementation. `cadence-source.py` verifies the pinned original source hash and
extracts the USB-only clock and parameter functions into the private build. It
preserves their license, values and halfword analog accesses. Native access
checks allow only the used analog indices and the selected COMB0 top, routing,
RTC and DWC3 registers. The adapter follows the actual normal/reverse Type-C
orientation sequence and measures reset/PLL waits after completed writes. It
uses the source PMA-ready check and bounded fallback; failure retains all
ownership. It does not add a forced-Gen1 quirk or speculative analog settings.

The owner holds the new exclusive COMB0 clock resource, stable shared SERDES and
PERI parents, and the actual DCDC2 800 mV core and CLDO5 1.8 V analog rails.
It adopts the documented common clock configuration without resetting or
retuning the shared domain used by COMB1 PCIe. COMB0 is USB-only in this profile;
DisplayPort and remote writers must be excluded for the whole retained lifetime.
No COMB1 resource or existing USB0 power-provider behavior changes.

The admitted device descriptor uses USB 3.0 revision encoding; later descriptor
revisions remain outside this bounded profile. The xHCI path selects the USB3
Supported Protocol root port, standard speed ID 4,
and that protocol's slot type. The USB2 companion's status changes are
acknowledged without enumeration. The Gen1 root must be enabled in U0; U1/U2
entry stays disabled. EP0 uses 512-byte packets; the device descriptor encodes that size as
exponent 9. Bulk endpoints use 1024-byte packets, with bMaxBurst from
each immediate companion descriptor. Stream capability is checked but streams
are disabled in the endpoint context. The existing owned rings, private bounce
copies, event completion and retained failure protocol apply to this path.

A private, hash-checked TinyUSB adaptation adds the SuperSpeed speed value,
EP0 geometry and companion traversal to the actual host and MSC class code.
The original source stays untouched. Enumeration reads the bounded BOS header
and complete BOS before configuration. USB 3.2 Revision 1.1 sections 9.1.2,
9.4.11 and 9.4.12 require SET_ISOCH_DELAY and SET_SEL even for BOT; both are
issued and must complete. The direct-root isochronous delay uses the normative
40 ns transmission delay. SET_SEL uses the larger of the BOS device and root
HCSPARAMS3 exit latencies, with the pinned Linux minimum of one. No hub latency
is inferred. These control values do not qualify actual link latency.

A successful workload 6 result uses outer wire version 4 and a 544-byte payload:
32 bytes of `SMSC` metadata followed by the copied 512-byte LBA 0 sector. The
metadata contains version, extent, LBA, attachment generation, device address,
Gen1 speed, interface and both bulk endpoint addresses. Publication follows the
existing successful READ10 data and CSW checks plus a fresh live generation
check. The host decoder validates every extent and identity field and saves the
sector through the existing capture path. Versions 1, 2 and 3 keep their storage,
HID and combined meanings. No storage write or desktop input operation is added.

Preparation additionally requires an actual
`a733-usb3-gen1-physical-entry-v1` record bound to the image, template, board and
port, with frozen evidence of the fitted TYPEC24 route, both COMB0 rails and
reference/common clocks. The evidence must also establish DP/remote-writer
exclusion, a reviewed passive Gen1 cable and BOT device, and source
switch/discharge/reverse blocking. The session source capacity may not exceed
the authenticated physical record. A modeled 900 mA
observation is never physical authority. That record is currently unavailable:
boards require physical recovery, and 900 mA sourcing and COMB0 electrical
operation remain unqualified. Source shutdown does not prove external device
power removal or DMA drain. Neither board has eMMC or UFS populated; this work
adds no probes for either absent device.

## GET_MAX_LUN stalls recover before the class callback

An admitted high-speed or Gen1 storage device may stall the exact one-byte
GET_MAX_LUN request on interface 0. The controller matches the transfer event
against the pending slot, EP0 and TRB, revokes the caller destination, and
preserves the original deadline. It copies no stalled payload. Other requests
and non-control endpoint stalls retain the session.

The owner polls two commands through the existing command/event rings: Reset
Endpoint with Transfer State Preserve cleared, then Set TR Dequeue Pointer.
Each completion must match its command pointer, slot and successful result.
The output endpoint context must move from Halted to Stopped; its final dequeue
pointer and cycle must match the next unpublished producer slot. Recovery does
not reset or reuse the ring allocation. It skips the failed request's remaining
stages and reserves that slot for the next SETUP. No endpoint doorbell is issued
between the recovery commands.

Only after both commands complete does the adapter discard its old control
buffer and publish one STALLED completion. The pinned TinyUSB control callback
then invokes its MSC fallback, selects LUN 0 and sends Test Unit Ready. Capacity
and copied READ10 use the existing read-only path. TinyUSB does not call
`hcd_edpt_clear_stall` for this EP0 failure; that hook continues to refuse generic
bulk recovery. A software timeout, stale event, failed command or lost generation
never authorizes a new transfer or resource reuse.

The sequence follows xHCI 1.2 sections 4.6.8, 4.6.10 and 4.10.2.1. The pinned
Linux `xhci-ring.c` handles a default-control stall with Reset Endpoint followed
by a dequeue update. Pinned TinyUSB `usbh.c` completes STALLED control requests
without its failed-transfer retry path; `msc_host.c` selects one LUN when
GET_MAX_LUN fails. These primary inputs are cached and pinned in
`sources.lock.json`; no stack source or model is replaced.

Focused software compositions execute the actual TinyUSB fallback, xHCI core
and copied READ10 path. They cover stalls at SETUP, DATA and STATUS, delayed
recovery commands, producer wrap and cycle, one interrupt-driven event path,
and retention for failed/stale commands, stale transfer events, deadline expiry,
wrong dequeue readback and generation loss. Device and register behavior is
modeled. Physical endpoint recovery and storage reads remain unqualified.

## Direct Gen1 keyboard reports use an explicit workload

Supervised workload 8 selects one directly attached Gen1 boot keyboard on
TYPEC24. It uses the existing HID descriptor, interrupt-IN ring, 64-byte report
buffer and copied caller API. It adds no hub, endpoint allocation or mutable
native layout. SuperSpeed hubs and USB2 children on a companion root remain
separate requirements. The Mimus keyboard consumer retains its existing USB2
profiles; workload 8 publishes raw reports through the board application.

The admitted configuration has interface 0, alternate 0, one boot-keyboard HID
interface and one interrupt-IN endpoint. The packet size is 1–64 bytes and
bInterval is 1–16. Its immediate six-byte SuperSpeed companion must specify
bMaxBurst zero, bmAttributes zero and wBytesPerInterval equal to the packet size.
These restrictions establish one transaction per service interval. The xHCI
Interval is bInterval minus one, Mult and Max Burst remain zero, and Max ESIT
Payload equals the admitted packet size. USB 3.2 section 9.6.6–7 and xHCI 1.2
sections 4.8.2.4 and 6.2.3.6 define these fields.

The private pinned TinyUSB adaptation includes the companion in the HID class
length and skips it before returning the consumed interface extent. Native
descriptor validation checks every companion field before endpoint opening.
BOS, SET_ISOCH_DELAY and SET_SEL use the existing Gen1 sequence. A stalled or
mismatched interrupt completion retains the controller and refuses caller data.

The 592-byte sealed board session remains unchanged. Workload 8 requires the
supervised USB3 image, host ownership, no radio profile and the existing 900 mA
source qualification floor. Workload 7 remains refused. Preparation checks a
32-byte immutable ELF capability record as well as the supplied manifest, so
asserting a manifest field cannot enable an older MSC-only image. The physical
record must explicitly name `hid-keyboard`, bind the image and session template,
and establish a compatible Gen1 cable and keyboard. Legacy MSC records preserve
their existing interpretation. No matching physical record is currently available.

The application requires HID class, Gen1 speed and keyboard protocol before
requesting reports. It copies four reports, checks lifetime and attachment
again, and publishes the existing HID wire version 2. Its speed field records
Gen1 only under workload 8; older workload decoders keep their USB2 limits.
The host saves the copied report batch as data and does not send desktop input.
High-speed fallback and generation loss before publication refuse success.

Focused software evidence is under
`~/.cache/sunstoneos/evidence/usb-ss-hid-20261020/`. Actual stack compositions
cover ordinary and 64-byte interrupt reports, strict companion/interval/protocol
refusals and uncertain completions. Linked application and host checks cover
copied publication, fallback and lifetime refusal, immutable image capability
and class-bound authority. Device, cache, RTOS and transport effects are modeled;
physical keyboard, power, DMA and interrupt operation remain unqualified.

## One Gen1 hub or the USB2 companion root serves one leaf

Supervised workloads 6 and 8 admit the existing Gen1 MSC or boot-keyboard leaf
through one self-powered Gen1 hub. Workloads 10 and 12 select a USB2 boot
keyboard or high-speed MSC device on the USB2 companion root. That device may
attach directly or through the existing bounded high-speed USB2 hub path.
Only the selected root enumerates devices. A dual-bus hub exposes separate
USB2 and SuperSpeed devices; its USB2 children belong to the companion root.
The SuperSpeed hub provides no USB2 transaction translator.

The sealed session remains 592 bytes. Its 32-bit field at offset 404 is
`usb_topology`: zero selects direct attachment and one requires a single hub.
Only workloads 6, 8, 10 and 12 may select one. Older workloads require zero
and retain their existing topology behavior. The native root descriptor gate
refuses a hub under direct authority or a leaf under hub authority before
configuration. Workloads 9 and 11 remain refused.

The Gen1 hub configuration is 31 bytes: one interface, one interrupt endpoint
and its immediate six-byte companion. The class descriptor is exactly 12 bytes
with type `0x2a`, one to seven ports, header-decode latency at most 1 microsecond
and hub forwarding delay at most 400 nanoseconds. Hub configuration completes
before a child receives a slot. Set Hub Depth selects zero for the root hub.
Each downstream port receives explicit zero U1/U2 timeout settings before power.
The existing power-good deadline governs the first status transfer.

Every device retains its own BOS, enumeration request and readiness state.
A SuperSpeed child uses the root port and one route-string port nibble, with
zero TT fields. USB2 low/full-speed children retain the existing parent TT
slot, port and multi-TT fields. Set Isochronous Delay uses 40 nanoseconds for a
direct device and 80 nanoseconds plus the hub delay for a SuperSpeed child.
This follows USB 3.2 Revision 1.1 section 9.4.11. The profile never enables
device-initiated U1/U2. Children omit Set SEL, which section 9.4.12 requires
before enabling those states; direct-device Set SEL behavior is preserved.

All four TYPEC24 profiles require qualified source capacity of at least 900 mA.
USB2 descriptors retain their 500 mA budget and two-milliampere units. Physical
admission must bind the selected root protocol, direct or hub topology and leaf
class. Hub admission additionally needs evidence for the actual local supply
and downstream power. A self-powered descriptor does not establish those facts.
The topology record uses `a733-usb3-topology-physical-entry-v2`; legacy version 1
records admit only direct Gen1 attachment. Preparation also verifies the immutable
read-only `board_usb3_topology_capability` marker for hub or companion operation.

One owner retains the existing 128 KiB DMA parent, three device records and
command/event rings. No allocation or ring is released after uncertain requests,
wrong-slot events, removal or another leaf. The application returns the existing
copied sector or four-report result through USB0 and host capture.

Evidence in `~/.cache/sunstoneos/evidence/usb-ss-hub-20261020/` executes the actual
TinyUSB hub/HID/MSC code and xHCI/provider composition with modeled device effects.
Focused cases cover both root protocols, copied results, topology mismatch,
depth-command loss, malformed status/delay, context mismatch, stale completions,
removal and an extra leaf. Linked client/session/serializer and host checks cover
the native application. Physical power, topology, DMA visibility, interrupt
delivery and recovery remain unqualified. Nested hubs, simultaneous root
enumeration and additional leaves remain unsupported in these profiles.

## Detach disables the slot before its pages are reused

The xHCI core tears down a device only when the controller state is certain,
following xHCI 1.2 sections 4.4, 4.6.4 and 4.6.9. Two events start a teardown:

- **Root port disconnect.** The root port reports Current Connect Status (CCS)
  clear together with Connect Status Change (CSC). The core acknowledges the
  change bits, sends one REMOVE notice with result 0 and tears down every bound
  device, hub children before the hub. A CCS loss without CSC is uncertain
  and retains the session.
- **Hub child detach.** `a733_xhci_detach` starts teardown of one hub child,
  for example after its hub port reports a disconnect. It returns 1 when the
  teardown is accepted, already running or nothing is bound to that address.
  It refuses the root device, which follows its port.

Teardown revokes every caller destination immediately and admits no new setup,
transfer or endpoint opening for that device. Each poll then issues at most one
command and waits for its completion:

1. Stop Endpoint for each endpoint whose output context is Running. The
   controller's Stopped Transfer Event ends any TD on that ring; a forced
   Stopped event naming another TRB of the same ring is accepted.
2. Reset Endpoint for each endpoint that is Halted, so that every endpoint is
   Stopped before the slot is disabled.
3. Disable Slot.

A TD still outstanding after its endpoint stops or halts must be addressed by
the endpoint's TR Dequeue Pointer; the controller then no longer executes it.
Only the Disable Slot completion clears the device's DCBAA entry and zeroes its
input and output contexts, ring and bounce pages, including the extra bulk
pair when that device held it. The device record keeps its fixed page
addresses, and the 128 KiB parent remains DEVICE-owned. After the last root
teardown the port is unannounced; a later connection sends a new ATTACH notice
and enumerates again on the same pages and slot numbers. A cleared child record
can be bound again by `a733_xhci_bind_child`.

A transfer completing with USB Transaction Error or Split Transaction Error
retires its TD and halts the endpoint. A disconnect can report this error before
its port change. The core therefore revokes the caller destination and holds
that endpoint without a class completion. A detach that follows resets the
endpoint. Otherwise the session is retained 1 s after the error, or earlier at
the TD's own deadline. The 1 s limit covers a hub status interval of up to
256 ms and the port status request that follows it.

Teardown waits for an Address Device command that is still in flight. A
successful completion, or a USB Transaction Error completion after teardown has
started, lets teardown continue with Endpoint 0; no TD exists for that command.
A Transaction Error completion that arrives before the detach retains the
session.

Every other outcome retains the controller, contexts, rings and DMA parent.
These outcomes include:

- a failed or missing command completion;
- an endpoint in the Error state;
- a TD whose dequeue pointer has moved without a completion;
- a transfer event for a disabled slot;
- Type-C attachment loss;
- a teardown that has not finished 5 s after it started. This bound matches
  the pinned Linux `XHCI_CMD_DEFAULT_TIMEOUT`.

The TinyUSB adapter in `drivers/usb/a733_xhci_tinyusb.c` uses this teardown
for both kinds of disconnect:

- **Root disconnect.** On a REMOVE notice with result 0, the adapter ends the
  outstanding sector read or HID receive without output and reports no mounted
  device. It then passes the removal to TinyUSB, which unmounts each class and
  closes each device. Every record is already under teardown, so the core
  accepts each close. A REMOVE notice with result 2 follows a core fault and
  ends the session.
- **Hub child disconnect.** The hub port status described in
  [One external hub connects one downstream application device](#one-external-hub-connects-one-downstream-application-device)
  lets the pinned hub class report the removal. TinyUSB then unmounts and
  closes the child, and the adapter calls `a733_xhci_detach` for it.

After each accepted close, the adapter clears the descriptor, hub and leaf
state that names the closed address. At every root ATTACH notice it clears all
device state, because the core announces an attachment only after every slot
is disabled. TinyUSB ignores root attach and remove events for 150 ms after a
root attachment while it debounces the connection. A device that leaves in
that window ends its own enumeration when TinyUSB finds the port disconnected.

A device must mount by the startup deadline. After a removal, the adapter waits
for a new attachment until the session lifetime ends. The next root attachment
or hub port connection must then mount within the startup budget, measured
from that attachment. The budget is the startup deadline minus the time at
which the TinyUSB stack started.

Other closes still end the session. The core refuses to detach the root device
while its port reports a connection, so a TinyUSB close of that device retains
the session, for example after a failed enumeration. Endpoint close and bulk
clear-halt requests also retain it.

On the supervised board, a single-device workload that loses its device before
publishing a result issues its request again after a later attachment mounts.
A published result is kept. The two-leaf workload still ends on a removal,
because the other leaf's request can remain outstanding.

Unplugging a device from the board's Type-C connector also ends the Type-C
attachment, and Type-C attachment loss still retains the session. The root
disconnect path therefore serves a device unplugged behind an adapter or hub
that keeps the Type-C attachment. Adopting a new Type-C attachment generation
after VBUS removal and BOT reset recovery for storage remain required work.
The pinned TinyUSB storage class never requests a bulk clear-halt, so BOT reset
recovery would need a change to that class as well as bulk-endpoint recovery
in the core. VBUS removal alone does not establish that a self-powered hub lost
power, so hub topologies need separate evidence before reuse.

`checks/a733-xhci-detach.py` runs the actual core against a modeled controller
with address and undefined-behavior sanitizers. Successful cases detach and
reattach a direct device with a bulk read on each attachment, replace a hub
child while the hub status TD stays outstanding, and detach a hub with its
child. Further successful cases cover a stop that races a halt, a held
transaction error before disconnect, and a child detach while its Address
Device command is in flight, with either a successful or a Transaction Error
completion. Retention cases cover a missing CSC, Type-C loss, a stop that never
completes, a failed Disable Slot, a transfer event for a disabled slot, and a
held error without disconnect on a bulk TD and on a hub status TD whose deadline
is the session lifetime. In each held-error case, retention follows 1 s after
the error. The model checks that contexts and rings keep their contents until
Disable Slot completes.

`checks/a733-usb2-host.py` runs the adapter, the actual TinyUSB classes and the
core together against a controller model that completes Stop Endpoint, Reset
Endpoint and Disable Slot commands. It covers these cases:

- **Root disconnect and replug.** With a TD outstanding on a high-speed storage
  device, a high-speed HID device or a storage device behind a hub, the root
  port reports CCS clear with CSC. The core sends one REMOVE notice with result
  0 and disables every slot. No transfer callback or caller copy follows, the
  pending result returns -1, and the DMA parent stays DEVICE-owned. A new
  connection then enumerates on the same pages and completes a sector read or
  a HID report.
- **Hub child replacement.** The hub reports its child's port disconnected,
  disabled and changed while a sector read is outstanding, for a high-speed hub
  and for a SuperSpeed hub. Only the child's slot is disabled; the hub stays
  configured. A new child on the same port enumerates and completes a transfer.
- **Retention.** A root teardown whose commands never complete retains the
  session 5 s after the disconnect. A hub port that reports a disconnect while
  still enabled retains both slots without a sector copy.

Controller timing, DMA visibility and physical disconnect behavior remain
unqualified.

## Two USB2 hub tiers share the three device records

A session that sets `hub_tiers` to 2 in `struct a733_usb2_native` admits one
USB2 high-speed hub on a port of the root-port hub. The second hub's device
descriptor must declare the single-TT or multi-TT high-speed protocol, and every
other hub rule above applies to it unchanged. A hub below the second tier, a
full-speed hub and a SuperSpeed hub below another hub close the session. Both
hubs and one leaf use the three xHCI device records, so the second tier serves
one leaf.

The xHCI core binds each child to its parent hub's slot and port. The child's
route string holds one port number per tier, as xHCI 1.2 section 4.5.2
requires: port 3 of a hub on root-hub port 2 gives route string `0x32`. A
low- or full-speed child uses its parent hub's transaction translator: the Slot
Context names the parent's slot and port and copies the parent's multi-TT
selection. Every hub between the root port and a low- or full-speed device is a
high-speed hub, so the parent is always the nearest high-speed hub.

The TinyUSB adapter keeps one hub state per hub in `hubs[]`: identity, power
status, powered ports, connected ports and the status-change transfer. A
disconnect reported by either hub removes the device on that port. When the
second-tier hub leaves, TinyUSB closes the hub and then its child; the core's
detach of the hub already includes the child. The core tears down a child
before its hub, and checks this by slot ownership instead of record order,
because a child can hold a lower record than its hub after an earlier removal.
A detaching hub whose child is not detaching is inconsistent and retains the
session.

## Bus-powered hubs stay within the upstream current budget

A session that sets `bus_powered_hubs` admits USB2 hubs whose configuration
descriptor reports bus power (`bmAttributes` 0x80). The adapter applies the
current limits of USB 2.0 sections 7.2.1 and 11.13 to every USB2 device:

- **Upstream current.** A root-port device may draw at most 500 mA, and at most
  the qualified Type-C source current. A port of a self-powered hub supplies
  500 mA and a port of a bus-powered hub supplies one unit load, 100 mA. A
  configuration whose `bMaxPower` exceeds its port's supply is refused before
  SET_CONFIGURATION.
- **Bus-powered hub total.** The hub's configuration current excludes its
  downstream ports, each of which may draw 100 mA. The hub's `bMaxPower` plus
  100 mA for every port must fit within its own upstream supply. A seven-port
  bus-powered hub on the root port therefore fails, and a bus-powered hub below
  another bus-powered hub fails for any port count.
- **Reported status.** Standard GET_STATUS must report the same power source as
  the configuration descriptor. Class hub GET_STATUS must report no
  over-current. Its Local Power Source bit describes a local supply, so a
  bus-powered hub may report either value.

These rules use descriptor and status values only. They do not measure current
or establish that a hub's power switching works, and the Type-C source must
still be qualified for the total load.

## An isochronous IN stream carries USB Audio Class 1 capture

The xHCI core supports one isochronous IN stream per controller for a full- or
high-speed USB2 device. Its service interval must be at most 1 ms: full-speed
`bInterval` 1 or high-speed `bInterval` 1 to 4. A packet holds at most 512
bytes. The stream uses the device's own data ring and bounce page, so the
128 KiB DMA layout is unchanged. Its endpoint context uses type Isoch IN, error
count 0 and a maximum ESIT payload of one packet.

Once started, the core keeps eight one-packet Isoch TDs on the ring. Each TD
sets Interrupt-on-Short-Packet, Interrupt-on-Completion and Start Isoch ASAP,
and uses a fixed 512-byte part of the bounce page. The core handles ring events
as the pinned Linux `handle_tx_event` does (xHCI 1.2 sections 4.10.2.8,
4.10.3.1 and 4.10.3.2):

- **Success or short packet.** The TD carries data. `a733_xhci_isoch_take`
  copies the oldest such packet into caller storage after its completion event
  and frees the entry; the next poll queues a replacement TD and rings the
  doorbell.
- **Transaction, babble or split-transaction error.** The TD ends without data
  and the stream continues. The core counts it in `isoch.dropped`.
- **Missed Service Error.** The event names the last TD the controller missed,
  or has no TRB pointer. When the Event Ring is full, one event can cover
  several missed TDs. Every queued TD up to the named one ends without data and
  is counted in `isoch.dropped`. A controller without the Contiguous Frame ID
  capability may skip further TDs without an event. Until the next event of
  another kind, that event may therefore name a later TD. The queued TDs before
  it end without data and are counted the same way.
- **Ring Overrun.** The controller found the ring empty and removed the
  endpoint from its schedule. The event names where the ring was empty. This is
  the enqueue position when every queued TD completed. It is the oldest queued
  TD when the controller looked just before a late refill was written. It may be
  a later TD after missed service intervals, or have no pointer on a pre-1.1
  controller. TDs before the named position end without data. The core counts
  the event in `isoch.overruns`. While TDs remain queued, it rings the endpoint
  doorbell again so that the controller schedules the endpoint again.

Any other completion code, or an event that names a TD the rules above do not
allow, retains the session and its DMA parent. So does a poll that finds the
event ring empty 100 ms or more after the last ring activity while TDs are
queued. Events that arrived while the owner was not polling are processed
first, so a late poll by itself does not retain the session. A device teardown
stops the endpoint. The TDs still queued end without completions when the
dequeue pointer names one of them or the enqueue position.

The TinyUSB adapter admits one USB Audio Class 1 capture device when the session
sets `audio_capture` and the image builds the pinned TinyUSB audio host class.
Its configuration must consist of one Audio Control interface and Audio
Streaming interfaces; any other interface class, such as a headset's HID
buttons, closes the session. Every capture alternate setting must use the same
isochronous IN endpoint. The adapter records up to four capture alternates
whose packet size and interval fit the stream above; a configuration without
one closes the session. Playback interfaces may be present and are never
opened. TinyUSB's audio class parses the class-specific descriptors itself.

The adapter allows these control requests for the audio device:

- the standard enumeration requests;
- SET_INTERFACE to alternate 0 or to a recorded capture alternate;
- volume or mute GET_CUR, GET_MIN, GET_MAX and GET_RES (UAC1 section 5.2.2.4)
  addressed to a Feature Unit that the Audio Control interface's
  class-specific descriptors declare;
- the endpoint sampling-frequency SET_CUR (UAC1 section 5.2.3.2.3.1), whose
  three data bytes must equal the requested rate.

A stall on any of them closes the session.

`a733_usb2_audio_start` selects the capture configuration with signed 16-bit
little-endian samples at the requested rate and channel count, opens its
endpoint and starts the stream. It returns 0 without other effect when the
device offers no such configuration. TinyUSB then issues SET_INTERFACE, the
sampling-frequency request when the endpoint supports it, and the first packet
request; that sequence must complete by the caller's deadline.
`a733_usb2_audio_read` copies the whole frames available, at most 1024 bytes,
after a fresh attachment observation. Each block carries the attachment
generation, a block sequence, rate, channel count, device identity and three
continuity totals: intervals without data, empty-ring reports and frames that
TinyUSB's 4096-byte overwrite FIFO replaced before a read. The copied samples
are not resampled or decoded.

The owner task must poll often enough to collect each packet and queue its
replacement: one controller event and one ring refill take one poll each, so
two polls per service interval keep the ring full. A removal ends the stream
without output. A replugged microphone must be started again.

Images build the audio class only when they compile
`class/audio/audio_host.c` from the pinned TinyUSB tree and define
`SUNSTONE_USB_HOST_AUDIO=1`; the current board build does not, so its adapter
refuses audio admission and returns no audio data. The board application has no
workload, wire format or host decoder for copied audio yet.

## Required wider modes and physical qualification

The implementation supports the bounded storage, HID, hub and audio capture
profiles above. It refuses:

- low/full-speed mass storage and richer mass-storage configurations;
- more than two USB2 hub tiers and a hub below a SuperSpeed hub;
- more than two leaves, or two leaves of the same class;
- interrupt OUT, high-bandwidth periodic transfers, isochronous OUT and more
  than one isochronous stream;
- USB Audio Class 2, CDC-ACM and other device classes, and other SuperSpeed HID
  profiles;
- Gen2, streams, nonstandard speed-ID tables and more than eight scratchpads.

GET_MAX_LUN protocol-stall recovery is implemented for the admitted storage
profile. Bulk stalls, other control stalls and uncertain recovery retain the
session; generic clear-halt and BOT reset recovery remain unsupported. These are implementation limits, not claims that the board
cannot support those modes.

Further required work includes more device records and endpoints per device
for additional leaves and classes such as CDC-ACM, simultaneous enumeration on
the USB2 and SuperSpeed roots, a board workload and wire format for copied
audio, wider xHCI endpoint modes, physical interrupt
qualification,
USB2 device/OTG roles, wider SuperSpeed classes and topology, Type-C role/PD
negotiation where electrically supported, safe shutdown/drain, adoption of a
new Type-C attachment generation and BOT reset recovery for storage. DisplayPort lane ownership must be coordinated with display users.
USB0 host capability requires an electrically valid route; USB1 needs a physical
route before it can be considered usable on this board revision.

A native image still needs authenticated boot authority for the board revision,
rail topology/stability, cold-master exclusion, immutable memory mappings and
independent recovery. Physical qualification requires the matching Type-C
connector, a known high-speed storage device, demonstrated VBUS shutdown and
observable transfer results. A source or software model cannot supply those
facts.

## Pinned implementation sources

The A733 user manual supplies USB2 register routing and clock fields in
sections 4.1.6.199–204, 18.2.6.2 and 18.4. The V1.2 schematic supplies the connector
and power route. The pinned board kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` supplies DWC3 global programming,
PHY tuning and the USB2-only replacement-clock sequence. Its adapted DWC3
programming retains the source copyright and GPL notice.

The original xHCI core follows the Intel xHCI 1.2 specification, including
sections 4.6, 4.9, 4.10, 4.11.2.2, 5.4, 5.5, 6.2, 6.4 and 6.5. TinyUSB revision
`d668b0abb7219b9ce324fa88d46604bb9e869973` supplies actual host enumeration and
mass-storage and HID class code under its existing MIT license. Downloaded sources and
raw evidence stay outside the repository. The hub adaptation follows the
[USB-IF USB2.0 specification](https://www.usb.org/document-library/usb-20-specification),
sections 9.4 and 11.23–11.24; its base PDF and bundle identities are pinned in
`sources.lock.json`. The USB3 path also pins the
[USB-IF USB3.2 bundle](https://www.usb.org/sites/default/files/usb_32_202206_0.zip)
and the same board-kernel Cadence PHY, USB core latency and xHCI root-BOS sources.
The focused runner snapshots its
inputs and preserves each executable, compiler command and result.

## Software evidence covers enumeration and retained failures

The focused runner is `checks/a733-usb2-host.py`. Its saved execution in
`~/.cache/sunstoneos/evidence/usb2-host-20261017/batch1` passed on the first
invocation with address and undefined-behavior sanitizers. The model executes
the actual TinyUSB enumeration, mass-storage class and xHCI ring code.

The successful transfer enumerates a high-speed device and compares all 512
returned sector bytes. Refusal cases cover a changed configuration length,
short descriptor, incorrect status tag, short data, active cancellation,
attachment loss and attachment loss while waiting for the common mutex.
The admission case checks native translation boundaries and compares reference
clock programming with the pinned Linux function.

The Type-C observation starts from an explicitly prepared model checkpoint.
Host calls use the real snapshot, validation and revocation functions; this
check does not execute FUSB302 role classification or qualify physical power.
The separate Type-C provider evidence supplies its transaction checks.

The platform provider, native adapter, xHCI core and TinyUSB binding each compile
for AArch64. The evidence directory retains the executed sources, generated
reference source, executable, four native objects, tool identities, commands and
logs. No board operations or performance measurements are part of this check.

## Offline interrupt-path evidence and remaining prerequisites

The focused results are preserved under
`~/.cache/sunstoneos/evidence/usb-host-interrupt-20261018/`.
The actual TinyUSB host/HID code, xHCI command/transfer rings and shared
providers run with address and undefined-behavior sanitizers. Low-, full- and
high-speed cases enumerate and copy four reports, including short and
zero-length completions. Full-speed execution checks the actual Evaluate
Context completion before the full device descriptor transfer.

Refusal cases cover invalid interval, high-bandwidth packet bits, oversized
report descriptors, wrong endpoint direction, stalled or mismatched-TRB
completion, detach, timeout and active cancellation. Failed cases retain DMA
and preserve caller destinations. Existing MSC cases passed before the HID
fixture additions; their frozen sources and executables remain available.

The full supervised board image links with the real HID class. Native
instruction checks execute the board's four-report consumer and version 2
serializer using report objects from the composed stack check. The host decoder
and durable capture consume those actual output bytes. Shared-provider
observations, scheduling, device effects and transport are modeled. A local
symbol-resolution fixture repair resumes only the unexecuted serializer/host
portion after preserving its completed report batch.

No board, USB device, queue or desktop input interface is accessed. Physical
qualification needs available supervised recovery, reviewed board/rail/Type-C
admission, the exposed TYPEC24 data connector, an electrically suitable adapter,
a supported directly attached HID device and a way to produce known reports.
VBUS shutdown, device enumeration, report timing and observed report contents
remain physically unqualified. Both microSD cards remain preserved. Physical CPU
interrupt delivery and device replacement after a Type-C attachment change
remain required work.

## Offline topology, power and audio evidence

`checks/usb-host-classes.py` runs the actual USB2 platform, xHCI core and
TinyUSB adapter with the pinned TinyUSB hub, MSC, HID and audio classes under
address and undefined-behavior sanitizers. Its controller model reads the
command and transfer rings and contexts from the DMA parent, follows each slot's
route string through a modeled device tree and posts events to the programmed
event ring. It rejects a route string, TT field or endpoint context that does
not match the modeled device.

Successful cases cover:

- storage and a low-speed keyboard, with single-TT and multi-TT hubs, below two
  hub tiers;
- removal and replacement of the second-tier hub, and of its leaf alone;
- a bus-powered hub serving a 100 mA keyboard, and a self-powered hub below a
  bus-powered hub serving a 500 mA storage device;
- audio capture at 16 kHz and 48 kHz, mono and stereo, at full speed directly
  or through a hub TT, and at high speed, with contiguous sample counters;
- injected missed-service intervals in four forms: one event per missed TD,
  one event for three missed TDs, events without a TRB pointer, and one event
  followed by two missed TDs without events;
- a ring overrun after a 30 ms owner stall, an overrun posted after the refill
  that names the first new TD, and a 150 ms owner gap whose waiting events are
  processed without retaining the session;
- retention 100 ms after the ring stops producing events, and a root disconnect
  followed by a replugged stream.

Refusal cases cover a second tier without admission, a third tier, unadmitted
bus power, a seven-port bus-powered hub, an over-budget leaf and a mismatched
power status. They also cover nested bus-powered hubs, an over-budget
self-powered hub, audio without admission, oversized audio packets and a
headset HID interface. Each case checks the stage at which the refusal occurs. The adapter, core
and audio class also compile for AArch64 with the audio class enabled.
`checks/a733-xhci-detach.py` adds a core case in which a second-tier hub owns a
higher record than its child and teardown still disables the child first.

These results are software results. Physical hubs, hub power, isochronous
timing, DMA visibility and USB microphones remain unqualified.

## Offline Gen1 evidence

`usb-host-superspeed-20261019/frozen` preserves the selected 163-object image and
all 435 actual compiler inputs, including the private TinyUSB files and Cadence
includes. The source adaptation generators and their pinned original inputs are
recorded separately. `snapshot/manifest.json` binds the source, tool and result
inventory. The native image uses supervised board startup and CPU0 INTID 187;
`root-route/review.json` admits only the additional workload selector and reuses
the unchanged route evidence.

Twenty focused SuperSpeed composition cases exercise the actual Cadence
functions, TinyUSB enumeration/MSC callbacks and xHCI ring/context code under
ASan/UBSan. They cover both orientations, maximum burst, advertised streams with
stream contexts disabled, required BOS/ISOCH/SEL transfers, power and resource
conflicts, malformed companions, link loss, unknown port events and stale
generations. Two affected USB2 cases check HS MSC and FS HID through the private
stack adaptation. Existing hub, dual-leaf and polling/IRQ evidence remains tied
to its frozen inputs; no unchanged suite was replayed.

The linked board instruction checks consume the actual composition's copied
sector. They cover success through USB0 serialization and host capture, refusal
of HS fallback publication, and authority loss after copy but before publication.
Eight host-only capacity cases check the physical-record bound without creating
a launch bundle. Focused Astra review covered the new PHY, descriptor, ownership
and publication paths before their model execution. These are software results;
physical link operation, source capacity, DMA confinement and interrupt delivery
remain unqualified.
