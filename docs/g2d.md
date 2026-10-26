# Native G2D fill, copy, blending, scaling and RGB order conversion

The native FreeRTOS application implements one retained fill, source-copy,
source-over-solid, twofold enlargement, general scaling or image-over-image
operation per boot. Copy and general scaling may also convert between 32-bit
RGB channel orders. This document assumes familiarity with DMA,
IOMMU translation and the supervised A733 launch contract. Software
implementation does not establish physical pixel correctness or safe resource
reclamation. Board testing is blocked while physical recovery is unavailable.

The caller supplies width, height, byte stride, operation, destination rectangle,
source origin and fill color in the sealed 904-byte session. Eight further
words carry the general-scaling source size (`sw`, `sh`), the image-over-image
background origin and plane alpha (`bx`, `by`, `alpha`), the output format and
two reserved words. Each word an operation does not use must be zero, so every
earlier session keeps its meaning. Width is at most
1280, height at most 720 and stride at most 5120 bytes. The stride must cover
all pixels and be divisible by four. ARGB8888 bytes are B, G, R, A in memory.
Fill requires a zero source origin; copy requires a zero fill color. Source-over-solid
uses straight per-pixel source alpha over the opaque ARGB color in `color`; its
alpha byte must be 255. It uses the same bounded source origin and rectangle as
copy. Twofold enlargement (`scale2x`, operation4) requires positive even
width and height for the destination rectangle and a zero color. Its source
crop has half the destination width and height at the supplied source origin.
General scaling (`scale`, operation 5) reads an `sw` by `sh` source crop at the
source origin and writes any other destination size, enlarging or reducing.
An identical size is refused, because the copy operation already covers it.
Image-over-image blending (`source-over-image`, operation 6) places a foreground
rectangle at the source origin over a background rectangle of the same size at
(`bx`, `by`) in the same upload. Foreground opacity is its per-pixel alpha times
plane alpha 1 to 255; the background keeps its own per-pixel alpha, so either
image may be translucent. Its color must be zero.

`format` describes the source and `output_format` the destination, using the
pinned `sunxi-g2d.h` codes 0 to 7: ARGB8888, ABGR8888, RGBA8888, BGRA8888,
XRGB8888, XBGR8888, RGBX8888 and BGRX8888. Each name lists channels from the
most significant byte of a little-endian 32-bit word. Only copy and general
scaling accept codes other than ARGB8888. Every order keeps four bytes per
pixel, so buffer sizes and mappings do not change.

Bounds checks use subtraction before computing offsets. Rotation and mirroring,
YUV color-space conversion, formats with other pixel sizes and a translucent
solid background remain required. The pinned source set does not include the
G2D rotation unit's driver, `g2d_rotate.c`, so rotation needs that source pinned
before an adaptation.

The caller uploads stride times height bytes, including row padding. A SHA256
digest authenticates the complete source even for fill. The destination starts
at zero, including padding. G2D writes only the selected destination rectangle.
A CPU copy never produces the operation's output pixels.

## Resources remain owned through every outcome

The existing board composition owns PMIC and USB services. G2D borrows the
shared PCK provider and an enabled DCDC2 rail at 800 mV. It reserves the common
fabric, oscillator, PLL reference and PERI0 parent, plus exclusive DE_SYS and
IOMMU ownership. The global IOMMU exclusion also reserves the other native
media master clock scopes against concurrent consumers.

The supported module clock selects the PERI0 300 MHz output undivided, for
300 MHz, the rate the vendor device tree assigns to G2D (`sun60iw2p1.dtsi`
lines 3383–3384). Native code validates the normalized 24 MHz PLL reference, parent
integer ratio, output gates, immutable fabric selectors and DE_SYS bus gates.
Before local clock or IOMMU access, a fixed power provider accepts domain1 only
when its command and status agree at OFF0 or ON8. An inherited ON state requires
no domain write. For OFF, it writes the five published DE_SYS delay values and
requests ON8 while preserving unrelated command bits. Status must reach ON8
within 10 ms and the original session deadline. Neither bound is extended by
polling or lock contention. The sequence follows the pinned Allwinner PCK600
driver; it does not reset the PCK controller or request poweroff.

