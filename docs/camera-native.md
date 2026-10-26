# Native reference camera capture

The native camera binding selects CAM1 or CAM2, prepares OV5640 exposures and
copies one completed NV16 image into an application-owned memory reservation.
The build selects any of the sensor's ten existing sizes, up to 2592 × 1944;
the default is 640 × 480. NV16 stores equally sized luminance and interleaved
chroma planes, each containing width × height bytes. The sensor sends two-lane UYVY; the pass-through route and camera DMA
produce these planes without an ISP image-processing pipeline.

The implementation uses the existing clock registry, media controller, rail
ledger, queued PMIC provider, banked pin manager, system IOMMU and singleton
memory allocator. It executes the existing sensor, analog calibration, CSI,
route and DMA controllers. The [native camera application](camera-application.md) connects these providers
to executable startup and USB output. Physical operation and its protected boot
admission remain unqualified.

## Boot admission and wiring

The protected record in `a733_camera_native.h` binds the camera owner, image
build, boot epoch, generation, lease and deadline to reviewed physical evidence.
The record includes inherited pin configurations, the module's reset wiring,
supply identities and programmed voltages, APB1 configuration, analog trim,
receiver timing and mapped guard requirements. Its test-pattern byte selects
the ordinary sensor image (0) or the OV5640 color bars (1). Its discard count,
from 0 to 15, sets how many images are captured and discarded unread before the
image that becomes readable, for example while sensor automatic exposure
settles. Each discarded image is a complete sensor stop and start. The count
occupies the alignment gap before the supply list, so existing field offsets
are unchanged and zero keeps the single exposure. Nonzero identifiers or
matching fields alone cannot establish the required authority.

The boot coordinator must establish these conditions before constructing the
binding:

- Exclude remote firmware and all ten system-IOMMU masters. Resolve inherited
  transactions and page walks before installing either bank's directory. Both
  banks start disabled; disabled status alone does not establish this history.
- Keep both IOMMU banks and all ten masters' register domains powered and
  accessible through the final fault scan. Retain shared fabric and parent
  clocks for the complete operation.
- Admit the media controller for VI and IOMMU, including its shared register
  words and parent clocks. Reserve the selected TWI11 or TWI9 independently
  in the same registry.
- Authenticate camera-module supplies, bank voltages, pullups, analog trim,
  receiver timing, reset safety and independent recovery. Supply readback
  confirms programmed PMIC settings; it does not measure physical voltage.
- Admit the singleton memory pool through the actual boot memory contract.
  Qualify the four-kibibyte prefetch guards for this camera profile. A mapped
  guard does not itself establish the hardware's maximum prefetch extent.

The authenticated media record selects exactly one connector through
`camera_mclk_mask`: 2 selects CAM1 and 4 selects CAM2. Other masks are refused
before allocation. The native object freezes that port and refuses a changed
selector before provider preparation or subsequent access. Host admission binds
the media and camera configuration hashes to the same reviewed facts, including
module wiring and the explicit `settle` and `deskew` fields. CAM2 has no default
or physically qualified deskew value.

| Connector | CSI pins | MCLK | Power-down | Sensor bus |
| --- | --- | --- | --- | --- |
| CAM1 | PK0–PK9 | PE5, MCLK1 | PE6 | TWI11, PE7/PE8 |
| CAM2 | PK10–PK19 | PE9, MCLK2 | PE10 | TWI9, PE14/PE15 |

Power-down starts asserted high on the selected connector. Its inherited pin
entries describe ten CSI pins, MCLK, power-down, two bus pins and optional reset,
in that order. The connector carries no other module-control signal, so the
record names one of two module profiles:

- **Separate reset.** The record names an admitted reset GPIO outside all
  selected CSI, MCLK, power-down and bus pins, initially asserted low. The binding claims it as a fifteenth pin and
  releases RESETB 1 ms after power-down release.
- **Power-down only.** The module provides its own RESETB, for example through a
  pull-up. The record sets the power-down-only flag and leaves the reset bank,
  reset pin and its inherited configuration zero. The binding claims fourteen
  pins, refuses any RESETB request and keeps the 1 ms and 20 ms waits between
  power-down release and the identity read. The Linux driver uses this sequence
  when no reset GPIO exists.

The module's wiring evidence must establish which profile applies.
Conflicting schematic supply labels prevent choosing camera rails from the
connector name alone. The binding observes three explicitly qualified PMIC
outputs and never changes their voltages.

