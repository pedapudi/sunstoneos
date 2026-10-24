# One camera capture runs the native controllers under one retained owner

The [capture coordinator](../drivers/camera/a733_camera_capture.c) connects the
OV5640 sensor, shared analog PHY, system IOMMU, pass-through route, local CSI
receiver and camera DMA. It captures progressive NV16 images through camera
port 0 or 1, either one still image at a time or continuously into a ring of
frame slots. An NV16 image has separate Y and interleaved UV planes, each with
a stride equal to the width and a width × height payload: 307,200 bytes at
640 × 480.

The implementation passes a composition model using all six native controllers
and an AArch64 compile. It has not run on the board. Physical sensor/module
wiring, clock/power/pin providers, actual cache maintenance, prefetch guards,
independent transaction drain and recovery qualification remain required.
External admission callbacks represent those obligations; a successful model
callback does not qualify its future hardware implementation.

## The sensor mode sets every controller's frame size

The configuration carries an OV5640 mode from `ov5640_mode_get` in UYVY or
YUYV; the coordinator refuses the raw format. Initialization selects that mode
in the sensor driver and passes its width, height and byte order to three
controllers:

- The receiver programs the packet-port and parser byte order and the parser
  output size.
- The route writes the VIPP output and crop sizes.
- The DMA programs the output size, strides and flip size.

Each image span must cover one width × height plane. The reference checks of
the [route](camera-route.md) and the [DMA](camera-dma.md) compare these register
and parameter images for further sizes in both byte orders.

The stopped-producer interval of the retained rearm sequence is three frame
periods of the mode the sensor driver derived, rounded up to a whole
microsecond, and at least 100 ms. A mode given with frame length 0 uses the
reference frame length, so its interval follows that length. Three periods of
the 30 frame/s VGA mode equal 100 ms; the 15 frame/s reference QSXGA mode needs
199,893 µs.

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
Every I/O span must end at or below `A733_IOMMU_IOVA_LIMIT` (4 GiB). Physical
spans may lie anywhere in DRAM below 2^34, which the IOMMU entries encode. The
native IOMMU controller then publishes and installs those tables.

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
`FRAME_DONE` alone cannot advance the transaction. On completion, the
coordinator first reads packet-port status (`0x118`) and parser status (`0x44`)
and records both words in `frame_status` without acknowledging them. The native
sensor driver then stops packets and verifies its control registers. The stop
writes `0x300e` before `0x4202`, the order used by the Linux reference driver.

The recorded words are diagnostics. They do not change which captures fault.
When the later sample faults on an error bit that the record lacks, the error
arrived while the sensor was stopping, after the captured frame had ended.
Whether the OV5640 stop can truncate a packet on this receiver is not known
from hardware; this record supplies that evidence from a board run.

In the close path, a separate packet-quiescence callback must then prove that
no in-flight ingress can change receiver status; sensor register readback
alone is insufficient. The retained readout described below omits this
callback. Its receiver stays armed after the parser read confirms that SCAP has
cleared, so the parser accepts no later frame. Before SCAP is written again,
the rearm sequence requires two stopped-sensor intervals and status with no
flag set.

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

1. Wait one quiet interval after the receiver status sample that followed the
   sensor stop. The interval is three frame periods and at least 100 ms, so a
   frame the sensor finishes after its stop readback ends inside it.
2. Sample receiver status again. Errors fault; other flags are acknowledged.
3. Rearm DMA. Its status must still equal the final observation of the
   previous image; only those bits are acknowledged. Status must then read
   clear, capture must be inactive and the channel must still be enabled. The
   DMA admission callback runs again with the retained token to prepare the
   cache for device writes.
4. Wait another quiet interval after the second sample.
5. Rearm the receiver. Packet-port and parser status must hold no flag at all,
   which shows that no packet arrived during the interval, and every parser
   capture field must read clear. SCAP is then written and read back.

The packet handoff, sensor start and the complete retained readout then run as
for the first image. Rearm needs no drain proof because ownership never leaves
the device and the tables and mappings stay unchanged. The retained readout
already relies on DMA capture completion meaning that the image writes have
landed; the rearm adds two silent intervals of at least 100 ms each before the
same memory is written again.

## Continuous capture delivers complete ring slots

