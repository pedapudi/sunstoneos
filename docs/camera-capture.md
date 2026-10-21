# One camera capture runs the native controllers under one retained owner

The [capture coordinator](../drivers/camera/a733_camera_capture.c) connects the
OV5640 sensor, shared analog PHY, system IOMMU, pass-through route, local CSI
receiver and camera DMA. It captures one progressive 640×480 NV16 image through
camera port 0 or 1. The image has separate Y and interleaved UV planes, each
with a 640-byte stride and 307,200-byte payload.

The implementation passes a composition model using all six native controllers
and an AArch64 compile. It has not run on the board. Physical sensor/module
wiring, clock/power/pin providers, actual cache maintenance, prefetch guards,
independent transaction drain and recovery qualification remain required.
External admission callbacks represent those obligations; a successful model
callback does not qualify its future hardware implementation.

## The global lease remains held after image delivery

The [configuration](../include/a733_camera_capture.h) supplies CPU, physical and
I/O addresses for three exclusively reserved page spans: VIPP parameters, Y
and UV. The parameter span is readable by the device; the image spans are
writable. Each span includes any platform-required prefetch guards. The
coordinator refuses overlapping CPU aliases, and the IOMMU builder refuses
overlapping physical or I/O spans and unsupported page-table layouts.

Initialization constructs the real IOMMU tables in a caller-reserved 32 KiB
arena aligned to 16 KiB. It performs no MMIO. The caller must already own the
CPU aliases while this construction runs. Global acquisition subsequently
authenticates their physical identity, DMA permissions and cache isolation.
All I/O spans must end below 4 GiB, matching the current camera allocation
policy. The native IOMMU controller then publishes and installs those tables.

The global owner reserves both IOMMU banks and all ten masters, the common
analog registers, local camera blocks, sensor resources and all memory spans.
Every native controller acquires a sublease of that owner. Both IOMMU banks'
clocks and every master's power-domain accessibility remain held through the
final fault scan, which reads all real masters' diagnostic registers.

After successful capture, local route, receiver, sensor and DMA subleases close.
The global lease, analog lease, IOMMU lease, mappings, table arena and copied
mapping metadata remain owned. Table contents and metadata stay immutable.
`READY` transfers the image payloads to the caller; it does not authorize reuse
of their mappings, destruction of the coordinator or release of global resources.
Only the retained readout below can capture another image. Disposal and
shared-master admission need separate implementations.

This initial IOMMU profile excludes the other nine masters even after image
delivery. Those masters include USB and the display engine. Production camera
capture alongside those devices therefore requires a broader shared-master
mapping and ownership policy.

## Two explicit handoffs precede sensor packet enable

Analog initialization requires quiet sensors and exclusive common/local PHY
ownership. Its `ACTIVE` result alone cannot authorize another controller.
The capture coordinator uses these preparation steps:

1. Acquire the global lease, then execute cold analog preparation and native
   IOMMU installation.
2. Open the OV5640 in its verified nonstreaming VGA mode. Prepare the native
   ISP1 pass-through route and VIPP0 parameters, then arm DMA0.
3. Authenticate the global, analog and IOMMU tokens with the local-path handoff.
   Only this owner's quiescent receiver may then program its local PHY,
   packet port and parser. Common-register writers remain excluded.
4. Execute native receiver preparation in SCAP mode, which captures one frame.
   Authenticate the assembled receiver, route, DMA and mapping leases with the
   packet handoff.
5. Let the native sensor driver validate its receiver capability and enable
   MIPI packets. The DMA start callback accepts the actual streaming sensor
   state; it performs no independent sensor start.

Both handoffs can remain pending under the same absolute deadline. Their
callbacks authenticate authority and prerequisites; they must not change
registers or enable a producer. The leaf controllers perform those operations.
The analog owner retains its token and common-register exclusion while
delegating only the selected child's local writes and sensor enable.

The receiver, route and DMA configurations derive from the same selected port
and mapping spans. This profile uses parser 0/1 channel 0, ISP1 pass-through
channel 0, VIPP0 and DMA0. It does not enable ISP image processing or tune it.
The receiver's settle/deskew counts and 324 MHz top clock remain explicitly
admitted values. The sensor retains its existing 24 MHz input-clock requirement.

## Frame completion precedes shutdown, drain and CPU visibility

The coordinator waits for the native DMA controller to observe `CAPTURE_DONE`.
`FRAME_DONE` alone cannot advance the transaction. On completion, the native
sensor driver stops packets and verifies its control registers. A separate
packet-quiescence callback must prove that no in-flight ingress can change
receiver status; sensor register readback alone is insufficient.

The native CSI controller then samples and checks packet/parser errors. A
native parser read must show every capture-enable field clear, confirming the
one-frame SCAP fence. Only then can the DMA controller disable capture.
The receiver is never automatically rearmed.

While DMA waits for drain, the coordinator finishes the native VIPP route,
closes the local receiver and closes the sensor. Each component retains its
existing drain and local-release requirements. Their release providers must
preserve resources still held by the global owner, including MMIO clocks,
common analog power and all IOMMU mappings.

