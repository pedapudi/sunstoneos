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
the complete copied output of every job. It checks the epoch, generation,
input/session hashes, fixed geometry, matched finish, cleared busy bit, DI
master bitmap, guards and copied-output hash. Reserved result bytes must be
zero; a sequence uses bytes 176–183 as described under repeated jobs below. Full-speed
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

No host check covers WEAVE preparation and capture: the allocation proof and
page table from the native constructor, the rejection of physical launch from
modeled authority, altered allocation aliases, permissions, guards, table holes
and geometry, thirteen capture cases over the TinyUSB copied response, or the
supervisor caller reaching readiness under its 19-second policy. Recorded
results are in [deinterlace-preparation.jsonl](evidence/deinterlace-preparation.jsonl).
Physical clock, DMA visibility, completion and recovery evidence remain
required before hardware qualification.

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

No host check covers motion preparation and capture: the exact native
allocation and table proof, nine altered allocation, permission, guard, table and
input-length refusals, seven malformed input, mode and result-extent refusals,
authentication of both source frames, and saving the complete output before
ACK. The record leaves pixel comparison unchecked. Physical motion output and
numerical behavior remain unqualified; recorded results are in
[deinterlace-preparation.jsonl](evidence/deinterlace-preparation.jsonl).

TNR selects `--mode tnr` and requires geometry marker revision 3.
It authenticates two 640 × 480 NV12 frames, previous then current, totaling
921600 bytes. Its first source sequence skips CPU feedback; hardware TNR stays
enabled. By default the strength is the pinned fixed value. `--strength low`,
`middle` or `high` seals session strength 1, 2 or 3, which selects the source's
adaptive initial gain for that level and requires geometry marker revision 4.
Preparation refuses a selected strength outside TNR or for an older image.
Capture labels the mode `TNR-FIXED`, `TNR-INITIAL-LOW`, `TNR-INITIAL-MIDDLE` or
`TNR-INITIAL-HIGH`; every TNR output uses the planar layout below.

TNR copies one 460800-byte planar YUV420 frame. Y starts at byte 0 with stride
640; U starts at 307200 and V at 384000, each with stride 320. The wire header
continues to describe the authenticated NV12 input. Capture records the output
format, separate plane strides and offsets and writes `output.yuv420`. It has
no numerical pixel oracle or picture-quality claim.

Each TNR flag buffer uses 307200 bytes and a 315392-byte guarded allocation.
The input flag maps read-only at `0x10c00000`; the output flag maps writable at
`0x10c50000`. Preparation checks the actual allocator rows and complete table,
including every flag page and unmapped hole. All allocations remain retained.
Production preparation chooses fresh identities and rejects modeled authority.
No host check covers TNR preparation and capture: the five mode-specific
preparation refusals (legacy flag sizes, a shortened flag allocation, the old
output IOVA, a missing final flag page and a pre-TNR geometry marker) or the
capture of the native copied response and ACK receipt with planar output and
metadata before ACK and refusal of a mode2 result header. Numerical TNR
behavior and physical operation remain unqualified; recorded results are in
[deinterlace-preparation.jsonl](evidence/deinterlace-preparation.jsonl).

## Repeated jobs

`--repeats 1`, `2` or `3` adds that many jobs to any mode. The producer writes
`1 + repeats` frames for WEAVE and `2 + repeats` frames for motion and TNR;
each later frame shifts the luma and chroma values of the pattern. It stores
the count in the upper 16 bits of the session's last geometry word, above the
TNR strength. For example, a four-job adaptive TNR input is generated with:

```sh
python3 apps/deinterlace/producer.py --mode tnr --strength high --repeats 3 --pattern-output /absolute/cache/input.nv12
```

Preparation requires geometry marker revision 5 for a nonzero count. Its
allocation check expects the cached upload and result allocations to hold every
frame and every output, and the first MD flag buffer of a motion or TNR
sequence to map read/write. Physical admission of a sequence also requires the
reviewed memory fact `di_finish_ends_job_buffer_access`. The fact is needed
because the native task refills a finished job's oldest input and its output
for the next job once the matched finish, a clean IOMMU scan and the copy are
complete.

A successful reply carries the outputs in job order after the header, and its
digest covers all of them; a four-job reply is 1,843,392 bytes. Bytes 176–179
of a terminal sequence header count the jobs whose finish was observed. In TNR,
bytes 180–183 hold each started job's programmed strength, one byte per job;
capture requires the first to be the session's initial strength and later
adaptive values to stay within the level's bounds. Capture labels an adaptive
TNR sequence `TNR-ADAPTIVE-LOW`, `TNR-ADAPTIVE-MIDDLE` or `TNR-ADAPTIVE-HIGH`,
writes all outputs to one file and records the job count and frame size. A
WEAVE sequence still compares its output with the sealed input, because each
job reproduces its own frame.

No host check covers these preparation and capture rules or the actual native
replies of WEAVE, motion and TNR sequences. No sequence bundle has been
prepared from typed authority records, and no sequence has run on the board.