The provider authenticates the actual G2D task, retained clock/PCK tokens and
readonly DCDC2 lease before taking the common register mutex. The complete
delay/request transaction holds that mutex; status polls release it after clean
observations. Failure while held retains the mutex, and every failure retains
all provider leases. READY is required before local G2D or IOMMU access. Domain
ON readback establishes no inherited traffic drain or electrical qualification.
The sealed initial authority must exclude and drain every DE_SYS consumer,
including DE, deinterlace, G2D and EINK, throughout the retained session.

After the provider reaches READY, native code deasserts the DE_SYS and G2D
resets, configures the disabled
module and enables its bus and module gates under the shared register mutex.
An uncertain register write retains that mutex. The sealed initial evidence
must establish independent drain and exclusion before these accesses.

Both IOMMU banks require global initial exclusion of all ten real masters.
Only master10 translates and only its micro fault registers are accessed.
Skipped masters remain excluded for the complete session. The initial proof
includes resolved inherited mappings, completed transactions, stable clocks,
exclusive RAM and retention until confirmed whole-board power removal.
Disabled registers or empty fault status do not establish this proof.

| Allocation | Size and alignment | Device address and permission |
| --- | --- | --- |
| IOMMU table arena | 32768 bytes, aligned to 16384 | Physical table base; never a payload mapping |
| Source | Page-rounded stride times height | `0x10000000`, read only |
| Destination | Page-rounded stride times height | `0x10400000`, read/write |
| RCQ commands | 4096 bytes | `0x10800000`, read only |
| Uploaded source and immutable output | Two private page-rounded write-back allocations | CPU only |

Each uncached source, destination and command allocation has an unmapped
4096-byte guard before and after its device-visible extent. Bytes after the
logical source and destination lengths retain a sentinel. Every uncached extent must have identical CPU and physical
addresses before pointer-derived mappings are accepted. The table arena has
its own complete allocation. The extended memory provider supplies every
allocation; physical addresses come from its actual leases. Table mappings
cover at most three distinct 4 MiB windows and never map the guards.

## Completion precedes the immutable copy

The register-command queue (RCQ) contains eight headers. It configures video
layer0, disables unused UI layers and writes the bounded destination rectangle.
Fill, copy, twofold enlargement and general scaling select blender COPY. Enlargement enables the
video scaler with the full 1152-byte register block inside the existing RCQ
page. It follows the pinned RGB path: half-step coordinates, zero phases, the
upscaling horizontal coefficient bank and linear vertical coefficients. The
source crop remains at most 640 by 360 pixels, so coarse downsampling is disabled.
General scaling uses the same RGB scaler path at the ratio of source to
destination size. The pinned Lanczos bank for that ratio filters horizontally
and the linear bank vertically. A source at least eight times wider or four
times taller than the destination is first decimated by the video layer, as
the pinned `g2d_ovl_v_calc_coarse` does, to eight times the destination width
or four times its height. Twofold enlargement is general scaling at exactly
half the destination size and emits identical commands.
Other operations disable the scaler. Source-over-solid enables UI2 in
constant-color mode as pipe1, with uploaded V0 foreground in pipe0, and selects
Porter-Duff SRCOVER. UI2 adds no buffer reads or IOMMU mapping. Both inputs and
the output use the effective upstream straight-alpha settings; the uploaded
source retains its per-pixel alpha rather than a substituted global alpha.
Image-over-image uses the same SRCOVER blend with UI2 reading the background
rectangle through the existing read-only source mapping. Video layer0 uses the
pinned mixer alpha mode, which multiplies per-pixel and plane alpha. A zero-dirty final header preserves the
hardware's even-header requirement. GPL source provenance and preserved
notices are recorded in [the source manifest](../apps/g2d/sources.json).

Submission clears stale task and configuration status, verifies their absence,
records the frame counter and enables one RCQ task. Success requires a fresh
task-end transition after submission. The frame counter remains diagnostic;
software does not infer undocumented increment semantics.