An independent DMA drain operation must prove a complete undamaged frame,
retired image writes and prefetches, and retired page-table walks. It runs
after native path shutdown. The coordinator then executes the native IOMMU
fault scan across both banks and all ten masters. A clean diagnostic scan
does not substitute for that preceding drain proof.

Finally, DMA performs its own final status and inactive-state checks, then
calls the real cache invalidation/acquire-barrier provider. Its local lease
closes before the coordinator returns `READY`. The leaf DMA controller's caller
is this coordinator; the application receives ownership only when the outer
capture operation returns success. No buffer is returned on an overdue or
uncertain completion.

## Retained images are rearmed with the sensor stopped

`a733_camera_capture_open_retained` ends in `READABLE`: the sensor is stopped,
the parser's single-image capture (SCAP) has cleared itself, and DMA capture
stays enabled with every lease and mapping held. `a733_camera_capture_rearm`
captures the next image into the same planes. Calling it declares that every
view and copy of the current image has ended. The sensor stays stopped until
the next packet handoff, and the rearm runs these steps:

1. Wait 100 ms after the receiver status sample that followed the sensor stop.
   This is three periods of the nominal 30 frame/s mode, so a frame the
   sensor finishes after its stop readback ends inside the interval.
2. Sample receiver status again. Errors fault; other flags are acknowledged.
3. Rearm DMA. Its status must still equal the final observation of the
   previous image; only those bits are acknowledged. Status must then read
   clear, capture must be inactive and the channel must still be enabled. The
   DMA admission callback runs again with the retained token to prepare the
   cache for device writes.
4. Wait another 100 ms after the second sample.
5. Rearm the receiver. Packet-port and parser status must hold no flag at all,
   which shows that no packet arrived during the interval, and every parser
   capture field must read clear. SCAP is then written and read back.

The packet handoff, sensor start and the complete retained readout then run as
for the first image. Rearm needs no drain proof because ownership never leaves
the device and the tables and mappings stay unchanged. The retained readout
already relies on DMA capture completion meaning that the image writes have
landed; the rearm adds two silent intervals of at least 100 ms each before the
same memory is written again.

## Faults retain resources without automatic rollback

Each poll executes at most one external admission, MMIO, I2C, cache or release
operation, plus the coordinator's and active leaf's monotonic clock checks.
Native polling and explicit state checks determine progress. Callback contexts
and controller objects must remain private, live and serialized.

Any leaf fault, refused handoff, changed acquisition token, deadline expiry,
clock regression, callback reentry or uncertain access ends in `FAULT`.
The coordinator performs no further operations, including attempted cleanup.
Automatic rollback would be unsafe after an uncertain packet enable or DMA
write. Recovery must resolve the retained global resources independently.

Local releases that already completed remain completed. For example, if the
DMA release succeeds after its deadline, its token stays cleared while the
coordinator faults and retains the global mapping lease. That outcome does not
authorize table reclamation or publication of a successful frame.

## Composition evidence reuses the leaf checks

The composition model runs the existing OV5640, analog, IOMMU, route, receiver
and DMA implementations against shared register state. It executes native
table construction and installation, verifies mapping permissions, and models
separate device and CPU image storage. Both camera ports complete one capture;
an offered second frame cannot overwrite memory after SCAP clears.

A second model case captures three retained images on each port, each with
its own payload. It checks that the previous image is unchanged until the DMA
rearm, that DMA acknowledges only the observed status, and that SCAP is
rewritten only after both quiet intervals, with clear status and the sensor
stopped. A fault at the receiver sample, DMA rearm or receiver rearm keeps
every lease and the previous image.

The model compares every Y and UV payload byte after synchronization and
checks untouched guard bytes. CPU storage retains its prior contents until
the cache callback. The synthetic extra guard page in this model tests span
ownership; it does not establish the hardware's required prefetch extent.
The generated bytes test delivery and ownership, not optical capture quality
or the hardware's UYVY-to-NV16 conversion.

Focused failures cover pending/refused handoffs, uncertain sensor enable,
frame-done without capture-done, an uncleared SCAP fence, CSI/VIPP/IOMMU errors,
drain failure, cache failure and late release. The initial composition batch
passed AddressSanitizer and UndefinedBehaviorSanitizer. A final publication
check also passed after strengthening the coordinator's reentry guard; earlier
composition evidence remains preserved. Existing leaf suites were not rerun.

The final AArch64 coordinator object contains 6,160 bytes of code/read-only
data and no mutable globals. Compiler reports show direct stack frames of
560 bytes for initialization and 48 bytes for polling. Stack provisioning must
also include the called leaf controllers and platform callbacks. These are
static build facts, not performance measurements.

Exact commands, inputs, executable artifacts and snapshots are retained under
`~/.cache/sunstoneos/evidence/camera-capture-20261016/`; the `publication/`
subdirectory contains the final changed-behavior check and native object.
The analog ownership clarification changes comments and prose only. Its
existing execution evidence remains valid. This composition adds no external
source dependency or physical qualification evidence.
