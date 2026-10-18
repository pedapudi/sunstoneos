# Native reference camera capture

The native camera binding prepares one OV5640 exposure and copies its completed
640 × 480 NV16 image into an application-owned memory reservation. NV16 stores
307,200 luminance bytes followed by 307,200 interleaved chroma bytes in separate
planes. The sensor sends two-lane UYVY; the pass-through route and camera DMA
produce these planes without an ISP image-processing pipeline.

The implementation uses the existing clock registry, media controller, rail
ledger, queued PMIC provider, banked pin manager, system IOMMU and singleton
memory allocator. It executes the existing sensor, analog calibration, CSI,
route and DMA controllers. Physical operation is unqualified. A camera-specific
protected boot handoff and executable camera image remain integration requirements.

## Boot admission and wiring

The protected record in `a733_camera_native.h` binds the camera owner, image
build, boot epoch, generation, lease and deadline to reviewed physical evidence.
The record includes inherited pin configurations, actual reset wiring, supply
identities and programmed voltages, APB1 configuration, analog trim, receiver
timing and mapped guard requirements. Nonzero identifiers or matching fields
alone cannot establish the required authority.

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
TWI11. PE6 starts asserted. The module must also provide a separately admitted
reset GPIO, initially asserted; the implementation has no successful no-op
reset substitute. A module-specific wiring profile must resolve that connection.
Conflicting schematic supply labels prevent choosing camera rails from the
connector name alone. The binding observes three explicitly qualified PMIC
outputs and never changes their voltages.

One designated privileged FreeRTOS task advances capture. The existing PMIC
provider task must run independently to complete queued supply observations.
The constructor runs once during serialized primary memory preparation. All
referenced providers and leases remain private and valid for the image lifetime.

## Sensor bus and capture ownership

The bounded byte-transfer engine supports TWI11 at `0x0251b000` and S_TWI1 at
`0x07084000`. CAM1 uses seven-bit address `0x3c`, two-byte big-endian register
addresses and one data byte per transaction. The Type-C controller uses the
same engine with one-byte register addresses and its separate native provider.
S_TWI1 does not use the extra PMU registers documented only for S_TWI0.

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
below 4 GiB. These spans fit at most two of the IOMMU builder's four 4 MiB windows.
The native constructor refuses insufficient capacity; it does not shrink the mode.

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

Source buffers, IOMMU tables, clocks, rails and pins remain reserved. There is no
rearm or disposal operation for this profile. Completion permits access to this
still image; it does not prove that every system transaction and page walk has
drained. Existing close-and-release APIs retain their separate drain requirements.

## Sources and focused verification

The pinned A733 manual defines TWI0-compatible instances in §18.11.7, the
standard-mode divider in §18.11 and single-image parser behavior in §16.1.13.4.
The still-image capture-done description is in §16.1.14.36; its status register
is in §16.1.14.37. Both still-image branches of the pinned BSP's
`bsp/drivers/vin/vin-video/vin_core.c` publish a completed buffer on this event.
Existing camera component documents retain their register and table provenance.

The focused native composition fixture executes the actual resource managers
and camera controllers with modeled MMIO, task identity, cache visibility and
physical admission. It covers completed payload copying, bus arbitration loss,
DMA errors, incomplete single-image capture, cache failure, late clock-authority
failure, pin-manager failure and IOMMU faults. A separate short case covers S_TWI1 register width and
retained cancellation. Models do not qualify wiring, prefetch, timing, coherence,
reset, camera output or physical recovery.

Wider resolutions, continuous capture, alternate sensors, ISP processing,
interrupt-driven service, buffer reclamation and a general application service
remain required hardware capabilities outside this reference capture profile.