One designated privileged FreeRTOS task advances capture. The existing PMIC
provider task must run independently to complete queued supply observations.
Each register access rechecks the media, selected TWI clock and rail leases. When the
clock registry or rail ledger reports BUSY, another task holds its metadata lock;
the binding then repeats only the status read until the boot deadline. Any other
result, or the deadline, faults the capture with all resources retained.
The constructor runs once during serialized primary memory preparation. All
referenced providers and leases remain private and valid for the image lifetime.

## Sensor bus and capture ownership

The bounded byte-transfer engine supports TWI11 at `0x0251b000`, TWI9 at
`0x02519000` and S_TWI1 at `0x07084000`. CAM1 uses seven-bit address `0x3c` and two-byte big-endian
register addresses. Reads and single writes carry one data byte; the sensor's
block writes carry up to 40 bytes to consecutive registers in one transaction. A
40-byte block takes about 4 ms at 100 kHz, within the 10 ms limit for one
transaction. The Type-C controller uses the same engine with one-byte register
addresses and its separate native provider. S_TWI1 does not use the extra PMU
registers documented only for S_TWI0.

The native camera-bus provider drives TWI11 for CAM1 (pins PE7/PE8) or TWI9 for
CAM2 (pins PE14/PE15, function 8 in the pinned manual and device tree). One
instance exists per image, and its transactions hold the camera bus mutex
`A733_PLATFORM_I2C11_LOCK` on either controller. The camera binding selects
the matching native provider and byte engine from the authenticated connector. On modeled registers, `checks/twi-busy.c`
runs the same prepare, write and read through a TWI9 binding and finds the
same store sequence at the TWI9 controller and gate word. No board run covers
TWI9.

The provider authenticates real clock, pin and rail leases. It observes the
leased APB1 source and divider, releases the initially asserted local reset,
opens the gate and selects a source-defined rate at or below 100 kHz. It
excludes legacy interrupts, autonomous driver and DMA operation. The camera bus
mutex remains owned from START through acknowledged STOP and
final idle validation. NACK, arbitration loss, timeout or uncertain MMIO retains
the transaction without issuing STOP, resetting the controller or retrying.
Explicit cancellation has the same retained outcome and cannot unlock the bus.
Clock and rail authority is checked at every native bus access. Pin-manager
health is inspected under the common register mutex before acquiring I2C and
after releasing it. An in-flight transaction may reach that final boundary;
a pin-manager fault prevents its result from becoming available.

For the default VGA mode, a 16 KiB-aligned parent reservation contains 32 KiB of IOMMU tables, 8 KiB of
parameter memory and two 311,296-byte plane allocations. Each plane includes
307,200 payload bytes and a 4 KiB guard. The 663,552-byte parent uses 162 pages
and one allocator record. The camera reaches the parent through the system
IOMMU at fixed I/O virtual addresses (IOVAs): the parameters at `0x1000_0000`,
the Y plane at `0x1000_2000` and the UV plane at `0x1004_E000`
(`A733_CAMERA_NATIVE_IOVA` and fixed offsets). The IOMMU translates each IOVA to
the parent's physical pages, which may lie anywhere below 2^34, the limit of
the IOMMU entry encoding. The CPU address of the parent need not equal its
physical address. The default VGA spans use one of the IOMMU builder's four 4 MiB
windows and 154 leaf words at any placement. The constructor checks the parent's
size and physical range before it writes any byte. A refused parent stays
DEVICE-owned and the constructor faults. The native constructor refuses insufficient capacity; it does not
shrink the mode. Before the parent becomes DEVICE-owned, the constructor fills
both planes and their guards with `0xa5`. DMA admission cleans and invalidates
that range, so bytes the DMA never writes hold the fill instead of earlier DRAM
contents such as a previous image. Each rearm fills both planes again before the
same clean and invalidate, after DMA status is acknowledged and capture is
inactive. The retained path does not read guards, so it cannot check them; the
DMA status check remains the device-side detector of short frames and address
errors. The host decoder reports rows that still hold the fill.

The parent becomes DEVICE-owned before preparation and remains reserved after
success or failure. Tables and parameter memory are immutable after publication.
The calibration lease permits local receiver setup only through the capture
coordinator's authenticated handoff. Sensor packet enable follows actual route,
DMA and single-image parser preparation.

## Completed-frame access retains hardware authority

The retained readout requires the original capture-done event, successful sensor
stop, a fresh receiver sample, self-cleared single-image capture, idle route
status and a completed system-IOMMU fault scan. DMA completion and error status
are checked again before CPU cache acquisition. No guard bytes become payload.
The native wrapper checks its deadline and actual ownership after cache callbacks
before publishing frame availability.

`a733_camera_native_copy` accepts a distinct CPU-owned allocation from the same
native singleton and camera owner. It copies only the requested payload range,
checking ownership and time between bounded chunks and before returning success.
A failed copy can leave partial destination bytes, which remain untrusted. The
caller serializes use, pins the destination and exposes it only after success.
This privileged interface does not authorize arbitrary EL0 addresses.

