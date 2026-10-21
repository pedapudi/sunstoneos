# Native reference camera capture

The native camera binding prepares OV5640 exposures and copies the completed
640 × 480 NV16 image into an application-owned memory reservation. NV16 stores
307,200 luminance bytes followed by 307,200 interleaved chroma bytes in separate
planes. The sensor sends two-lane UYVY; the pass-through route and camera DMA
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
  words and parent clocks. Reserve TWI11 independently in the same registry.
- Authenticate camera-module supplies, bank voltages, pullups, analog trim,
  receiver timing, reset safety and independent recovery. Supply readback
  confirms programmed PMIC settings; it does not measure physical voltage.
- Admit the singleton memory pool through the actual boot memory contract.
  Qualify the four-kibibyte prefetch guards for this camera profile. A mapped
  guard does not itself establish the hardware's maximum prefetch extent.

CAM1 uses PK0–PK9 for CSI, PE5 for MCLK1, PE6 for power-down and PE7/PE8 for
TWI11. PE6 starts asserted. The connector carries no other module-control
signal, so the record names one of two module profiles:

- **Separate reset.** The record names an admitted reset GPIO outside PE5–PE8
  and PK0–PK9, initially asserted. The binding claims it as a fifteenth pin and
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
Each register access rechecks the media, TWI11 clock and rail leases. When the
clock registry or rail ledger reports BUSY, another task holds its metadata lock;
the binding then repeats only the status read until the boot deadline. Any other
result, or the deadline, faults the capture with all resources retained.
The constructor runs once during serialized primary memory preparation. All
referenced providers and leases remain private and valid for the image lifetime.

## Sensor bus and capture ownership

The bounded byte-transfer engine supports TWI11 at `0x0251b000` and S_TWI1 at
`0x07084000`. CAM1 uses seven-bit address `0x3c` and two-byte big-endian
register addresses. Reads and single writes carry one data byte; the sensor's
block writes carry up to 40 bytes to consecutive registers in one transaction. A
40-byte block takes about 4 ms at 100 kHz, within the 10 ms limit for one
transaction. The Type-C controller uses the same engine with one-byte register
addresses and its separate native provider. S_TWI1 does not use the extra PMU
registers documented only for S_TWI0.

The native TWI11 provider authenticates real clock, pin and rail leases. It
observes the leased APB1 source and divider, releases the initially asserted
local reset, opens the gate and selects a source-defined rate at or below
100 kHz. It excludes legacy interrupts, autonomous driver and DMA operation.
The common I2C mutex remains owned from START through acknowledged STOP and
final idle validation. NACK, arbitration loss, timeout or uncertain MMIO retains
the transaction without issuing STOP, resetting the controller or retrying.
Explicit cancellation has the same retained outcome and cannot unlock the bus.
Clock and rail authority is checked at every native bus access. Pin-manager
health is inspected under the common register mutex before acquiring I2C and
after releasing it. An in-flight transaction may reach that final boundary;
a pin-manager fault prevents its result from becoming available.

A 16 KiB-aligned parent reservation contains 32 KiB of IOMMU tables, 8 KiB of
parameter memory and two 311,296-byte plane allocations. Each plane includes
307,200 payload bytes and a 4 KiB guard. The 663,552-byte parent uses 162 pages
and one allocator record. Every address is an identity CPU/physical/IOVA address
below 4 GiB. These spans fit at most two of the IOMMU builder's four 4 MiB
windows. The native constructor refuses insufficient capacity; it does not
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
and leave SCAP clear. A separate short case covers S_TWI1 register width and
retained cancellation. Models do not qualify wiring, prefetch, timing,
coherence, reset, camera output or physical recovery.

Wider resolutions, continuous capture, the CAM2 connector, alternate sensors,
ISP processing, interrupt-driven service, buffer reclamation and a general
application service remain required hardware capabilities outside this reference
capture profile.
