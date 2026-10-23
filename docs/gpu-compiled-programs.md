# Caller-selected GPU compute programs

The optional native compute application accepts a trusted compiled program and
copied input in a sealed session. The same executable accepts different programs
without recompilation. The service publishes private GPU memory, submits the
program, acquires its firmware completion timeline and returns copied output.
Software checks pass. Physical USC execution remains unqualified.

The interface reuses `OPEN`, `COMPUTE`, `STATUS`, `COMPUTE_OUTPUT` and `CLOSE`.
`pvr_gpu_service_compute_read` requests an output word offset and returns at most
32 acquired words with the submission sequence and total extent. Session ownership
also governs output access. Reset, cancellation and other faults revoke output
and retain resources. A session fixes its program, input layout and geometry before OPEN. The optional
runtime-input mode replaces copied data after acquired completion in that same
session. Selecting another program still requires another session.

## Trusted compiler profile

The [package definition](../include/pvr_compute_package.h) accepts one common-profile
Mesa USC program, at most 4,032 instruction bytes (4,000 in profile three), 4,096 input bytes and 3,968
output bytes. The native producer copies code and input into separate private
4 KiB allocations. USC storage includes 32 instruction-prefetch bytes and zero
padding. The output has a 64-byte leading guard and guarded unused trailing bytes.
All program/input mappings are GPU read-only; output permits GPU writes. Existing
context state also permits GPU writes. Firmware memory stays outside the workload VM.

The Programmable Data Sequencer supplies the input address in unified registers
zero and one and the output address in registers two and three. Version two also
supplies the first element index in register four and the tile count in register
five. Input starts with a little-endian unsigned 32-bit element count.
The package declares input header bytes, input bytes per element and output bytes
per element. The native validator requires complete count-derived extents without
overflow. Trusted source review must establish that those declarations describe
the program's actual accesses.

The package version and profile identify the register and dispatch ABI. The
8,288-byte package and 8,944-byte session envelope retain their sizes.

| Version/profile | Input registers | Dispatch geometry |
| --- | --- | --- |
| One | Two pointers in four registers | One 32-lane workgroup; a lane can loop over elements |
| Two | Two pointers and first-index/tile-count in six registers | One through 31 direct dispatches, each containing one 32-lane workgroup |
| Three | Two pointers and local index in five unified registers; workgroup IDs/counts in seven coefficients | One direct dispatch containing a bounded 1D/2D grid of at most 31 workgroups |

Version two partitions at most 992 elements. Dispatch `g` receives first index
`32*g` and `min(32, count-32*g)` valid elements. A lane computes its global element
index by adding the first index to its instance number. It must reject lanes
outside the tile count. The resulting output intervals partition the declared
elements without overlap, including a partial final tile. The supplied kernels
also check the full input count before each access.

The planner validates the exact input and output extents, positive dispatch
count, count-derived number of groups and a fixed tile width of 32. Version two
stores dispatch count in the header word at byte 68 and tile width at byte 156;
version one requires both words to remain zero. All other resource requirements
remain explicit. Programs cannot require shared memory, barriers, atomics,
textures, scratch storage, spills or external USC library calls. Entry offset
must be zero. Arbitrary geometry, additional argument layouts, kernel barriers/shared memory,
concurrent jobs, broader mappings and graphics remain required functionality.

Each dispatch has an immutable PDS data slot. The pinned three-constant generator
emits 80 data bytes and 28 code bytes, including the conditional prelude. Data
slots round up to 128 bytes. Shared code occupies a 64-byte slot at offset 3,968,
followed by 64 zero bytes. Thus the existing 4 KiB PDS page holds 31 data slots
and shared code. Logical PDS extent grows from 128 bytes to the complete page;
the native mapping and publication cover that complete extent. The existing
88-page workload reservation requires no additional page.

The CDM stream contains one 28-byte kernel per dispatch, then the existing
28-byte final fence kernel and four-byte termination. Its largest form occupies
900 bytes. Termination stays outside the final 64-byte page guard defined by
Mesa's `cdm.xml`. One firmware compute command and one timeline UPDATE enclose
the complete stream. Partial group completion cannot expose output: acquisition
requires the final timeline sequence and intact guards.

Mesa's DOUTW constants use H32/L32 storage order, while the even input register
receives the low half. The tile pair therefore packs tile count in the high half
and first index in the low half. The native bytes at PDS slot offsets 32 and 36
contain tile count and first index respectively.