A configuration with `slots` set to 2, 3 or 4 divides both image spans into
that many frame slots of `slot_bytes` each. `slot_bytes` is a multiple of 4096
that covers one plane, so no cache line holds bytes of two slots, and the
slots must fit both spans. `a733_camera_capture_open_stream` runs the same
preparation as a still image, except that the receiver uses continuous video
capture (`VCAP`) and the DMA enters its ring mode just before the sensor
starts. The coordinator returns 1 from each poll once the sensor has started,
and each poll performs one DMA operation.

The absolute deadline given to `a733_camera_capture_open_stream` bounds the
preparation, including admissions and handoffs that stay pending. It then
bounds the stream: at that deadline the coordinator faults and keeps every
lease. `a733_camera_capture_renew`, valid only in `STREAMING`, replaces the
coordinator's and the DMA's deadline with a later one and performs no I/O. An
owner keeps a short watchdog, for example three frame periods ahead, and
renews it while it keeps polling. A short open deadline therefore still fails
a hung preparation in bounded time, and a stream lasts as long as its owner
renews it.

The DMA address registers latch at each frame start; the reference driver's
frame-start handler programs the buffer for the frame after next on this
assumption. On each observed frame start the DMA controller:

1. Makes the slot programmed at the previous start the active slot.
2. Delivers the previous active slot only if every frame started so far,
   that frame included, has had its frame-done flag observed. The controller
   counts the flags still owed: each observed start adds one, and each status
   read that shows frame done removes one. A frame-done flag seen in the same
   status read as a frame start belongs to a frame before that start. If any
   flag is still owed, the slot returns to the free pool unread and the frame
   counts as dropped. A frame-done flag that arrives after the next frame
   start therefore pays the debt of its own frame and cannot make a later,
   unfinished frame look complete. Two flags that merge into one status read
   leave one flag owed for the rest of the stream, so every later frame is
   dropped. The owner sees the drop count rise with nothing delivered. To
   recover, it stops the stream and then initializes a new coordinator, or
   the same object again from fresh zeroed storage, before opening another
   stream; a stopped coordinator cannot be reopened.
3. Programs the first free slot after the active one into both address
   registers. With no free slot, the registers keep naming the active slot;
   the next frame overwrites it, and the overwritten frame counts as dropped.
4. Reads status again. A frame start that arrived between the acknowledgement
   and this read may have preceded the address writes, in which case the
   active slot was latched again or the two planes were split between two
   slots. The controller then treats the active slot's contents as
   unattributable: it delivers nothing at this start or the next, and the
   programmed slot becomes active at the next start.

The owed count is exact only while every frame start appears in a status read
of its own. The coordinator sets the DMA status interval just under half the frame
period of the mode the sensor derived: half the period, less 1 % for sensor
clock tolerance and 2 µs for whole-microsecond clock readings. The QVGA mode's
33.3 ms period gives 16.5 ms. Consecutive DMA status reads are at most four
polls apart, plus one poll for each pending slot invalidation: after a frame
start the acknowledgement and two address writes come before the next read,
and an invalidation that is still pending delays the read. The owner must therefore poll more often than every eighth of a frame
period. If two status reads lie further apart than the interval, two frame
starts may have merged into one observation and the count may be short. A short count
would let a late flag pay for a frame the device is still writing. The DMA
then marks the count unknown, and from that point every frame is dropped, as
after merged flags, until the owner stops the stream. The DMA's first
interval starts with a clock reading taken just before the sensor start, when
no frame can have started.

The first interval also covers the sensor's stream-on sequence: two register
writes, their read-backs and the DMA start callback, about six owner polls
plus their I2C transfer times. If that exceeds the interval, the first read
marks the count unknown and the stream delivers nothing, although every slot
stays owned correctly. Whether a real OV5640 start fits is unmeasured. The
fix is to open the first interval at the stream-on write itself, which needs
a hook inside the sensor's start sequence; until a sensor is attached to
measure it, the stream can fail closed in this way.

