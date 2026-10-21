# Native DI301 preparation and capture

The host path prepares fixed 640 × 480 NV12 input for the A733 DI301 engine
under FreeRTOS. WEAVE uses one combined-field frame; first-operation motion
uses two consecutive combined-field frames. NV12 stores the full-resolution
Y plane followed by
interleaved U/V samples at half vertical resolution. Both plane strides are
640 bytes. A frame contains 460800 bytes. WEAVE input contains one frame,
and motion input concatenates two frames into 921600 bytes. Both modes copy
one 460800-byte output frame.
The combined frame interleaves top and bottom field lines in each plane.
Offline WEAVE preserves those lines in the output.

`apps/deinterlace/producer.py` seals the input digest and four typed authority
records into an 888-byte session. The required configuration is top field first,
NV12, WEAVE or first-operation MOTION OUT_1FRAME, the 300 MHz DI module clock,
both IOMMU gate values `0x10007`, and the shared DCDC2 rail at 800 mV. The reference digest stays zero.
The deterministic field pattern distinguishes the fields and plane rows:

```sh
python3 apps/deinterlace/producer.py --pattern-output /absolute/cache/input.nv12
```

`apps/deinterlace/prepare.py` admits an immutable native ELF and its tested
manifest. It checks the linked session geometry, protected memory ranges,
actual allocation rows and every IOMMU table descriptor. The two cached
allocations hold authenticated input staging and immutable copied output.
The uncached allocations hold the page table and guarded DMA input/output.
WEAVE maps only source and destination spans; source pages are read-only.
The engine receives one authenticated upload with the admitted input digest.
Motion adds guarded current-frame and motion-detection flag allocations. Its previous/current
frame spans are read-only; the output frame and output flags are writable.
The motion page table includes four L2 windows and leaves all guards and
intervening holes unmapped.

Physical admission requires typed records bound to the image, target and
session. They cover prior DMA exclusion and draining, clock and power ownership,
private memory, device mappings, DMA visibility and reviewed whole-board power
removal. Decoded clock settings require established parent stability and ownership.
A model record cannot authorize physical execution. An offline fixture may replace
only the physical-authority check and must label all resulting artifacts as models.

`apps/deinterlace/observe.py` uses the existing supervised entry and queue policy.
The native lifetime is 20 seconds; host capture has 19 seconds. The USB identity
is product `sunstoneos native deinterlace`, serial `sunstoneos-a733-deinterlace`,
and device version `0x0100`. Entry validates the frozen bundle and matching
reviewed software evidence before using the shared supervisor.

`apps/deinterlace/capture.py` receives a 192-byte result header and, on success,
the complete copied NV12 frame. It checks the epoch, generation, input/session
hashes, fixed geometry, matched finish, cleared busy bit, DI master bitmap,
guards and copied-output hash. Reserved result bytes must be zero. Full-speed
and high-speed responses require complete short-packet or zero-length-packet
termination; truncated streams fail capture.

Capture durably saves the result and output, compares WEAVE output with the
sealed input, and journals acknowledgment intent before sending the 41-byte
acknowledgment. It requires a matching acknowledgment receipt before reporting
observed success. Motion verifies the immutable copied bytes, session identity
and completion guards; it records `pixel_reference_checked=false` and
`matches_cpu_reference=null`. It has no numerical pixel oracle.
A transport or durability failure preserves an unobserved
result and the available journal. Output acknowledgment does not release memory,
clock, power or DMA ownership. Resources remain retained until the reviewed
whole-board power removal.

The focused checks are `checks/deinterlace-host-preparation.py` and
`checks/deinterlace-host.py`. Preparation consumes allocation and table bytes
from the actual native constructor fixture. Capture consumes an actual native
reply produced for the final prepared session. Mutated replies and transport
faults test refusal and acknowledgment ordering. These checks establish software
behavior; physical clock, DMA visibility, completion and recovery evidence
remain required before hardware qualification.

WEAVE preparation and capture passed the focused offline checks on 20 October
2026. Preparation consumed the actual native constructor's allocation proof and
complete page table for the emitted session. It rejected physical launch from
modeled authority and refused altered allocation aliases, permissions, guards,
table holes and geometry. Capture passed thirteen cases using the actual native
TinyUSB copied response for the sealed session. Those cases covered full-speed
and high-speed framing, pending responses, identity and completion failures,
truncated streams, durability failures, upload failure and deadline expiry.
The actual supervisor caller reached readiness with its 19-second policy and
stopped before queue or device access.