The package preserves Mesa's eight-byte `pco_precomp_data` header. Its instruction
length, temporary registers, input registers, coefficients and shared registers
must agree with the exported compiler metadata. The compiler can report zero
allocated input registers for explicit input-register intrinsics. The host exports
the actual register-use mask after NIR lowering and requires precisely registers
zero through five for version two. Its ABI reserves all six despite the raw
allocation count. The original compiler header remains unchanged. Profiles one and two require coefficients and shared registers to be zero.
Profile three uses the coefficient ranges described below and keeps shared
registers at zero.

Temporary registers use the upstream DOUTU encoding: round up to four-register
blocks and encode the six-bit block count at bit 35. Four pointer registers use
one 16-byte CDM unified allocation unit; six registers use two units. Native PDS
and CDM bytes match the pinned
Mesa generators for both checked programs. The conditional PDS prelude and final
fence remain; they do not add support for barriers inside a kernel.

These rules are an execution contract for trusted programs. Package hashes and
bounds do not independently verify arbitrary USC instructions or prove that their
memory accesses are confined. The actual VM mappings enforce their permissions;
the native API requires the caller to establish reviewed source/build provenance.

## A real workgroup grid

Version/profile three submits one direct CDM kernel for a 1D or 2D grid.
The sealed package fixes the compiler local shape `(Lx,Ly,1)`, with positive
`Lx*Ly=32`. Logical global dimensions `(W,H,1)` satisfy `W*H=count<=992`.
Group dimensions are `(ceil(W/Lx),ceil(H/Ly),1)` and their product is at most 31.
This is an initial admission policy; it is not the hardware's grid limit.
Programs must reject the padded lanes outside the logical dimensions.

The common package prefix remains 160 bytes. Only version/profile three
interprets the next 32 bytes as eight geometry words: group dimensions,
logical global dimensions, system-value ABI one and reserved zero. Its actual
USC source begins at byte 192 with a 4,000-byte capacity. Earlier profiles still
place code at byte 160 with their 4,032-byte capacity. All versions retain an
8,288-byte package, input at byte 4,192 and the same sealed envelope size.
Mismatched version/profile pairs are refused before interpreting that union.

Input begins with three unsigned words: count, width and height. Initial package
admission and runtime copied-input consumption both require these words to match
the sealed logical dimensions. The remaining input/output lengths must agree
with the declared per-element layout. Input replacement changes values while
retaining program, geometry, allocation and timeline ownership.

The pinned compiler allocates local invocation index in unified register four,
after the two pointer arguments in registers zero through three. Workgroup IDs
occupy coefficients zero through two. Workgroup counts occupy coefficients four
through six, leaving coefficient three as alignment padding. The compiler exports
five unified inputs and seven coefficients. Coefficients are workgroup-local
common-store inputs; they are separate from OpenCL shared memory and shared
registers. Scratch, spills, barriers, atomics, shared memory and shared registers
remain zero in this profile.

The host adapter extracts the pinned `allocate_val`, `check_unused_sysvals` and
`pvr_alloc_cs_sysvals` functions from Mesa's `pvr_arch_pipeline.c`. It reserves the
four pointer inputs and invokes those functions after NIR postprocessing, at the
same point as the Vulkan compiler. Both useful kernels use the standard local-ID,
workgroup-ID and workgroup-count intrinsics. Metadata and actual system-value
ranges must match the profile. The metadata exporter and a separate adapter
without that exporter produce identical USC bytes. Provenance records identify
the original upstream compiler, the actual comparison executable and its adapter
source separately. Older profiles retain their original compiler comparison.

The pinned compute PDS generator supplies a coefficient-update entry and an
ordinary task entry. Its data uses 18 words; its emitted code ends after 13
instructions. The generator's `code_size` reports 52 bytes despite the upstream
upload interface naming its argument in words. The reference checks the emitted
end pointer and records the units explicitly. It also saves the ordinary-entry
offset before code generation resets that generator field.

The coefficient path ends with HALT at instruction nine. The ordinary branch
targets instruction ten. The native producer inserts two pointer DOUTWs at that
ordinary entry, leaving the branch and coefficient path intact. Pointer constants
and controls occupy bytes 80 through 111 after aligned generator data. The
inserted writes use no temporaries and omit END/LAST. Original ordinary-task
local-index publication and USC launch follow them. The resulting 60-byte code
begins at byte 128. The indirect address stays zero; direct group counts use
ordinary X/Y/Z word order. Pointer constants retain the earlier generator's
H32/L32 convention. That byte comparison does not qualify physical register-half
mapping.

