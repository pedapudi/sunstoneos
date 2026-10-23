# Native offline deinterlacing

`sunstone-a733-deinterlace` accepts an authenticated fixed-geometry NV12
upload, submits one to four offline WEAVE, motion-adaptive or temporal noise
reduction (TNR) jobs to A733 DI301, and exposes the immutable copied outputs
through USB0. It retains the controller, buffers, IOMMU tables, clock/power
ownership and copied result until whole-board power removal. Successful
output delivery does not authorize another session or resource reuse.

The first path supports combined fields, top field first, with 640-byte Y and UV
strides. The Y plane contains 480 lines and the UV plane 240 lines, totaling
460,800 bytes. Alternating lines belong to alternating fields. Deterministic
field patterns and caller-supplied frames are valid inputs; no media permission
or preknown output oracle is required. Weave preserves those fields without
motion-adaptive interpolation. Mode 2 adds motion-adaptive processing;
mode 3 adds TNR at the source's fixed strength or a selected strength. A
session may repeat its mode for up to three further jobs, and later jobs
receive the source's adaptive feedback from hardware statistics, as described
in [repeated jobs](#repeated-jobs-and-adaptive-feedback). Film detection,
crop, scale and additional input formats remain unsupported.

## Selected implementation and configuration

The A733 User Manual V1.00 §14.4 describes offline NV12 input/output and weave
support. The pinned board configuration enables `CONFIG_SUNXI_DI_V3X` and
`CONFIG_SUNXI_DI_SINGEL_FILE`. The V3X Makefile selects **DI301** for
`CONFIG_ARCH_SUN60IW2`; DI300 is selected for other SoCs. The broad Kconfig name
“DI V300” does not select the register implementation by itself.

[The component manifest](../apps/deinterlace/sources.json) records exact primary
source URLs, revisions, hashes and execution locations. The configuration code
comes from Orange Pi Linux revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
The effective board configuration is pinned to orangepi-build revision
`a95129bf30ea11886cc35f08ebb3e69695a26656`, as recorded in the existing radio
component evidence. The DI source is GPL-2.0-or-later; original notices remain
in the preserved build inputs. The DRM format header retains its own notice.
No Linux kernel, firmware blob or proprietary DI library executes on the board.

The native CPU adapter reuses DI301's fixed and per-frame configuration code
against a private CPU register image. This image is never mapped into DI.
`adapt.py` records the small include/type adaptations and converts signed
literal shifts such as `1 << 31` to unsigned shifts with the same intended
32-bit register values. Kernel allocation, delay and context-switch paths are
not used; reaching their port stubs makes configuration fail.

Upstream enables adaptive feedback even for its WEAVE client. A single job
and every WEAVE job use the existing `alg_en == 0` branch after fixed setup,
preserving configured weave modes while still programming framebuffer
addresses and crop bounds. Motion and TNR sequences keep the source's
`alg_en == 1`.

Only an explicit register whitelist reaches hardware: active top-level weave
controls, pitches and addresses, plus DIT parameters and full-frame crop/demo
fields. Each word has a source-defined writable mask; native read/modify/write
preserves reserved bits. Disabled MD/FMD/TNR defaults remain CPU-only. Reset,
start, interrupt, status, version and BIST words are excluded from bulk
publication. Live control/status accesses use separate native callbacks.
The selected build asserts structure offsets, and an actual AArch64 check
verifies the relevant bitfield encodings.

## Resources and memory ownership

The pinned device tree selects base `0x05400000`, INTID 84, DE_SYS/PCK domain 1
and IOMMU master 9. The module uses PERI0_600M divided by two for 300 MHz;
CCU offsets `0x0a20` and `0x0a24` control its module and bus/reset clocks.
The adapter validates the detected oscillator, normalized PLL_REF and PERI0
rate rather than assuming a raw 24 MHz board oscillator. It adopts an already
powered DE_SYS domain and the existing shared DCDC2 800 mV operating point.
It does not run the domain-ON provider that G2D uses. If the domain command or
status does not read ON, setup fails at the parent check before any clock write.
Register readback checks configured consistency, not electrical qualification.

The path reuses the G2D board/provider composition and existing exclusive
DE_SYS, IOMMU and related master-exclusion scopes. Before common GIC writes,
the root-owned boot guard rejects inherited INTID 84 enable, pending or active
state. The controller interrupt remains disabled; the sole task polls status.
The CPU's existing low Device mapping covers DI, but does not isolate DMA.
Native callbacks also verify translation and whitelist each DI word.

Two private WB allocations of 462,848 bytes hold the authenticated upload and
copied result. The WB pool has class `dma32`, so the second starts at the
64 KiB boundary `0x48080000`. The uncached pool supplies a 32 KiB table allocation and two
471,040-byte image allocations. Each image starts after a 4 KiB guard page and
has another guard page after its rounded extent; tail padding remains guarded.
The IOMMU maps only source IOVA `0x10000000` read-only and output IOVA
`0x10400000` read/write. Each span covers 462,848 bytes. Guards, CPU metadata
and register-generation storage are not mapped. Master 9 alone translates;
all other masters remain excluded for the complete retained lifetime.

The existing input parser copies bounded fragments and authenticates the full
input digest before any DI provider acquisition or submission. Initial
transaction/walk drain, physical alias exclusion, clocks, rails, power and
remote-writer exclusion require independently admitted facts. Generated hashes,
fixture flags and idle registers do not establish those facts.

## Completion and immutable output

Before start, the task disables the controller interrupt, rejects busy/start
state, clears only stale finish, and verifies it cleared. It publishes the
whitelisted configuration under the common register mutex and arms exactly one
job. DI301 source defines processing finish at status bit 0, acknowledged
with W1C, and busy at bit 8. Linux's processing path consumes finish before
returning its DMA buffers. The native path requires its matched finish with
busy clear, then scans the retained IOMMU state and guards before copying.
It repeats those checks around each bounded copy chunk and hashes the copied
output before publication. Partial copies are never exposed as healthy output.

In pinned Linux revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`,
`drv_div3x/di_driver.c:894–920` acknowledges successful finish, reads result
history and wakes the caller. Reset occurs only on error or timeout
(`di_driver.c:803,914`), with no successful-job stop or reset before buffer
return. The native path keeps IRQs masked and preserves each job's matched
finish latch while it copies that job's output. The launch of a following job
acknowledges the latch with W1C before it publishes any register, which takes
the place of the source's interrupt-time acknowledgment. The pinned source does
not make finish acknowledgment a condition for output reads or DMA visibility.
The source-only audit, including exact source hashes and anchors, is preserved
in `deinterlace-motion-20261020/completion-source-audit.json`.

This is processing-completion evidence, not an independent fabric-drain
mechanism. Neither finish, nonbusy nor a CPU barrier authorizes release of a
buffer to another owner. Within a sequence, a completed job's buffers stay
owned by the session and mapped only for master 9; the session refills them as
described below. Timeout, stale status, IOMMU/guard faults or uncertain accesses
retain every resource; there is no reset, retry, release, unmap or further job.
Physical completion and write visibility remain unqualified.

The 888-byte session and 192-byte copied-result header are documented by their
actual C layouts and the linked `deinterlace_geometry` marker. USB replies bind
epoch, generation, input/session/output digests and fixed geometry. Full copied
output delivery, including a terminal ZLP when required, precedes ACK admission.
ACK binds generation and output digest and never releases buffers. A sequence
replies with every job's output in job order after the header, and the output
digest covers all of them. A terminal header of a sequence also reports the
number of jobs whose matched finish was observed (bytes 176–179) and, in mode 3,
each started job's programmed TNR strength, one byte per job (bytes 180–183).
Single-job sessions keep those bytes zero. Before ACK,
queries may repeat the same immutable result; after ACK, queries return only
the acknowledged header. There is one result slot and no replacement, queue or
drop policy. The 20-second session deadline bounds upload, execution and USB
service; expiry stops service and retains memory, even if delivery is incomplete.
The host workflow is described in [deinterlace preparation](deinterlace-preparation.md).

## Initial motion-adaptive operation

Mode 2 reuses the pinned `DI_MODE_30HZ` configuration: `MOTION` with one output
frame, based on field 2 as defined by `dit_setting_reg.output_mode`. The upload
contains two complete combined-field NV12 frames, previous then current,
totaling 921,600 bytes. One digest authenticates both frames before provider
acquisition. Output remains one 460,800-byte NV12 frame. The 888-byte session
and 192-byte result retain their layout. The immutable geometry marker declares
the maximum upload size; revisions 2 and later admit motion, 3 and later TNR, 4
and later a selected TNR strength, and 5 repeated jobs. Mode 1 retains its
existing single-frame input and WEAVE behavior.

The first job starts with zero motion history. The source initializes two
76,800-byte motion-flag buffers with stride 160 and initial direction zero.
In a single job the first buffer is read-only and the second writable by DI.
The fixed path enables DIT, MD and their required DMA channels, while TNR, film
detection and video-on-film remain disabled. For a single job the existing
`alg_en == 0` branch preserves the selected hardware motion settings and
excludes CPU feedback; it does not disable hardware MD. A sequence alternates
both flag buffers and enables the source's feedback, as described in
[repeated jobs](#repeated-jobs-and-adaptive-feedback).

Motion confidence thresholds and slopes at offsets `0x1004` through `0x100c`
have no explicit initialization in the pinned DI301 setup. Mode 2 therefore
requires an admitted lifetime with dedicated DI reset held asserted and its bus
and module clocks gated. Native readback checks all three conditions before
following `di_clk_enable`: preserve the already released DE_SYS reset, release
dedicated DI reset, configure rate, then enable bus and module clocks. It never
asserts reset on inherited work or uses reset as a drain/recovery operation.
This follows the upstream reset-default lifecycle; numerical confidence defaults
and physical reset effectiveness remain unqualified. The adapter never writes
CPU-shadow zeros into those confidence registers. Mode 1 still uses its admitted
powered, reset-deasserted entry.

A single motion job requires 1,384,448 bytes of WB allocations and 1,617,920
bytes of NC allocations; each repeated job adds 460,800 bytes to each WB
allocation. The existing 32 KiB IOMMU table maps five spans across four L2
windows: previous input at `0x10000000`, output at `0x10400000`, current input
at `0x10800000`, and flags at `0x10c00000` and `0x10c20000`. All have unmapped
guard pages and checked tail padding. Both input frames are checked against the
retained authenticated upload before output publication. The same one-attempt,
retained-result and USB ACK rules apply.

## Temporal noise reduction at a fixed or selected strength

Mode 3 submits TNR-only jobs. The first job reads two combined-field NV12
frames, previous then current, totaling 921,600 bytes. Output is planar YUV420:
307,200 bytes of Y followed by 76,800 bytes each of U and V. Output strides
are 640 bytes for Y and 320 bytes for U and V. The session still describes
input geometry; mode 3 identifies the output format in the copied result.

The effective single-file selector in pinned `di_client.c` enables MD and TNR,
routes previous/current inputs and the TNR output, and requires planar output.
With session strength 0, the adapter uses `DI_TNR_MODE_FIX`, and fixed setup
in `di301.c` programs strength 58 (`0x3a`) for each channel. On the first
sequence, per-frame setup skips CPU feedback but still enables TNR and programs
its parameters. No source branch bypasses TNR for the first job.
Numerical filtering remains unqualified.

Session strength 1, 2 or 3 selects the source's adaptive TNR mode at level
LOW, MIDDLE or HIGH. The word occupies the last geometry word, which earlier
sessions kept zero, and must be zero in modes 1 and 2. Its lower 16 bits hold
the strength and its upper 16 bits the repeat count. The algorithm parameters
stay enabled for every job, so `di_alg_fixed_para` sets the level's initial
gain and `di_dev_set_tnr_para` writes it to all three channel strengths: 38
(`0x26`) for LOW, 48 (`0x30`) for MIDDLE and 62 (`0x3e`) for HIGH. The first job
runs no CPU algorithm; later jobs of a sequence adjust the gain from hardware
statistics. Among the submitted TNR words only the strength word changes; the
enabled algorithm also changes two DIT words that stay in the CPU image. Plan
validation of a first job accepts only the four source strengths, and the
session's selection must match. Host preparation refuses a selected strength
for an image older than geometry revision 4.

Each zeroed flag buffer follows the source's TNR allocation of width times
height, totaling 307,200 bytes. The source retains stride 160 despite this
larger allocation. Input flags map at `0x10c00000`; output flags map at
`0x10c50000` to avoid overlap. Each guarded flag allocation occupies 315,392
bytes. Frame allocations, authentication, reset-held admission, completion,
copied output and retained-resource rules follow the initial motion path.

Only active top-level, MD and TNR words reach hardware for this mode. Reserved
bits remain preserved, and disabled DIT/FMD settings stay in the CPU image.
The TNR word masks follow named fields in pinned `di301_reg.h`, including
source-initialized accumulators and random state. No CPU-shadow confidence
thresholds are submitted. Camera and display operation, pixel quality and
physical DMA remain unqualified.

## Repeated jobs and adaptive feedback

The upper 16 bits of the session's strength word hold `repeats`, the number of
jobs after the first, from 0 to 3. Earlier sessions kept that half zero and
still run one job. Each repeated job consumes one more uploaded frame and
produces one more output, so the upload holds `1 + repeats` frames in mode 1
and `2 + repeats` frames in modes 2 and 3. One digest authenticates the whole
upload before provider acquisition, as for a single job. The limit of three
repeats keeps a five-frame upload of 2,304,000 bytes and a four-output reply
of 1,843,392 bytes within the 20-second session at USB full speed. The linked
geometry marker reports revision 5 and raises both capacities to that limit.

Job *k* of a motion or TNR sequence reads frame *k* as previous and frame
*k* + 1 as current. Frames alternate between the two guarded input
allocations: even frames use the source span at `0x10000000` and odd frames
the current span at `0x10800000`. The two MD flag buffers alternate the same
way, following the source's `md_buf.dir` toggle, so each job reads the flags
the previous job wrote. A sequence therefore maps the first flag buffer
read/write as well. WEAVE keeps every frame in its one input allocation.

A job proceeds as follows:

- **Launch:** acknowledge the previous finish, publish the whitelisted
  registers and start, as for a single job.
- **Finish:** require the matched finish with busy clear. If another job
  follows, read the statistics that pinned `di_dev_get_proc_result` collects:
  15 FMD/FOD/ITD counters at `0x300c`–`0x306c` and the nine TNR accumulators
  at `0x402c`–`0x404c`. These registers are only read, through the same
  whitelist, translation and liveness checks as every other access.
- **Copy:** scan the IOMMU, copy the output into the job's slot of the private
  result and recheck that both DMA inputs still equal their uploaded frames.
- **Next plan:** store the statistics in the CPU register image and call
  `a733_deinterlace_next()`. It runs the source's finish-time collection,
  increments the sequence number and calls `di_dev_apply_para`, which runs
  `di_alg` and generates the next job's registers.
- **Retire and refill:** move the oldest input allocation and the output from
  DEVICE to CPU ownership, copy the next frame into the input, zero the output,
  issue a barrier and return both to DEVICE ownership. Guards are checked
  before and after.

The next plan is generated before any buffer changes, so a refused plan
leaves the finished job's buffers untouched. A buffer is rewritten only after
the job that read it has a matched finish with busy clear, a clean IOMMU scan
and a completed copy. The pinned driver returns a job's DMA buffers to its
caller at the same point. That reliance is a physical claim: preparation of a
sequence requires the reviewed memory fact `di_finish_ends_job_buffer_access`.
Each later launch also requires the start bit to read zero again; the source
never reads it back, so board evidence must show that DI clears it.

Motion and TNR sequences run the source's algorithm with `alg_en == 1` from
the first job, which in mode 2 also sets the DIT field-weave phase bits in
`0x2010`. In mode 3 the first job's submitted registers equal a single job's.
WEAVE sequences keep `alg_en == 0`: the source's ITD branch would otherwise
select motion interpolation. The algorithm feeds back into later jobs as
follows:

- **TNR gain:** with strength 1–3, `di_alg_tnr_strengths_adjustment` updates
  the gain from the TNR accumulators and clamps it to the level's bounds:
  16–38 for LOW, 28–48 for MIDDLE and 52–62 for HIGH. The gain is written to
  all three channel strengths of the next job. Like Linux `do_div`, the source
  divides by the low 32 bits of a 64-bit denominator. Strength 0 keeps the
  fixed value 58 (`0x3a`).
- **Field order:** detection changes `bff` only after 15 consecutive reversed
  classifications, so it cannot change any submitted word within four jobs.
  Plan validation requires top field first.
- **Progressive detection:** ITD needs more than five progressive
  classifications to lock, so it cannot change the DIT interpolation mode
  within four jobs. Plan validation requires motion interpolation in mode 2.
- **Film detection:** the source enables it only for 60 Hz clients with two
  output frames, which this application does not configure. FMD hardware
  stays disabled; its counters are read only because the source reads them.

The adapter refuses two statistic patterns that the source cannot process in
standard C. A field-order count above `INT32_MAX / 11` would overflow the
source's signed products, which Linux builds wrap. A TNR denominator whose low
32 bits are zero would divide by zero. Either refusal ends the session with
every resource retained, as does any refused statistics read, plan or refill.

## Preserved software evidence

Evidence is under `deinterlace-native-20261020`. The configuration-only
ASan/UBSan check covers reused setup, bounds, masks and immutable identity.
The final native batch uses ELF `a472162b…` and the exact sealed host session.
It executes real upload, allocation, clock/rail ledgers, IOMMU construction and
scanning, configuration publication, completion handling and private copying.
The device-effect model interprets submitted DI addresses, pitches, format and
mode to perform the weave; it does not replace output with a precomputed oracle.
Scheduler, prepared-memory authority, translation metadata, external PMIC reads
and physical MMIO effects remain modeled.

The same final image passes nominal/terminal behavior and targeted stale,
nonfinish, busy-finish, IOMMU, late-copy, guard and upload refusals. A linked
TinyUSB full-speed case restores the actual retained terminal state and checks
the 460,992-byte response, FIFO refill, terminal ZLP and ACK ordering, with DCD
transfers modeled. Root's four boot-guard and three SRAM cases are preserved
separately. None of these checks establishes physical DI, USB, power, recovery
or DMA visibility. No board, queue or storage access occurred.

The separate `deinterlace-motion-20261020` evidence covers initial MOTION
configuration under ASan/UBSan, the actual allocator and five-span table, cold
entry refusals, new buffer guards and copied-result ownership. Its device model
performs explicitly synthetic bounded writes; it does not implement or validate
DI motion arithmetic. The host checks output integrity without a CPU pixel
oracle. Earlier WEAVE, boot, power and USB evidence remains bound to its original
frozen image and is not replayed for this mode.

The accepted motion image retains the inherited descriptive workload label
`copied-NV12-offline-weave`. That label is not a runtime predicate: the sealed
session mode and linked geometry marker determine admission. The generator is
corrected separately for future builds; the accepted image is not rebuilt for
that wording change.

The `deinterlace-tnr-20261020` evidence executes the linked fixed-strength TNR
configuration, guarded allocation, IOMMU construction, completion, copied output
and native acknowledgment. A distinct fault case corrupts the boundary after
the larger TNR flag buffer; native code retains resources and refuses output.
The host captures that exact native response as planar YUV420 and rejects a
response header naming the motion mode. Two focused SRAM cases cover mode 3
admission and reject an unsupported mode. Device completion and output bytes
are synthetic; these checks establish no numerical filtering or physical DMA.

The allocation fixture preserves CPU context, task stack, provider state and
modeled MMIO at the actual allocator return. Its original run omitted that
checkpoint; one affected replay repaired serialization and reproduced the
original allocation proof and page table byte for byte. Completion resumes the
saved context with an identical final session. The fixture models its prepared
board deadline and counter before allocation; it excludes bootstrap timing.

Selectable strength is checked by `checks/deinterlace-strength.py`. Host plan
generation under ASan/UBSan produces all four strengths with the pinned DI301
code, confirms that only the strength word differs among submitted TNR words,
and refuses strength 4, strength outside mode 3 and an unknown strength word.
The linked AArch64 image, SHA-256
`9a4c1c388770184a5ce52079bd4583e5883c47e0aa4c2f84bb8c14a297bbc7d2`, produces
identical register plans for strengths 0 to 3 and refuses strengths 4 and 5.
Host preparation mirrors the strength rule for every mode, refuses a selected
strength for the accepted revision-3 image and labels each captured mode. A
rebuilt 2536-byte SRAM launcher admits strengths 0 to 3 and refuses an
out-of-range strength and strength outside TNR in the shared AArch32 model,
using an earlier TNR bundle's identities. The earlier WEAVE, motion and TNR
launcher cases, plan checks and TNR host capture pass with the changed code.
No device effect, filtering arithmetic or physical operation is modeled.

Repeated jobs are checked by two host checks against the linked image with
SHA-256 `3d5acdf6bcdbd2653b2e0537f4a8169235cbb83fe0be5b33e2b6fe0e6007d25e`.
Their results are not yet frozen as evidence.

- `checks/deinterlace-jobs.py` generates 54 sequences, 162 job plans in all,
  with the pinned DI301 code under ASan/UBSan. Each later job's statistics come
  from a fixed generator, including all-zero counters. The check recomputes
  each of the 72 later TNR strengths from those statistics with the source's
  integer widths; the gain both falls and rises within its level. It confirms
  that submitted words change between jobs only in input and flag addresses,
  the TNR strength and the cleared accumulators. Single-job plans of every
  mode and strength equal those of the parent revision. The linked image
  produces byte-identical plans from the same statistics. The check also
  covers host preparation and capture rules, the admission fact a sequence
  needs, and a rebuilt 2536-byte SRAM launcher that admits repeats 0 to 3 and
  refuses 4.
- `checks/deinterlace-jobs-native.py` runs the linked task through WEAVE,
  motion and TNR sequences with a synthetic device. The device model reads
  each job's inputs through the actual IOMMU table and requires the job's
  uploaded frames and the previous job's flags. It keeps busy set for three
  status reads and returns per-job statistics. A memory hook refuses any CPU
  write to a DMA input or the output while a job is in flight. Every later TNR
  strength must equal the independently recomputed gain. The native USB
  serializer, host capture and ACK receipt run on each reply, and the actual
  allocation rows and page table pass host preparation's allocation check.
  Failure cases cover a refused statistics read, the two refused statistic
  patterns, a missing finish on the third job, busy with finish, a changed
  input, a changed guard and an IOMMU fault. Each ends with every allocation
  retained and the finished-job count in the reply.

`checks/deinterlace-usb.py` also replays the four-job TNR result through the
actual TinyUSB stack at full and high speed: 1,843,392 bytes with FIFO refill,
terminal ZLP and ACK ordering. The earlier WEAVE, motion, TNR, strength,
launcher, boot-guard and host-capture checks pass with this image. Its
single-job TNR reply, ACK receipt, output and page table equal the earlier
`deinterlace-tnr-20261020` evidence byte for byte. None of these checks
establishes DI arithmetic, the statistics hardware produces, buffer access
after finish or any other physical behavior.