Normal completion follows the effective pinned Linux success path. The native
application latches the observed task-end status, acknowledges only task-end with
a write of one, and verifies that it cleared. It requires the mixer AHB reset
bit to be deasserted, pulses that bit from zero to one, and verifies both writes
while preserving the rotation reset and neighboring bits. An unexpected asserted
reset or uncertain write causes terminal failure with retained resources.

Only the completed sequence permits output reads. A non-destructive IOMMU fault
scan follows the reset and each bounded output-copy chunk. Guard checks, the
captured reset word and a cleared task-end status surround every chunk. The
exported completion status retains the pre-acknowledgment observation. The final
copied digest and source digest are checked before terminal success publication.

The pinned IRQ path performs this acknowledgment and reset before signaling
completion, including for retained `TASK_APPLY` operations. Its source does not
establish that reset drains writes or permits safe buffer reuse. Physical output
visibility remains unqualified. Linux timeout recovery is not a drain guarantee;
all native outcomes retain memory, mappings and device resources.

USB exports the immutable private copy. Its 192-byte header binds the full
sealed session digest, source digest, output digest, dimensions, operation,
completion status, master10 fault-scan mask and guard result. Query is byte0.
ACK is byte1 followed by the generation as LE64 and the output SHA256 digest.
The response joins header and payload into one packet stream. The transmit
callback refills the FIFO before TinyUSB considers an intermediate zero-length
packet. A response whose length is divisible by the negotiated endpoint packet
size ends with one zero-length packet. The host consumes that packet before ACK.
ACK is accepted only after actual completion of the entire output transfer,
including its terminal zero-length packet when required.
A final query returns acknowledged status with the same proof and no payload.
ACK, timeout and task completion release no memory, mappings, clocks or power.
The shared session deadline bounds upload, setup, submission, copying and USB.

The host source-over reference computes each RGB channel as
`(source * alpha + background * (255 - alpha) + 127) // 255` and sets output alpha
to 255. This is an ideal nearest-integer expectation. The pinned setters establish
routing and controls, but do not establish device quantization. For blending, agreement or disagreement is recorded as an unqualified
diagnostic and does not gate ACK. Authentication, completion and durable output
checks remain strict. Modeled output cannot qualify physical arithmetic. The UAPI SRCOVER/DSTOVER formula comments conflict with the
executed source and are not used as an oracle.

For image-over-image, the host computes ideal straight-alpha source-over with
foreground alpha `Af = As * plane / 255²` and background alpha `Ab = Ab8 / 255`,
where `As` and `Ab8` are the 8-bit pixel alphas. Output alpha is
`Ao = Af + Ab * (1 - Af)`, and each color channel is
`(Cs * Af + Cb * Ab * (1 - Af)) / Ao`, rounded to nearest; a zero `Ao` gives a
zero pixel. A converting copy reorders channels and treats an X channel as 255
on input and output. Both references are unqualified diagnostics, like
source-over-solid, and do not gate ACK. General scaling has no numerical oracle;
the host requires only zero bytes outside the destination rectangle.

Physical qualification must compare fill, copy and source-over output, including arbitrary
source alpha, nonzero origins and untouched padding, with the CPU reference.
It must also establish actual MMIO and DMA
reachability, parent stability and retained failure handling. INTID83 is
identified by the pinned device tree; this first path polls its enabled RCQ
status while retaining interrupt routing exclusion. Interrupt delivery remains
unqualified.

The image with DE_SYS initialization has SHA256
`819165376f652c8353dc53aebee9d9c842d44957ea806c2e9bb0cbaac428e0c5`.
Evidence under `g2d-power-20261020/` records five focused cases on that image.
The linked task passes coherent OFF-to-ON initialization followed by RCQ source
copy, and adopts coherent ON without domain writes. Initial incoherence, status
timeout and uncertainty after the first delay write produce retained terminal
failures without submitting G2D work. The timeout keeps its original request
bound, and clean pending polls release the shared register mutex.