CDM allocates two 16-byte unified units, one 64-byte coefficient unit, one
16-byte PDS temporary unit and seven 16-byte PDS data units. `USC_COMMON_SHARED`
is false. Hardware local shape is flattened to `(32,1,1)`; NIR reconstructs the
compiler's local coordinates. Mesa treats coefficient state as local to each
workgroup, so another workgroup does not reuse it as persistent shared state.
A final fence and termination follow the 28-byte grid command, for 60 command
bytes total. One firmware task and one UPDATE cover the complete grid. The
existing complete PDS page publication and per-job cache contract still apply.

The [transpose kernel](../shaders/pvr_grid_transpose.cl) uses an `(8,4,1)` local
shape and writes a row-major matrix into transposed order. Each admitted `(x,y)`
writes `x*H+y`, a unique index below the element count. The
[neighbor kernel](../shaders/pvr_grid_neighbor.cl) uses `(32,1,1)` and sums each
matrix element with its available horizontal neighbors. It writes the unique
row-major index and never crosses a row boundary. Both reject padded lanes.
These consumers use the same native mechanism as other source-admitted programs;
the runtime contains no kernel-name or executable-hash selection.

## Compiler and host preparation

The source pin is Mesa `86c7176d2dd0065555521bf8c61f208bf53c75d6`.
The existing [compiler build](gpu-compute-programs.md) supplies `mesa_clc`, `pco_clc`,
PDS generators and private dependencies. Compilation runs on the host; FreeRTOS
executes the native producer/service and USC executes the selected GPU code.
No runtime compiler or filesystem is required. Existing MIT notices remain intact.

The host [compiler tool](../apps/gpu-program/compile.py) adds metadata export to a
private copy of the pinned `pco_clc` source. Compilation passes are unchanged.
It exports the final `pco_data` and lowered NIR input-register use, rejects
unsupported requirements and records the
actual compiler executable, source, SPIR-V, USC and command identities. Its raw
outputs and instrumented source stay outside the repository. Use explicit cache
paths for Mesa source/build, Ninja and the output directory:

```sh
python3 apps/gpu-program/compile.py compile \
  --profile 2 --source shaders/pvr_vector_add_dispatch.cl --mesa "$MESA_SOURCE" \
  --build "$MESA_BUILD" --ninja "$NINJA" --output "$PROGRAM_BUILD"
python3 apps/gpu-program/compile.py package \
  --program "$PROGRAM_BUILD/program.bin" \
  --manifest "$PROGRAM_BUILD/manifest.json" --input "$INPUT" \
  --output-bytes 132 --input-header-bytes 4 --input-element-bytes 8 \
  --output-element-bytes 4 --output "$PACKAGE"
```

The example expects 33 elements in two dispatches. Vector addition stores the count followed by
two unsigned 32-bit vectors. The separate affine program stores count, factor,
bias and one vector. Arithmetic wraps modulo 2^32. These are validation consumers;
the runtime contains no program-name or executable-hash allowlist.

A trusted review record uses schema `sunstone-gpu-trusted-program-v1`,
`source_reviewed: true`, and `package_sha256`, `source_sha256` and
`compiler_sha256`. Review must cover program behavior and the declared data layout.
The [session helper](../apps/gpu-program/prepare.py) combines that record, the
592-byte GPU base session and the package into an 8,944-byte GPU envelope.
The envelope contains its own lengths and package digest. The canonical GPU
admission record must reference the review through `gpu_program: {path, sha256}`.
Canonical preparation and bundle validation verify and preserve this evidence.

Build the supervised single-core compute application with
`SUNSTONE_GPU_PROGRAM_APPLICATION=ON`. The existing GPU preparation command then
accepts the extended session template. Its launch contract binds the full session
digest. Native sealing copies and authenticates the envelope before OPEN. Private
GPU linker assertions preserve the complete copy across BSS clearing and reserve
12 KiB for upload. CPU startup and return implementation remain unchanged.

### Programs linked into the image

