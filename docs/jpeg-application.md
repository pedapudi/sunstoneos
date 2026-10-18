# Supervised native JPEG application

The fixed native application connects the existing JPEG parser/executor and
cold decoder platform to shared board providers, decoder IOMMU mappings and
USB0 PIO output. It runs one pinned baseline 48-by-32 YCbCr420 JPEG decode, selecting either the
constant or nonconstant diagnostic profile. Physical decoder startup, compound
identity and copied pixels remain unqualified.

The application links the actual supervised board startup implementation.
At its scheduler boundary it creates one additional privileged JPEG owner task.
The existing PMIC task, USB0 task and software supervisor keep their provider
objects and ownership. No USB2 host, Type-C source or radio task is constructed.
All CPUs run through the existing supervised all-core foundation. The image
never arms or feeds a watchdog. A sixty-second bound stops new work while
retaining memory, clocks, power and device authority. CPU/interconnect stalls
can prevent this software deadline; confirmed whole-board power removal is the
recovery operation. The microSD and other persistent storage are untouched.

## Real admission and resource ownership

An immutable 856-byte session contains the existing 592-byte supervised board
selection2 input followed by a JPEG extension. The full session digest is bound
into the supervised contract. The JPEG extension authenticates initial
isolation, memory, electrical and supervision records, as well as the fixed
fixture and independent reference. Packaging validates these records against the
actual target, image and physical evidence. It supplies no qualified defaults.

Initial admission must establish exclusion of all ten IOMMU requesters and
secure/remote writers, prior transactions and page walks drained, both banks
disabled, decoder IRQ81 and both IOMMU IRQs exclusively owned/masked, and the
decoder OFF with external
reset asserted. Skipped requesters must remain physically quiescent for the
whole session. Register idle bits do not establish those facts. An admitted
mapping must reach genuine immutable nonsecure ROM at0x17000..0x21 fff.
The existing cold provider observes its published ROM profile before powering
the decoder. Unsupported ROM or compound decoder identity refuses execution.

The actual board schematic v1.2 groups VDD-VE with VDD-SYS, VDD-DRAM and
VDD-NPU on DCDC2 at 0.8 V. Its preserved PDF SHA256 is
`4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0`,
on PDF page 2, under `~/.cache/sunstoneos/sources/board-documents/schematic-v1.2.pdf`.
The JPEG task acquires a shared DCDC2 lease and samples the real PMIC provider;
it requires enabled 800 mV output. It never changes the shared voltage. Separate
electrical evidence must qualify the selected VE frequency at that voltage.

One common clock registry supplies the exact decoder/PLL/fabric token and a
separate IOMMU/local-exclusion token to the same permanent owner. The existing
PCK manager grants a shared OSC/PCK token. The decoder platform validates its
actual borrowed tokens before register operations. It adopts stable VE0/VE1
parents and refuses unsuitable inherited PLL state. IOMMU clock gates/reset
must already be enabled and stable. Provisioning an initially disabled VE PLL
or IOMMU clock path remains a separate dependency.

## Decoder-only IOMMU profile

The original camera installation is unchanged. The decoder variant translates
masters 6/7 and accesses only those powered micro-register sets. Both bank
register sets remain owned and accessible. Global bank/walk faults are checked;
fault output reports the exact scanned master mask 0 xc0. Unobserved skipped
master statuses are never reported as sampled zero values.

The pinned `sunxi-iommu-v2.c` read/write wrappers omit micro-register accesses
for `skip_mask`; its device-tree `skip` property also omits the corresponding
master device/power setup. Global initialization still runs with those sets
skipped. The new fixed profile follows that distinction: internal IOMMU clock
accessibility does not prove external requester isolation or power state.
All skipped requesters require independent lifetime exclusion. There is no
later master admission or shared-bank concurrent traffic.

The native owner obtains32 KiB aligned table backing and three12 KiB Normal
Non-cacheable allocations from the existing boot-validated private pool.
Only each allocation's central 4 KiB page is mapped: input read-only, luma and
chroma write-only. All neighboring IOVA pages remain unmapped. Physical guard
pages remain reserved and checked. The input is zero-padded through its mapped
page and the inclusive codec end is that page's last byte. Output pages include
padded plane allocations and checked unused tails. These permissions and guards
constrain device access; the minimum plane lengths alone supply no burst bound.
An unexpected access faults the IOMMU and suppresses success. No page or token
is released, including after completion or uncertainty.

## Completion produces a private verified copy

The existing executor performs real identity, engine, stale-status, table,
trigger and completion/error checks. Its original drain-based DONE path remains
available and unchanged. This application selects a distinct retained-copy
path, which disables/acknowledges codec interrupts and enters COPIED only after
its native copy callback succeeds. It never claims global transaction drain,
DMA ownership transfer or permission to reset/reuse device memory.