These cases use fresh function-call boundaries at EL1t. PMIC read-service
results, task identity, counter, Device translations, mutexes and peripheral
effects are modeled. They establish neither actual scheduling nor SMP behavior.
Every stopped case preserves CPU context, RAM, MMIO and model state. Root's
static comparison confirms unchanged boot object sections and symbolic
relocations; accepted startup checks retain their original scope.

Broader unchanged evidence is preserved under `g2d-native-20261020/` for image
SHA256 `4e043dad7e691ad50c4164dd9085ae118a0e0911d7fd8aea93e16610e7966328`.
Its frozen closure contains 169 linked objects and 448 compiled dependencies.
Focused source review covers RCQ layout, DMA ownership, clock fields, retained
IOMMU admission and TinyUSB packet completion.

The actual linked allocator, pool getters, upload constructor, G2D constructor
and IOMMU validator pass a model-only 64 by 64 source-copy fixture. The check
confirms exact source bytes, a zero destination, three bounded permission spans
and intact guards. Separate cases refuse differing CPU/physical addresses and
corrupted guards while retaining allocations. Prepared-pool authority and time
are modeled; no peripheral MMIO is mapped. Host execution also checks actual
RCQ fields, fragmented upload authentication and master10-only IOMMU programming
and fault scans. These checks establish software behavior under their stated
models. Physical G2D, USB and interrupt operation remain unqualified.

Twelve saved task cases execute that image's linked AArch64 task, clock and rail
registries, GPU resource manager, native clock accesses, IOMMU and copied-output
logic. They cover fresh completion, stale status refusal, global and master10
faults, configuration-only timeout, failure after copying, unchanged terminal
polling and two 32768-byte copy chunks. Separate fill and copy cases interpret
the actual emitted RCQ and mixer words with an ARGB8888 hardware-effect model,
then compare copied output with the sealed caller reference. Every trace excludes
other masters' micro registers. PMIC service results, scheduler mutexes,
translation observations and hardware effects remain explicit model boundaries.

The transport check reuses the terminal copied checkpoint. Actual configured
TinyUSB vendor, FIFO and endpoint code passes full-speed and high-speed streams
and an exact high-speed packet-length case with modeled DCD completions.
The raw constructor table, task checkpoints and case records are preserved
beside the frozen image. These models do not establish physical pixels,
interrupt delivery, USB operation or recovery.


The source-over image has SHA256
`b27476caef23ad7cd378552efcc5a919420c8e801f6f658466290d6b1bbfe710`, with evidence
under `g2d-blend-20261020/`. A source comparison check matches the complete UI2
and blender register images against unchanged pinned Linux setters under
ASan/UBSan. One linked native job interprets emitted RCQ words with explicit
ideal blending effects, then checks the retained copy, full session binding,
master10-only scans and guards. Its source origin and destination origin differ,
and a 60-pixel image with 256-byte stride exercises untouched row padding.

Two changed SRAM entry cases admit this operation and refuse a nonopaque
background. The changed inline geometry predicate requires the new launcher;
its source and binary are retained. Root confirms all twelve boot-object
sections and symbolic relocations, generated boot source and FreeRTOS port
source match the accepted power image. Existing boot and TinyUSB evidence keeps
its recorded scope without replay. The operation model consumes root-published
contract bytes as supplied state at the actual private and live record addresses;
it does not execute startup or establish cache publication. New host cases use
the saved native copied bytes with the identical sealed session and check ideal
reference-difference reporting, durable capture and ACK refusal on a corrupted
output digest.


The normal-completion image has SHA256
`01b71b3f3248f8f7e5dc6ace268abaa70e249933e3fad7084bf83058cb01e1b9`, with evidence
under `g2d-success-20261020/`. Six linked native completion cases restore one
submitted-job checkpoint. Normal completion acknowledges task-end, preserves
configuration status during acknowledgment, pulses only mixer reset and copies
16384 bytes. Stuck acknowledgment, an unexpectedly asserted reset, uncertainty
after either reset write and a later task-end each return native failure without
reading or copying output pixels. Every case retains its DMA extents. The four
failures within the completion sequence leave completion readiness clear.

