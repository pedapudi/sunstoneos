# Retained native DisplayPort output

The native DisplayPort image targets the documented TYPEC24 route on the
Orange Pi Zero 3W/A733 V1.2 drawing. Four COMB0 lane pairs and AUX reach that
connector. The separate HPD pad is unconnected. Connection status therefore
comes from negotiated USB Power Delivery messages, including HPD level and
HPD events. Physical board revision, negotiation and visible output remain
unqualified.

The closed profile uses four DisplayPort lanes at 1.62 Gb/s, 1280×720 at
60 Hz, and eight-bit RGB transmission. It accepts Type-C pin assignment C or E.
A sink must advertise four lanes, the selected rate, and the complete 720p60
timing through a checked EDID base block or its first CTA extension. Unsupported
sinks fail while ownership remains retained. Audio, HDCP, MST, live frame
replacement and other rates are outside this initial profile.

The image composes the existing supervised board PMIC, USB0, clock, rail, pin,
PCK and register-lock providers. It holds COMB0, the shared SERDES lifecycle
controls, and VO1 exclusively before Type-C negotiation starts. USB3 and PCIe
cannot share that SERDES lifecycle during this initial profile. It subsequently
reserves the entire DE and excludes the other IOMMU masters. No lease is
released after a failure or successful output stop.

## The source supplies the controller and analog sequence

The [component manifest](../apps/displayport/sources.json) pins the Linux source
revision, source hashes and licenses. The Cadence adaptation retains the original
16-bit analog-register accesses, cold bootstrap, 1.62 Gb/s table, signal-level
tables and four-lane configuration. Every PHY readiness timeout becomes a
terminal failure. The exact-board lane inversion is enabled on all four lanes;
the lane map stays in identity order. The negotiated orientation controls the
published COMB0 direction field. An additional lane-map reversal would apply
orientation twice.

The Trilinear adaptation sends AUX requests with elapsed-time bounds, checks
reply identity and byte counts, and limits DEFER retries. It reads DPCD receiver
capabilities, checks EDID checksums, and completes clock recovery and channel
equalization for every lane before enabling pixels. The retained result includes
raw capability, EDID, training and controller observations. Injected sink replies
in a software model cannot establish a physical AUX transaction or trained link.

TV1 at `0x05731000` feeds the controller at `0x05740000`. The inherited device
tree selects DE route value 5 for TV1. The node name `edp0@5720000` does not
identify its register address. The source-defined controller clock controls are
at `0x05760000`; VO1 routing belongs to `0x05510000`.

The published analog table assumes a 26 MHz raw oscillator. The native profile
requires that decoded oscillator state, a stable normalized 24 MHz PLL reference,
and an adopted integer VIDEO1 output of 1188 MHz. Its divider supplies the
74.25 MHz pixel clock. Actual register readback checks consistency with the
immutable session. It does not measure frequency or qualify electrical behavior.
DCDC2 must read as enabled at 800 mV and CLDO5 at 1800 mV through the actual
PMIC provider. The physical admission record must establish their compatibility
and settling.

The AUX clock divider uses the qualified core APB host frequency supplied by the
session. The manual permits integer frequencies from 10 through 400 MHz. The
published driver hardcodes 200 MHz with a FIXME. Available sources do not
establish that this core input follows the global APB1 selector. An admission
record must establish the actual core host frequency and its stability; fabric
readback alone cannot supply that evidence.
The cached `dp-aux-host-clock-audit-20261020/result.json` records the six primary
source excerpts supporting this boundary. The manual's divider register describes
a programmed host frequency; the pixel-clock selector and gate do not establish
the internal AUX APB source.

## Memory and copied completion stay owned

Before scheduling, the actual uncached allocator returns one 2 MiB allocation
aligned to 16 KiB. CPU and physical addresses must match within the admitted
extended uncached pool. The first 32 KiB holds the IOMMU tables. One read-only
span maps the remaining allocation to display master 8 at IOVA `0x10000000`.
Pixels start 4096 bytes into that span, at IOVA `0x10001000`, with a 2560-byte
pitch. The eight colored bars, black border and diagonal marks come from CPU
writes before publication.

The leading pixel redzone and tail padding contain `0xa5`. These bytes remain
mapped; they are diagnostic redzones, without an unmapped guard claim. The image
checks those bytes and the immutable pixel digest after output stop. Initial
IOMMU installation still requires authenticated prior drain, resolved inherited
tables and exclusion of every master. Disabled hardware bits cannot establish
those facts.

A successful observation requires a newly cleared and subsequently observed DE
frame-end status, a healthy link and controller, a completed master-8 fault scan,
output stop, and confirmed Type-C shutdown. DP and TV1 stop before DE disable.
Frame status is observed only after the incremental IOMMU scan completes.
Normal completion requests shutdown from the Type-C task, which revokes at an
idle transaction boundary. Faults still revoke immediately. Read-only authority
checks retry metadata contention within the original deadline; device writes
and partial controller operations are never repeated by that retry.
The complete allocation, translations, supplies, clock and PCK ownership stay
reserved until power removal. Disabling output does not prove DMA drain.
An authority failure can prevent output stop; the terminal fault preserves the
partial observations and ownership.

USB0 serves a copied 1024-byte observation in response to query byte zero.
The terminal copy is immutable and includes the full session digest, build
identity, negotiated attachment, physical allocation, pixel digest and copied
352-byte controller result. The transfer includes the terminal zero-length
packet at full and high speed. Reading a reply grants no resource release.

## Software evidence has explicit model boundaries

The controller check executes the linked Cadence and Trilinear adaptations with
modeled PHY readiness, AUX completion and sink responses. Its six cases cover
nominal training, PHY timeout, repeated AUX deferral, short AUX reply, lost clock
recovery and immutable-configuration mutation. The constructor check executes
the actual allocator, page-table construction, pixel generation and digest
calculation from the linked image. Its retained allocation bytes allow the host
preparer to check every descriptor, pixel and padding byte without repeating
those constructors.

The negotiation check executes the native PMIC, Type-C, power and display task
bodies through real provider objects. It models the independent TWI buses and
USB Power Delivery partner. The composed check continues those task contexts
through the display application. Architectural scheduling, elapsed time,
register behavior, sink replies and frame-end events remain modeled.

The composed scheduler serializes tasks. Its optional nominal ordering lets a
task finish an existing register-lock operation before a tick-woken power
observer runs. Native counter reads and deadlines still execute. This ordering
can demonstrate device sequencing; it does not establish SMP scheduling,
priority inheritance, contention tolerance or physical service timing. A
preserved alternate ordering makes the power observer encounter a busy register
lock and conservatively shut down. Neither ordering qualifies physical output.