`compile.py compile --profile 3 --session` compiles a program that has no
package: its code is linked into the image and copied into two 4 KiB program
pages at session open, so the code limit is 8,128 bytes. Such a program may
leave the workgroup counts unread; the compiler then reports three
coefficients and the system-value ranges `[4, 1, 0, 3, 0, 0]`. The PDS still
writes all seven coefficients. The manifest records `session: true`, and
`compile.py package` refuses it. The one such program is the
[mimus matrix-product kernel](gpu-mimus.md), whose constant
`checks/gpu-mimus-program.py` generates and checks.

The profile-three PDS data and program, the fence program, the CDM grid
stream and the 96-byte firmware command are encoded by functions in
`drivers/gpu/pvr_compute_job.c` that both the package path and the mimus
session call; the package bytes are unchanged and still equal Mesa's.

The [observer](../apps/gpu-program/observe.py) reads an already-running selected
USB instance and epoch. It returns copied output to a new file. It never launches
or resets a board. Physical launch requires the existing independent GPU memory,
entry, recovery and ownership evidence; the host helpers do not establish it.

## Runtime input in one session

Build with both `SUNSTONE_GPU_PROGRAM_APPLICATION=ON` and
`SUNSTONE_GPU_RUNTIME_INPUT=ON` to keep the native owner task available after
its first result. The image manifest sets `runtime_input: true`; preparation checks
an immutable protocol marker against that field. The sealed program, geometry,
input length, element count, mappings and private allocations stay fixed.
Runtime input is ordinary copied data and requires no firmware rebuild or
session-envelope change. This optional mode leaves the fixed returning
application's two-job protocol intact.

A registered native task calls `pvr_gpu_service_compute_input` with the sequence
being replaced, a CPU-owned source, its length and finite queue/operation bounds.
The call copies the complete input before queue acceptance. It reserves one
4 KiB slot in the compute component; queued messages contain only client,
ticket and sequence. The caller may reuse its source when the call returns.
A normal collected reply reports worker admission. A busy slot or failed queue
send refuses the call and releases its reservation. Ticket and sequence wrap
are refused.

The worker requires session ownership, completed compute, an idle client ring,
matching epochs and serials, and the acquired native timeline for the supplied
sequence. It repeats native resource admission at consumption. Length and count
must match the sealed package. Refusals preserve the previous data and output.
After admission it checks the CPU mapping, copies input into the existing
GPU read-only allocation and publishes the complete page through the existing
EL1 cache operation. The next submission advances the timeline sequence and
revokes the previous result. Executable, PDS/CDM and translation bytes remain
immutable. Graphics and compute stay serialized.

`pvr_gpu_service_compute_read_sequence` requests at most 32 copied output words
for one nonzero sequence. A stale sequence returns no words, including while a
replacement runs. The original read API retains its compatibility behavior.
Device backing cannot be overwritten until acquired completion; output replies
own their copied bytes. A queued input slot is released once after consumption
or rejection. Faults retain GPU ownership and backing.