The check reuses accepted RCQ words and modeled pixels as data. It supplies the
root-derived runtime contract at the actual private and live record addresses;
prepared pools, scheduler services and device effects remain modeled. It does
not execute startup, requalify pixel arithmetic or prove physical write
visibility. The earlier blend model omitted the normal-success reset and retains
only that earlier implementation's scope. The unchanged launcher binary and
prior boot and transport evidence retain their recorded scopes without replay.


The twofold-enlargement image has SHA256
`f77be8b28ef7283658c4d5e80bff37392a3cd90c6b6033703b2caa0881522ce4`, with evidence
under `g2d-scale-20261020/`. A sanitizer check compares complete video-overlay,
scaler and writeback register images with the pinned setters at minimum,
asymmetric and maximum geometry. It also refuses odd destination dimensions,
source half-extent overflow and nonzero color.

One linked native job consumes the final producer session and root-published
runtime contract. Its RCQ bytes match the source comparison artifact; actual
acknowledgment, reset, IOMMU scans and guarded copy retain 16384 bytes. Device
pixels are explicitly injected tagged data confined to the destination rectangle.
They establish no interpolation, filtered-alpha, edge-handling or rounding rule.
Host capture checks authenticated output and exact zero bytes outside the target
rectangle, including padding, without comparing interpolated pixels.


One high-speed transport case restores that exact copied checkpoint and executes
the linked C serializer and TinyUSB vendor, FIFO and endpoint code. It emits the
16576-byte authenticated response and final receipt while gating ACK processing
on completed transfers. DCD effects, provider readiness and scheduling are
modeled; no G2D job, full-speed matrix or earlier completion cases replay.


## General scaling, image-over-image and RGB order conversion checks

These operations write the RCQ fields named by the pinned setters. Video
layer0 carries the source format in `lay_fbfmt`, writeback carries the output
format in `fmt`, and the blender alpha mode stays 0, as the pinned
`bld_cs_set` selects for every RGB format. No buffer, mapping, clock, power or
completion step changes. Rebuilt image SHA-256
`08e56fc59dca1f9f148dc217649e78453944c9a7a81a143cd5b31a47ff30351b` passes four
focused checks. None executes a G2D job or establishes physical pixels.

- `checks/g2d-modes-reference.py` runs unchanged pinned scaler, overlay,
  blender and writeback setters under ASan/UBSan beside the native builder. It
  matches 1369 scaling cases, including enlargement, reduction, mixed axes
  and both decimation paths, all 64 format pairs for copy and scaling, and four
  image-over-image geometries. Given `--previous f99e356`, it compiles the
  builder from that revision and confirms identical commands for earlier fill,
  copy, source-over-solid and twofold enlargement sessions. Twenty-nine refusal
  cases and ten admitted cases also run through host preparation, which agrees
  on every verdict.
- `checks/g2d-modes-native.py` executes the linked AArch64 session validator
  and RCQ builder on those 39 cases. Every verdict matches, and the ten admitted
  command pages equal the host builder's bytes.
- `checks/g2d-modes-launcher.py` runs the rebuilt 3008-byte SRAM launcher in
  the shared AArch32 model inside an earlier prepared bundle. It admits four new
  operation sessions and refuses eight invalid ones. The launcher's geometry
  predicate reads each session word once and checks each axis with one compare,
  which keeps it within the 3072-byte SRAM region. The earlier launcher cases
  pass with the rebuilt binary.
- `checks/g2d-modes-host.py` seals new sessions with the actual producer from an
  accepted earlier session's board prefix. It checks seven refusals, the ideal
  references and capture policy: blending and conversion differences still
  receive ACK, while a differing plain copy and a scaled write outside the
  destination rectangle are refused before ACK. Response frames are synthetic.

The earlier scaling and blending register checks and host capture checks pass
with the changed code. Physical pixels, interpolation, X-channel handling,
alpha rounding, DMA visibility and performance remain unqualified.
