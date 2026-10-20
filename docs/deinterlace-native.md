# Native offline deinterlacing

`sunstone-a733-deinterlace` accepts an authenticated fixed-geometry NV12 job,
submits one offline WEAVE or initial motion-adaptive operation to A733 DI301, and exposes an immutable
copied output through USB0. It retains the controller, buffers, IOMMU tables,
clock/power ownership and copied result until whole-board power removal.
Successful output delivery does not authorize another job or resource reuse.

The first path supports combined fields, top field first, with 640-byte Y and UV
strides. The Y plane contains 480 lines and the UV plane 240 lines, totaling
460,800 bytes. Alternating lines belong to alternating fields. Deterministic
field patterns and caller-supplied frames are valid inputs; no media permission
or preknown output oracle is required. Weave preserves those fields without
motion-adaptive interpolation. TNR, film detection, crop, scale, additional formats and repeated jobs remain
outside both profiles. Mode 2 adds initial motion-adaptive processing as
described below.

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

Upstream enables adaptive feedback even for its WEAVE client. This fixed path
uses the existing `alg_en == 0` branch after fixed setup, preserving configured
weave modes while still programming framebuffer addresses and crop bounds.
It does not run ITD feedback or process a second frame.

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
Register readback checks configured consistency, not electrical qualification.

The path reuses the G2D board/provider composition and existing exclusive
DE_SYS, IOMMU and related master-exclusion scopes. Before common GIC writes,
the root-owned boot guard rejects inherited INTID 84 enable, pending or active
state. The controller interrupt remains disabled; the sole task polls status.
The CPU's existing low Device mapping covers DI, but does not isolate DMA.
Native callbacks also verify translation and whitelist each DI word.

Two private WB allocations of 462,848 bytes hold the authenticated upload and
copied result. The uncached pool supplies a 32 KiB table allocation and two
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
operation. DI301 source defines processing finish at status bit 0, acknowledged
with W1C, and busy at bit 8. Linux's processing path consumes finish before
returning its DMA buffers. The native path requires its matched finish with
busy clear, then scans the retained IOMMU state and guards before copying.
It repeats those checks around each bounded copy chunk and hashes the copied
output before publication. Partial copies are never exposed as healthy output.

In pinned Linux revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`,
`drv_div3x/di_driver.c:894–920` acknowledges successful finish, reads result
history and wakes the caller. Reset occurs only on error or timeout
(`di_driver.c:803,914`), with no successful-job stop or reset before buffer
return. The current single-job native path keeps IRQs masked and preserves the
matched finish latch throughout copying. The pinned source does not make finish
acknowledgment a condition for output reads or DMA visibility. Future interrupt
service or repeated jobs require their own acknowledgment and motion-flag
lifecycle. The source-only audit, including exact source hashes and anchors, is
preserved in `deinterlace-motion-20261020/completion-source-audit.json`.

This is processing-completion evidence, not an independent fabric-drain
mechanism. Neither finish, nonbusy nor a CPU barrier authorizes reclamation.
Timeout, stale status, IOMMU/guard faults or uncertain accesses retain every
resource; there is no reset, retry, release, unmap or second submission.
Physical completion and write visibility remain unqualified.

The 888-byte session and 192-byte copied-result header are documented by their
actual C layouts and the linked `deinterlace_geometry` marker. USB replies bind
epoch, generation, input/session/output digests and fixed geometry. Full copied
output delivery, including a terminal ZLP when required, precedes ACK admission.
ACK binds generation and output digest and never releases buffers. Before ACK,
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
and 192-byte result retain their layout; the immutable geometry marker is
revision 2 and declares the maximum upload size. Mode 1 retains its existing
single-frame input and WEAVE behavior.

This is one initial operation with zero motion history. The source initializes
two 76,800-byte motion-flag buffers with stride 160 and initial direction zero.
The first buffer is read-only and the second writable by DI; neither is reused.
The fixed path enables DIT, MD and their required DMA channels, while TNR, film
detection and video-on-film remain disabled. The existing `alg_en == 0` branch
preserves the selected hardware motion settings and excludes CPU feedback; it
does not disable hardware MD. No steady-state history or repeated-job claim is
made.

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

Motion requires 1,384,448 bytes of WB allocations and 1,617,920 bytes of NC
allocations. The existing 32 KiB IOMMU table maps five spans across four L2
windows: previous input at `0x10000000`, output at `0x10400000`, current input
at `0x10800000`, and flags at `0x10c00000` and `0x10c20000`. All have unmapped
guard pages and checked tail padding. Both input frames are checked against the
retained authenticated upload before output publication. The same one-attempt,
retained-result and USB ACK rules apply.

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