Device-cache ordering is implemented through the pinned Linux firmware
contract independently of CPU cache maintenance. The board driver at commit
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` documents default
[per-job device memory-cache flushing for coherency](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/include/pvr_drm.h#L515-L527).
Its [queue submission](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_queue.c#L698-L705)
places the compute task before its timeline UPDATE. Its completion code uses
that timeline to signal the job's done fence. The native queue uses the same
CDM task and UPDATE order. Its compute flags select serialization and one core;
its zero firmware configuration flags match the pinned initializer. That
initializer disables overlap only below 4 KiB; the A733 SLC is 16 KiB.
The native SLC initialization also follows the pinned driver's feature-dependent
settings. No custom cache policy replaces those defaults.

The normal pinned job/queue submission path does not add a separate
`SLCFLUSHINVAL` command per job. That command's ABI declaration does not establish
a missing call in this path. The separate `MMUCACHE` command handles translation
changes. Native graphics and compute table publication finishes before firmware
enablement, and repeated jobs preserve their mappings; copied input replacement
changes data rather than page tables.

The acquired previous UPDATE therefore establishes completion under that device
cache contract before CPU input replacement or output poisoning. The next
acquired UPDATE precedes CPU output invalidation, guard verification and copying.
Input and output CPU publication use the existing whole-page EL1 operations.
CPU cache cleaning does not itself invalidate GPU caches. The internal MCU/USC
cache operations remain inside opaque firmware governed by the published
contract. No missing host-side cache command or ordering step is identified;
physical visibility remains unqualified.

The corrected rendering path uses the same contract. Geometry UPDATE n and
partial-fragment UPDATE 2n−1 precede final fragment UPDATE 2n. Only acquired
geometry n and final fragment 2n permit output invalidation or another frame's
device input overwrite. Partial-fragment completion alone cannot transfer output
ownership. These render sources match the accepted depth/blend freeze.

The accepted source audit is
`gpu-runtime-input-20261019/root-cache-review.json`. The current compute helper,
configuration, split-render and explicit-command comparison is preserved in
`gpu-cache-visibility-20261020/assessment.json`. No build or execution was repeated
for that comparison. Software models do not establish physical cache visibility.

The GPU application's USB task receives one bounded frame into its private
mailbox. Only its registered owner task invokes the GPU service. The USB task
cannot overwrite a posted input; it discards a busy upload without changing it.
A partial frame has a one-second deadline and never reaches the service. Each
GPU operation has a one-second bound, and the existing sealed session deadline
still bounds the complete application lifetime. Output becomes readable only
after the owner copies every acquired page. The USB response buffer stays
owned until actual transmission completion.

The runtime request is eight little-endian 32-bit words: magic `0x49525047`,
version two, operation, expected sequence, epoch low, epoch high, first output
word and input byte length. Operations are READ=1, SUBMIT=2 and CLOSE=3.
SUBMIT appends the declared input bytes, at most 4,096. READ and CLOSE append
no payload. READ offsets are multiples of 32 within the declared result.
The existing 40-byte program reply header uses version two and carries the
actual sequence. Status zero means pending, one means copied output, two means
retained failure, three means completed shutdown and four means refusal.
Only status one includes words. CLOSE waits for the reviewed provider shutdown
before status three; it retains the USB session and copied host results.

The host [runtime consumer](../apps/gpu-program/runtime.py) accepts an exchange
callback or an already validated, locked device handle from the existing board
coordinator. It reads sequence one, uploads caller-supplied data, reads sequence
two and requests CLOSE. Its deadline includes writes and reads. It does not
open or launch a device. The same native API permits further input replacements
until sequence exhaustion or session expiry.

Runtime data is not executable admission evidence. Source review must establish
that admitted kernels safely interpret every allowed data value and the declared
layout. Hashes, input bounds and count checks cannot validate arbitrary USC
behavior; isolation depends on the actual mapped permissions.

## Selected evidence

Evidence resides in
`~/.cache/sunstoneos/evidence/gpu-dispatch-geometry-20261019/`.
Version two emits 264 USC bytes for affine arithmetic and 320 for vector addition.
Both use nine temporary registers and report the required six-register mask,
32-lane workgroup and no unsupported memory or synchronization requirements.
Both binaries match output from the unchanged pinned compiler.

Six sanitizer/model cases cover both programs at one and 33 elements, plus
992 affine elements and 511 vector-add elements. Every case compares complete
PDS/CDM pages with upstream generators and returns modeled device-written output.
A near-limit heap layout and a legacy package also pass. Invalid geometry,
capacity, extent and register-mask cases are refused. A damaged guard retains
the job. Native FreeRTOS composition covers full-page PDS mapping/publication,
owner-only access, final timeline completion and paged copied output for all
992 elements. These models supply GPU writes; they do not execute USC or
establish physical cache visibility.

Host checks bind both two-dispatch packages to the same ELF, reject missing source review and
changed provenance, preserve complete copied envelopes across the actual linked
BSS-clear bounds, and decode paged output. The native image links with the actual
reserved-memory, GPU and USB providers. No board access or benchmarks ran.

The native reservation admission callback also accepts acquired completion for
another submission. It checks service ownership, matching epochs and command
serials, the completed job/transport identities and the actual uncached timeline.
Its previous NEW-only check rejected a second COMPUTE. The real reservation
callback now passes completed admission and rejects changed owner, epoch,
timeline and serial. The historical frozen two-job returning image retains that
old defect; a corrected image requires qualification before physical use.

Runtime-input evidence resides in
`~/.cache/sunstoneos/evidence/gpu-runtime-input-20261019/`. Native FreeRTOS case
13 covers changed affine input at 33 elements, queue saturation, queued owner
revocation, nonowner requests, length/count/sequence refusal, caller buffer reuse,
wrap refusal, sequence-matched copied output and healthy CLOSE. Its GPU writes
and hardware callbacks are modeled. The unchanged actual native reservation
callback retains its separate completed-owner/timeline evidence.

The linked application check runs the actual owner task, mailbox, USB parser and
transmission callback with the host consumer. It covers two different inputs in
one session with seven-byte receives and 23-byte writes, different sequence-one
and sequence-two results, stale output refusal and completed shutdown. Service
replies, scheduling, USB completion and GPU arithmetic are modeled. Focused
correctness review found no remaining substantive issue. These checks establish
software behavior; physical GPU execution remains unqualified.

Grid evidence resides in
`~/.cache/sunstoneos/evidence/gpu-workgroup-grid-20261019/`. Both compiler products
use 11 temporary registers, five unified inputs and seven coefficients. Six
sanitizer/model and complete upstream-byte comparisons cover both kernels at one
element, padded 13 × 5 dimensions and 992 elements. They include both PDS entry
paths, exact emitted lengths, register allocations, padded coverage, retained
guard failure and a single final timeline. One near-limit heap layout and one
package from each legacy profile also pass.

The actual reservation constructor accepts the grid package and checks private
code copying, mappings, lease identity and completed native owner/timeline
admission. Native FreeRTOS case 14 exercises two changing matrix inputs, geometry
mismatch refusal before overwrite, copied-input ownership, sequence-specific
output and healthy CLOSE. The same linked application ELF accepts each program
in a separate session and runs two changed inputs through the existing runtime
host consumer. The arithmetic and GPU writes are modeled. Focused correctness
review found no remaining substantive issue; physical execution remains
unqualified.

## Copy output across explicit disposal and reopening

The opt-in compute lifecycle client in `apps/gpu-program/lifecycle.py` uses
protocol version 6. It requires a supervised single-core runtime-input image
built with `-DSUNSTONE_GPU_LIFECYCLE=ON`.
Its queue epoch identifies one GPU queue lifetime within
the original sealed session epoch. STATUS discovers that queue epoch; every
mutation and copied-output read binds it. Successful OPEN advances it by one,
executes the sealed initial input and publishes sequence one. Epoch wrap is
refused. The legacy runtime client continues to use its existing protocol.
Read-only STATUS sends request sequence zero and validates the returned current
sequence and queue epoch.

The coordinator supplies its validated, locked device handle, the original
absolute monotonic session deadline, and a durable callback. The callback takes
`retain(kind, identity, payload)` and returns only after the bytes and their
identity are durable. Use the coordinator's existing file synchronization and
metadata publication helpers. Identity includes profile, sealed session epoch,
queue epoch, sequence and observed state.

```python
client = lifecycle.Compute.for_device(
    device, sealed_epoch, output_words,
    deadline=session_deadline, retain=retain_gpu_copy)