The callback scans actual IOMMU faults, checks guards and codec errors, copies
the active Y/Cb/Cr pixels once into private CPU storage, and compares all 2304
bytes with the independent fixed full-range YUV420p reference. It then scans
faults again and rechecks guards/errors before publishing the copy. The constant profile uses a 155-byte zero-coefficient derivative, SHA256
`3689fc87a6594a114026b65c2e104589ae3a872188bce0cb12ae08ab92ede398`.
Its frame and quantization metadata come from the preserved baseline fixture
`6522f2719f1f05e23a95aa71a18d52f2e91b19b4b6c94381b4100de4c79fd573`.
The derivative uses explicit DC-category-zero/AC-end-of-block Huffman tables
and zero coefficients for every block. Its ideal inverse transform is zero;
the unsigned JPEG level shift produces exactly 128 in every Y/Cb/Cr sample.
An independent FFmpeg decode must produce those same 2304 bytes before the
image build. Output backing starts with nonzero 0xa5 poison, so untouched DMA
memory cannot satisfy the comparison.
The comparison is exact. Padding is excluded from the copied payload. The constant profile remains available as the first diagnostic. Its preserved
evidence does not qualify nonconstant images or broader JPEG numerical
conformance.

USB0 uses VID/PID 1f3a:efe8, revision 0100, product `sunstoneos native JPEG` and
serial `sunstoneos-a733-jpeg`. One zero-valued request byte returns a 128-byte
little-endian header. It contains magic 0x4745504a, version 1, status and payload
length as 32-bit fields; epoch/generation as 64-bit fields; then fixture,
reference and copied-payload SHA256 digests. Status 0 is pending, 1 is a verified
copied result with 2304 following bytes, and 2 is retained failure without a
payload. `apps/jpeg/observe.py` validates captured files and opens no device.
Root-owned live transport must bind the exact descriptors, admitted session and
reference before interpreting a result.

## Software checkpoint

The original constant-profile `sunstone-a733-jpeg` image and SRAM launcher build successfully.
The image SHA256 is
`d519e230216280ba9049769bf9b021d9cd114291ef92dfb91359f66760ac6504`.
Evidence is retained under
`~/.cache/sunstoneos/evidence/jpeg-application-20261017/batch1/`.
The independent reference contains exactly 2304 bytes of value 128, SHA256
`dea313a03db46a224a28a8a79febff397b832f3256b9c9ce236e79ee90e6ed7f`.

Focused checks pass for the full immutable session, fixed electrical input,
retained-copy completion, decoder-only IOMMU installation, private copied pixels,
poisoned guards, mismatched pixels, pre/post-copy faults and errors, and the full
USB result frame. Packaging checks cover protected SRAM geometry, sealed uploads,
launcher extent, truncated-session refusal and refusal of an armed parent target.
Native copy/frame checks model provider admission, MMIO and IOMMU observations;
they do not establish physical decoder operation. The packaging geometry fixture
also models physical admission and is not an authorized board bundle.
Existing codec, cold-start, IOMMU, provider and scheduler evidence was reused.
No board or persistent storage was accessed. Stable inherited VE/IOMMU clocks,
real initial isolation and board electrical/memory authority remain physical
entry prerequisites.

The harness compiles all three sanitizer fixtures before native configuration
or host-case execution. Independent compilation failures retain separate logs
and are reported together. Before native instruction cases, its setup check
reports missing ELF symbols, named DWARF layout members and fixed fixture,
reference or session geometry in one diagnostic. It uses the public
`board_usb0_task` entry and explicit provider boundaries because static helpers
may be inlined. C return values are interpreted as 32-bit W0 values. Layout lookup skips
anonymous DWARF members, and source comparison ignores generated Python bytecode.

Dependency collection follows every object in the actual JPEG link command.
This build has 164 linked objects: 158 compiler `.obj.d` records and six assembly
records in CMake's `depend.internal`. The assembly fixture's embedded JPEG and
reference are included. Missing object coverage or empty discovery fails setup.
The batch manifests now contain 419 inputs and 164 compile commands. Original
empty manifests and the setup-only validation record are preserved in
`batch1/setup-followup/`. No image rebuild or runtime case was repeated for this
repair.

## Exact nonconstant diagnostic

The nonconstant profile uses a reproducibly generated 520-byte baseline JPEG.
It retains the 48-by-32 YCbCr420 frame, 64-byte luma stride, 32-byte chroma stride,
4 KiB mapped input/output pages and 2,304-byte active planar Y/Cb/Cr payload.
Its source generator is `apps/jpeg/fixture.py`; `nonconstant()` constructs the
entropy-coded image, while `nonconstant_reference()` indexes the ideal plane
pixels directly. The reference does not traverse scan order, zigzag positions,
DC predictors or Huffman codes.

Each block has a distinct DC level and nonzero horizontal, vertical and product
terms at DCT frequencies (4,0), (0,4) and (4,4). After JPEG normalization, each
selected inverse-transform basis contributes positive or negative one-eighth.
The dequantized coefficients are multiples of eight, making every ideal sample
an integer. The chosen levels avoid clipping. Distinct luma and chroma patterns,
block rows and columns, quantization tables and Huffman tables make plane swaps,
incorrect strides, sampling and table programming observable. These properties
support exact active-sample comparison with zero tolerance for this restricted
fixture; they do not establish general JPEG rounding conformance.

