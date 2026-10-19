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
and retain resources. A session fixes its program and input before OPEN; changing
them requires another session. Same-session input replacement remains required work.

## Trusted compiler profile

The [package definition](../include/pvr_compute_package.h) accepts one common-profile
Mesa USC program, at most 4,032 instruction bytes, 4,096 input bytes and 3,968
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
must be zero. A single dispatch containing multiple workgroups, general geometry,
additional argument layouts, concurrent jobs, broader mappings and graphics
remain required functionality.

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
allocation count. The original compiler header remains unchanged. Coefficients
and shared registers must be zero.

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

The [observer](../apps/gpu-program/observe.py) reads an already-running selected
USB instance and epoch. It returns copied output to a new file. It never launches
or resets a board. Physical launch requires the existing independent GPU memory,
entry, recovery and ownership evidence; the host helpers do not establish it.

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