initial = client.read()
replacement = client.submit(input_bytes)
client.close()
```

CLOSE retains GPU allocations. Reclamation and reuse require explicit calls:

```python
client.dispose()
client.open()
reopened_initial = client.read()
client.close()
```

Each complete output is durable before another submission or CLOSE can
invalidate it. CLOSE, DISPOSE and OPEN receipts become durable before their
methods return. Pending STATUS replies require no separate durable write.
DISPOSED reports native child disposal completion; it does not independently
qualify physical drain or a memory lease generation. Reopening keeps the same
session deadline and sealed input. It does not relaunch the application.

Transport, identity, persistence or deadline uncertainty stops the client.
`client.failure`, `client.outputs` and `client.receipts` preserve observed
identity, partial copied bytes and complete output even if publication failed.
The stopped instance refuses further transport operations. A completed output
or lifecycle receipt arriving late is retained before the deadline error.

The shared lifecycle and durability paths have producer-backed depth-mode
coverage described in [the render evidence](gpu-render.md#copy-frames-across-explicit-disposal-and-reopening).
The compute client passed one healthy seventeen-exchange lifecycle and four
tail-page refusal cases using native image SHA-256
`06315568dcea521e3cbae72388cf4d0300d408401814b2d2eabde62d7974fa41`.
The accepted 65-word package supplies pages of 32, 32 and one word. Both copied
results match the actual producer bytes. The four faults change page index,
tail count, result sequence or acquired extent; each preserves the exact
256-byte preceding prefix and stops further requests.

The actual pending OPEN STATUS retains the old queue and sequence until READY
publishes the next queue; an immediate READ then succeeds. Shared depth failure
evidence remains valid and was not replayed. GPU/service completion is modeled
and tied to a separate three-child native disposal/reconstruction record.
End-to-end GPU/firmware execution, physical drain, visibility and timing remain
unqualified.