The retained host results are
`~/.cache/sunstoneos/evidence/deinterlace-host-20261020/result.json` and
`capture/result.json` beneath the same directory. The supervisor result is
`~/.cache/sunstoneos/evidence/deinterlace-supervisor-entry-20261020.json`.
The snapshot at
`~/.cache/sunstoneos/evidence/deinterlace-host-20261020/frozen/manifest.json`
preserves the host sources, sealed bundle and captured evidence. Native device
effects and entry authority were modeled; these results establish host software
behavior and do not qualify physical DI301 operation.

First-operation motion requires the initial record fact
`di_dedicated_reset_held_bus_and_module_gated`. WEAVE retains
`di_inherited_powered_reset_deasserted`. The motion entry checks reset and gate
readback before the source-exact reset release, configuration and gate sequence.
It does not assert reset during execution or recovery. Reliance on the reset
defaults follows the pinned Linux lifecycle; it does not establish numerical
correctness or physical operation. Motion starts with private zero history and
retains all frame, flag, table and staging allocations after success or failure.

The geometry marker's second revision raises maximum input capacity to 921600
bytes. The session remains 888 bytes, the result header 192 bytes and the
extended contract 288 bytes. Legacy first-revision images remain restricted to
WEAVE. Generate the deterministic two-frame input with:

```sh
python3 apps/deinterlace/producer.py --mode motion --pattern-output /absolute/cache/input.nv12
```

The focused motion checks are `checks/deinterlace-motion-preparation.py` and
`checks/deinterlace-motion-host.py`. They consume exact native allocation/table
proof and actual copied frame/ACK receipt artifacts. Earlier WEAVE, USB stream
framing and ACK durability evidence remains applicable within unchanged code
boundaries. Physical motion output and numerical behavior remain unqualified.

Motion preparation and capture passed the focused offline checks on 20 October
2026. The actual constructor supplied all allocation rows and page-table bytes
for the exact typed template. Nine altered allocation, permission, guard, table
and input-length cases were refused. The final sealed session passed root's two
changed-mode SRAM cases before native execution.

Capture consumed the actual native serializer's copied frame and ACK receipt.
It refused seven malformed input, mode and result-extent cases, authenticated
both source frames, and saved the complete output before ACK. The record leaves
pixel comparison unchecked. The native fixture used synthetic DI writes without
an arithmetic model; these results establish binding, extent and retention
behavior, not motion-picture quality or physical completion.

The current motion host closure is
`~/.cache/sunstoneos/evidence/deinterlace-motion-host-20261020/frozen/manifest.json`.
Preparation and capture results are `result.json` and `capture/result.json`
beneath the same parent directory. Native source, constructor and execution
closure remains in `~/.cache/sunstoneos/evidence/deinterlace-motion-20261020/`.

Fixed-strength TNR selects `--mode tnr` and requires geometry marker revision 3.
It authenticates two 640 × 480 NV12 frames, previous then current, totaling
921600 bytes. Its first source sequence skips CPU feedback; hardware TNR stays
enabled. Strength follows the pinned native configuration.

TNR copies one 460800-byte planar YUV420 frame. Y starts at byte 0 with stride
640; U starts at 307200 and V at 384000, each with stride 320. The wire header
continues to describe the authenticated NV12 input. Capture records the output
format, separate plane strides and offsets and writes `output.yuv420`. It has
no numerical pixel oracle or picture-quality claim.

Each TNR flag buffer uses 307200 bytes and a 315392-byte guarded allocation.
The input flag maps read-only at `0x10c00000`; the output flag maps writable at
`0x10c50000`. Preparation checks the actual allocator rows and complete table,
including every flag page and unmapped hole. All allocations remain retained.
The focused checks are `checks/deinterlace-tnr-preparation.py` and
`checks/deinterlace-tnr-host.py`; predecessor USB framing and durability evidence
continues to cover unchanged code. Their model fixture preserves the allocated
template's identities through fixture-only deterministic entropy. Production
preparation still chooses fresh identities and rejects modeled authority.