Source buffers, IOMMU tables, clocks, rails and pins remain reserved. A
discarded image is rearmed through the coordinator's
[retained rearm sequence](camera-capture.md) before it becomes visible, so no
view of it can exist. The readable image has no rearm or disposal operation.
Completion permits access to this still image; it does not prove that every
system transaction and page walk has drained. Existing close-and-release APIs
retain their separate drain requirements.

The [stop and drain source audit](camera-stop.md) describes the documented
parser frame boundary and the missing receiver, DMA and IOMMU retirement
conditions. Vendor stream-off does not establish the complete release contract.

## Finite streaming keeps one slot held across the copied frame

`a733_camera_native_supervised_stream_init` selects two slots per plane for
the selected UYVY mode. For default VGA, the parent reservation is 1,286,144 bytes: 32 KiB of
tables, 8 KiB of parameters and four 311,296-byte guarded slots. Together with
the 614,400-byte CPU output, it fits the application's existing 2 MiB pool.
The constructor fills and publishes both spans and derives their IOVAs from
the same allocation. The supervised boot record still requires all-master
exclusion, admitted guards and manual power removal. The image identity binds
the stream build option; the record format is unchanged.

The native packet handoff admits the coordinator's continuous video mode.
A delivered slot receives cache invalidation over its width × height payload in
each plane. The coordinator supplies the completion and address-ownership
checks before that callback. Guard bytes remain outside the copied payload.

`a733_camera_native_stream_copy` takes the oldest delivered slot and copies at
most 4 KiB per call into a CPU-owned allocation from the same owner and pool.
The destination lease, including its generation, stays fixed across calls.
The slot remains held until both complete payloads have been copied and checked.
A failure leaves any partial destination untrusted and retains the source.
The boot record's discard count consumes complete delivered slots without
copying them, while the sensor continues running.

The application polls capture before every copy call. VGA needs 150 calls;
the largest mode needs 2462 calls, including a short tail in each plane. Holding
one of the two slots can cause frames in the other slot to be overwritten;
the existing ring counts those drops and protects the held slot. The finite
session uses its admitted absolute deadline throughout preparation and copying.
There is no unbounded renewal or in-place restart.

After giving the copied slot back, `a733_camera_native_stream_stop` requests
the existing sensor-stop sequence. The absent packet-quiescence provider
then refuses, retaining the parser, DMA, IOMMU and every hardware lease.
Sensor register readback does not establish transaction drain. The separate
CPU copy remains usable; the application reports sensor-stop status and the
absence of drain proof independently.

The earlier VGA native model completed the copy in 150 pieces, with a maximum observed
DMA status-read gap of 4,044 modeled microseconds, below the mode's roughly
16,500 µs limit. It copied 614,400 bytes and invalidated each copied plane
once. DMA failure during copying, slot-cache failure and a changed destination
generation preserved ownership. The still-image nominal case also passed.
These checks reuse the actual native providers and controllers with modeled
registers, cache visibility and task identity. They do not measure hardware
latency. Earlier coordinator tests retain their delayed-start and frame-race
coverage; their implementation is unchanged.

Evidence is under `~/.cache/sunstoneos/evidence/resume-camera-native-1025/`.
The initial stream fixture's one-second session expired after preparation and
39 copy pieces; the finite stream fixture now admits two seconds to cover
preparation plus the 150 scheduled copy pieces. Production deadlines did not
change. Another fixture correction uses the DMA error bit instead of a normal
status bit. Earlier passing cases keep their original artifact identities.

## Sources and focused verification

The pinned A733 manual defines TWI0-compatible instances in §18.11.7, the
standard-mode divider in §18.11 and single-image parser behavior in §16.1.13.4.
The still-image capture-done description is in §16.1.14.36; its status register
is in §16.1.14.37. Both still-image branches of the pinned BSP's
`bsp/drivers/vin/vin-video/vin_core.c` publish a completed buffer on this event.
Existing camera component documents retain their register and table provenance.