While the owner polls within that bound, a delivered slot is never one that
either address register names or that any frame may still write: its
frame-done flag, and those of all earlier frames, were observed before
delivery. A slot freed without delivery stays
device-owned, so a late frame may still finish writing it. Before the caller sees it, the platform's
`sync_slot` operation invalidates the CPU cache over both of its planes. The
consumer takes delivered slots in capture order with their frame-start count,
reads the two planes through `a733_camera_capture_take`, and returns each slot
with `a733_camera_capture_give`. The ring records frame starts, delivered
frames and dropped frames; dropped frames equal starts minus delivered frames
minus the active frame. Frame starts merged during a polling gap are missing
from both the start and the drop counts. Any error flag faults and retains
every resource.

`a733_camera_capture_stop`, with every slot given back, stops the sensor,
waits for the packet-quiet proof, samples receiver status, clears the parser's
video capture fields and verifies the parser fence. The close path of a still
image then follows: DMA disable, route, receiver and sensor close, the
independent drain proof, the IOMMU fault scan, final DMA status without a
capture-done requirement and cache invalidation of both complete spans.
`READY` returns both spans to the caller.

Whether the address registers latch at frame start, and whether frame done
precedes the next frame start in every mode, needs a module on the board. The
1080p and QSXGA modes have only 24 lines of vertical blanking, so a frame-done
flag after the next start is most likely there; it drops frames.
The native application does not use continuous capture yet.

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

The AArch64 coordinator object with continuous capture contains 8,396 bytes of
code/read-only data and no mutable globals. Compiler reports show direct stack
frames of 608 bytes for initialization and 48 bytes for polling. Stack provisioning must
also include the called leaf controllers and platform callbacks. These are
static build facts, not performance measurements.

A separate continuous-capture model, `checks/a733-camera-stream.py`, runs the
same native controllers in a 320 × 240 mode with three slots. Its frame engine
latches the address registers at each modeled frame start, writes each frame's
own payload into the latched slots at frame end and raises the frame-done
flag. A late frame instead writes its last 64 bytes per plane and raises frame
done a set delay after the next frame start. The model checks that the device
writes only the programmed or active slot, and a late frame's last bytes only
a slot the CPU cannot read. It checks that every invalidated slot is named by
neither address register, the frame in progress nor a late frame, and that
every delivered frame is complete and matches its frame-start count. The cases
are:

- both ports and both byte orders, with every frame delivered;
- a consumer holding two of three slots, with the dropped frames counted;
- a frame start racing the first or the second address write, which drops two
  frames and keeps every later frame correct;
- one frame whose frame done arrives 3 ms after the next start, which drops
  that frame only;
- every frame's frame done arriving 3, 5 or 7 ms after the next start, which
  delivers nothing;
- one late frame done that merges with the next frame's flag in one status
  read, which drops every later frame;
- an owner that stalls after the fifth frame start until two more frames have
  started, with every frame done 3 ms late, which delivers nothing, and with
  frame done on time, which delivers the four frames before the stall and none
  after it;
- a stream renewed on every poll by three frame periods, which runs past its
  open deadline, refused renewals before `STREAMING` and for a reached
  deadline, and a stream without renewal, which faults at the open deadline
  and keeps every lease;
- a QSXGA mode given with frame length 0, whose quiet interval follows the
  derived reference frame length, and the VGA interval of 100 ms;
- missing frame-done flags, which deliver nothing, and an owed count set one
  below its ceiling, which saturates instead of wrapping to zero;
- an error flag and a failed invalidation, which fault and retain every lease;
- refused slot counts, slot sizes, spans and the raw format.

Removing the status read after the address writes, delivering without
a frame-done flag, delivering an unattributable slot, or delivering whenever
a frame-done flag followed the slot's start without counting owed flags each
makes the model fail. Computing the quiet interval from the given mode instead of
the derived one also fails it. The check script also builds the model with the
status-interval comparison removed and requires it to fail: the stalled owner
with late frame done then gets slots delivered while the device still writes
their last bytes. The model passes under AddressSanitizer and UndefinedBehaviorSanitizer.
Its results are under
`~/.cache/sunstoneos/build/audit-camera-streaming/dev/stream-check/`.

Exact commands, inputs, executable artifacts and snapshots are retained under
`~/.cache/sunstoneos/evidence/camera-capture-20261016/`; the `publication/`
subdirectory contains the final changed-behavior check and native object.
The analog ownership clarification changes comments and prose only. Its
existing execution evidence remains valid. This composition adds no external
source dependency or physical qualification evidence.
