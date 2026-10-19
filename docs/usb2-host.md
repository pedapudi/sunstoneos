# USB2 host storage reads and copied HID interrupt input

The USB2 host implementation connects the A733 DWC3/xHCI controller to TinyUSB
and copied read-only storage and HID input APIs. It supports one directly attached
high-speed mass-storage device or one low-, full- or high-speed HID interface.
The storage profile uses one configuration, interface 0, alternate 0, two bulk
endpoints and LUN 0. Storage configuration descriptors must contain the standard
32-byte configuration/interface/endpoint sequence. Blocks must be 512 bytes.
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
to these addresses and fields. INTID 187 must remain disabled, inactive and
without a pending handler; this profile polls completion events.

## Type-C power remains independently supervised

The actual Type-C provider owns FUSB302, source-role classification and PL8 VBUS
control. The host pins one provider identity, attachment generation and polarity.
Every controller operation requires a fresh observation from that provider.
A busy publication permits no new MMIO, DMA publication or caller-output copy.
Loss permanently revokes the host session; a new attachment is not adopted.

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
transfer-descriptor completion. Opening bulk endpoints does not make them usable
until Configure Endpoint completes.

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

Abort, malformed completion, disconnect and expiry revoke caller destinations
and retain all private DMA backing. Reset, power-off and a halted status are not
claimed to block and drain all external transactions. There is no release,
reattach or retry API. A failed command is not reported as a completed read.

The TinyUSB stack uses its single-context queue implementation inside the actual
FreeRTOS owner task. No interrupt or second task may call the stack. Each host
poll advances at most one xHCI event/state and two TinyUSB queue iterations.
All control calls are asynchronous. The native image must schedule this task
within operation deadlines and keep the independent Type-C tasks runnable.

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
controller schedules periodic bus transactions. CPU completion handling still
polls the event ring; this work does not qualify INTID 187 interrupt delivery.

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
One consumed result permits another receive. Failure, disconnect, expiry or
active cancellation permanently closes admission and retains all DMA backing.
There is no reattachment or endpoint-stall recovery path.

The native board task preserves its storage behavior and selects HID by the
actual enumerated class. It collects four reports into an immutable 512-byte
batch before publishing success. It then continues observing attachment,
controller faults and lifetime without changing the published bytes. The board
wire session remains unchanged; internal native layouts include new typed
periodic/HID metadata and require recompiling their embedding consumers.
Earlier frozen images remain tied to their original compiled layouts.

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

## Required wider modes and physical qualification

The implementation supports the bounded interrupt-IN HID profile below. It
refuses low/full-speed mass storage, hubs, interrupt OUT, high-bandwidth periodic
transfers, isochronous endpoints, SuperSpeed, nonstandard speed-ID tables, more than eight
scratchpads and richer mass-storage configurations. Endpoint-stall recovery is
not implemented; devices that stall GET_MAX_LUN are retained rather than using
the usual fallback. These are implementation limits, not claims that the board
cannot support those modes.

Further required work includes wider xHCI endpoint modes, interrupt integration,
USB2 device/OTG roles, SuperSpeed COMB0 routing and orientation, Type-C role/PD
negotiation where electrically supported, safe shutdown/drain and repeated
attachment. DisplayPort lane ownership must be coordinated with display users.
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
raw evidence stay outside the repository. The focused runner snapshots its
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
remain physically unqualified. Both microSD cards remain preserved. Broader
speed/class support, hubs, isochronous transfers, SuperSpeed, CPU interrupt
delivery and safe detach/reuse remain required work.