The focused native composition fixture executes the actual resource managers and
camera controllers with modeled MMIO, task identity, cache visibility and
physical admission. It covers completed payload copying, bus arbitration loss,
DMA errors, incomplete single-image capture, cache failure, late clock-authority
failure, pin-manager failure and IOMMU faults. A color-bar case checks that the
modeled sensor register `0x503d` reads `0x84`. A power-down-only case claims
fourteen pins and waits at least 21 ms from power-down release to the identity
read. Two cases hold both registries BUSY: capture succeeds when the hold ends
before the deadline and faults when it does not. A short frame without DMA error
status leaves fill bytes in the unwritten rows. Every nominal case sends the
sensor configuration in 36 block writes of up to 40 bytes. A discard case
captures three images with distinct payloads, checks that each previous image
stays in the planes until the rearm refills them, that every image starts from
the fill, and that SCAP is rewritten only after both quiet intervals. It then
copies only the third image. Three rearm faults, from changed DMA status, a
packet during the quiet interval and a lost SCAP write, each keep every lease
and leave SCAP clear. Every nominal case checks that the route parameter
register and both plane address registers hold the fixed IOVAs, and that the
IOMMU tables translate them to the parent's physical spans. One case places the
parent at physical `0x1_8000_0000` with its CPU alias at `0x4800_0000` and
captures and copies an image. Another places the parent across 2^34 and
requires refusal before the fill, any register access or any table write. Given
a file name as a second argument, the fixture writes every modeled register
write to that file. A separate short case covers S_TWI1 register width and
retained cancellation. Models do not qualify wiring, prefetch, timing,
coherence, reset, camera output or physical recovery.

Wider resolutions, alternate sensors,
ISP processing, interrupt-driven service, buffer reclamation and a general
application service remain required hardware capabilities outside this reference
capture profile.


## CAM2 uses the existing capture and retention contracts

CAM2 selects parser 1, the PHY at `0x05810200`, the packet port at
`0x05811400` and the parser at `0x05821000`. ISP1, VIPP0, DMA0 and the
all-master IOMMU exclusion remain the same exclusive path. Both still and
finite continuous modes use the existing copy and retained-stop contracts.
The [connector design](designs/camera-cam2-native.md) records routing sources,
per-port authority, costs and focused checks. Its model covers CAM2 still
capture, power-down-only modules, retained rearm, finite streaming, selector
changes and incorrect inherited power-down. Local programming targets only
the selected connector after shared analog preparation. No camera is attached;
these checks establish no sensor compatibility, image quality or physical timing.

## All native sizes use bounded memory and complete output

`SUNSTONE_CAMERA_SIZE` selects QQVGA, QCIF, QVGA, VGA, NTSC, PAL, XGA, 720P,
1080P, or QSXGA. Native construction derives the existing two-lane UYVY mode
before allocating. The selected geometry is immutable for the session.
[The layout and admission design](designs/camera-native-modes.md) lists every
size and its guarded reservation.

Still sizes through PAL and stream sizes through VGA retain the compact 2 MiB
pool. Larger images reserve only `0x48000000..0x4a000000`, a 32 MiB write-back
pool. Sealed session and profile admission precede the sixteen external CPU
mapping blocks and allocator publication. Other memory profiles are incompatible,
and the camera branch does not call the general DRAM map expansion.
The largest stream uses 36 KiB of IOMMU tables, 8 KiB of parameters, four
5,046,272-byte guarded slots, and a 10,080,256-byte output allocation. Their total
is 30,310,400 bytes. Image data occupies 10,077,696 bytes; padding and guards are
excluded from copies and USB output. Its five IOMMU windows map parameters and
source slots only. Existing clients retain the four-window builder.

Software evidence is under `~/.cache/sunstoneos/evidence/camera-modes-1026/`.
Cheap compiled layout checks cover all ten modes and both operations. The
actual native still and stream paths execute for default VGA and QSXGA. Maximum
still output passes in `native-max-geometry`; maximum streaming and an injected
DMA failure pass in `native-max-stream`. Earlier VGA routing, sensor tables,
interval-opening, and retained-stop evidence remain applicable.

| Modeled native stream cost | VGA | QSXGA |
| --- | ---: | ---: |
| Register accesses | 8126 | 10471 |
| Register writes | 2359 | 2425 |
| Delivered-plane invalidations | 2 | 2 |
| Copy calls | 150 | 2462 |
| Copied bytes | 614,400 | 10,077,696 |
| Largest status-read gap | 4044 µs | 4044 µs |

VGA counts match the accepted preceding native evidence. Larger payloads need
more bounded copy steps and longer capture polling; source mode tables account
for additional configuration writes. With the same compiler flags, the native
binding grows from 14,460 to 14,662 text/constant bytes, and the IOMMU object from
6829 to 6977 bytes. The native binding retains eight bytes of global storage.
The five-table scan covers 9216 words instead of 8192. QEMU verifies all CPU map
entries and permissions, sixteen pool-preparation translations, admission and
missing-map refusals, and the unchanged compact map.

These counts do not measure physical cache duration, DDR bandwidth, image
quality, receiver timing, or throughput. Every size and connector still requires
its actual electrical, settle/deskew, exclusion, guard, and recovery evidence.