FFmpeg 8.0.1 from Ubuntu package `7:8.0.1-3ubuntu2` provides the independent
open-source CPU decoder reference. The oracle command selects the integer
`simple` IDCT, disables CPU-specific dispatch and hardware acceleration, and
emits full-range `yuvj420p` without RGB conversion or chroma upsampling.
The decoder produced exactly the analytic 2,304 samples. Its executable,
observed version, package versions and shared-library hashes are recorded in
`~/.cache/sunstoneos/evidence/jpeg-nonconstant-20261017/oracle/`.
The JPEG SHA-256 is
`d31a703609ea58038f8b5a352ee6ac3a1e02ccb0b39d74d4f124a57cff16c8e4`;
the reference SHA-256 is
`eada68215e5eb2522ba9751de74987eb1f51c948aa455740036460e9a4238296`.
The original 155-byte constant fixture and its recorded evidence remain preserved.

`apps/jpeg/fixtures.json` binds each allowed fixture length and SHA-256 to its
reference length and SHA-256. CMake rejects unknown or crossed pairs. Offline
preparation also checks the selected session against the actual immutable,
file-backed inputs embedded in the final ELF. Native startup authenticates those
same embedded inputs before constructing the task. The input copy and job use
the authenticated embedded length while retaining zero padding through the
4 KiB read-only input mapping. The supervised host selects its reference from
the frozen ELF and validates the admitted pair before any device exchange.
Session, USB result and output-plane geometry are unchanged.

The native copy compares every active Y, Cb and Cr sample exactly. Stride
padding inside the 2,048-byte luma and 1,024-byte chroma allocations is excluded
from the payload and may be written by the decoder. Physical guard pages and
the unused mapped-page tails must retain their poison values. Existing IOMMU,
codec-error and pre/post-copy guard checks remain mandatory. A tolerance change,
padding comparison relaxation or ownership-release claim requires separate
justification; this profile grants none.

The nonconstant parser and table-preparation fixture passed with AddressSanitizer
and UndefinedBehaviorSanitizer. The affected native JPEG image linked successfully,
with 164 linked objects and 419 compiler dependencies preserved under
`~/.cache/sunstoneos/evidence/jpeg-nonconstant-20261017/central/`.
Its SHA-256 is
`8f66dc8f49d48d758489040c65b5c4022c3246c1bcc83cb1d9d0c367c770343b`.
Actual native copy/frame checks accepted the reference with changed row padding
and refused swapped chroma planes, wrong luma or chroma strides, changed chroma
sampling and a one-sample error. Host checks passed paired-identity admission,
cross-pair refusal, durable copied output and rejection of forged constant or
swapped-plane payloads with matching transport hashes. The corresponding results
are in `parser/`, `native/` and `host/` beside the central evidence.

The checks model provider admission, MMIO and IOMMU observations. They execute
the actual parser, preparation, private copy, exact comparison and USB frame code;
they do not run a hardware JPEG decoder. Existing guard, provider and supervised
lifecycle evidence is reused. Physical decoder startup, table programming,
FIFO/device memory behavior and copied pixels remain unqualified.

## Retained session and USB failure

The JPEG boot patch copies the entire 856-byte session during primary EL3
admission, before provider construction or secondary startup. It hashes the
copied bytes against the private admitted contract and validates their complete
layout. Successful coherence publication makes that one retained snapshot
available to the later application. A failed seal cannot be retried in the same
image, and failed publication exposes no snapshot.

JPEG's fixed USB0-only workload does not need the board radio identity helper.
Its local boot patch replaces that helper's live session read with the seal hook;
the shared boot source is unchanged. Provider setup and application startup both
require the sealed board prefix to match the board's admitted prefix. Startup
consumes the retained extension instead of rereading the live session address.
A disagreement refuses construction. Later changes to the live extension cannot
replace admitted owner, clock, evidence or fixture identities.

Every JPEG USB terminal stop now calls `board_fail` before parking. That revokes
shared liveness, including eligibility for a result already published by the
JPEG task. The decoder task observes revoked liveness and marks its result as
failed; USB remains parked and sends no further queued bytes. This change does
not establish hardware quiescence or release any retained resource.

The focused sealing record is
`~/.cache/sunstoneos/evidence/jpeg-sealing-20261018/`. The sanitizer fixture
passed copied-session mutation, failed publication and once-only checks. Six
linked instruction cases passed retained-extension use, prefix disagreement
refusal and USB failure revocation through the actual board liveness functions.
Cache publication, USB provider observations and scheduler boundaries were
modeled. The native image linked with the JPEG boot hook and without the inherited
board boot object. The first host compile required a fixture-only declaration
for the architecture-guarded cache-publication boundary; that failure is retained.
The existing nonconstant JPEG/reference bytes and numerical evidence were reused.
No decoder or copied-pixel case was rerun.
