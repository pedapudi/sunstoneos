# Per-workgroup GPU local memory

Status: qualified on the board (generic tiled GEMM with 384 bytes of local memory, four jobs) and host-tested.

The GPU queue implements bounded on-chip local memory for trusted compute
programs with 32 work items per workgroup. Board results: the local GEMM entry
`1793566602125877874-e604614d` and its global-memory control
`1793566402339564949-f2aad6a6` in [gpu.jsonl](../evidence/gpu.jsonl).

The reader is assumed to understand compute workgroups, GPU command streams,
and C memory ownership. A coefficient is a word in the PowerVR Unified Shading
Cluster's common store. The compiler uses one per-workgroup coefficient
allocation for system values and mutable local memory. A separate hardware
shared-register bank holds other pipeline data.

## Local memory occupies the per-workgroup coefficient allocation

The pinned Mesa compiler has two implementations of shared-memory accesses:

- `pco_trans_nir.c` translates `load_shared` and `store_shared` into coefficient
  reads and writes through `trans_load_common_store` and
  `trans_store_common_store`. A variable offset uses indexed common-store
  instructions; it does not introduce a device-memory address.
- `pco_nir.c` can select global backing when its spill threshold is exceeded.
  `pco_nir_io.c` then introduces a shared base pointer and ordinary global
  loads and stores. The local-memory profile refuses this compiler result.

The compute allocator, `pvr_alloc_cs_shmem` in `pvr_arch_pipeline.c`, places
local words after the system-value coefficient range. It adds their count to
`common.coeffs`. It leaves `common.shareds` zero for the admitted profile.
The local range is private to each workgroup and lasts until that group ends.
The runtime creates no DRAM allocation for it.

The CDM command processor reserves the common store using `KERNEL0`'s
`usc_common_size` field. The field measures 64-byte units. The resource encoder
reserves `ceil((system_words + local_words) / 16)` units and leaves
`KERNEL1.usc_common_shared` clear, selecting coefficients. For the example's
three system words and 96 local words, it reserves seven units, or 448 bytes.
384 bytes hold local data; the rest covers system values and alignment.

The XML encoding permits 256 units, or 4,096 words. This is an encoding ceiling;
it does not prove every word is available under every hardware configuration.
The implementation admits at most 240 local words, or 960 bytes. Including
seven system words, its largest allocation is sixteen units, or 1,024 bytes.
Increasing that software limit requires a separate hardware resource audit.

Mesa's command-stream generator excludes coefficients from firmware shared
register preservation. Its context-switch contract prevents a switch during
execution of a workgroup. The existing context resources therefore suffice for
this local-memory lifetime.

## Compiler metadata defines the admitted resources

`apps/gpu-program/compile.py compile --profile 4 --queue-program` produces a
session program and its immutable `gpu_queue_program` descriptor. The package
wire format continues to support its existing profiles only.

The host adapter uses the pinned compute system-value and local-memory
allocators. It retains OpenCL `__local` variables until PCO assigns their
32-bit byte offsets. It removes the usclib frontend's earlier local layout
pass, which would otherwise allocate the variables twice. Other address spaces
retain the existing frontend lowering.

The usclib tool normally supplies a zero runtime local-memory limit. The
adapter supplies the 4,096-word CDM encoding ceiling for compiler selection.
Admission independently enforces the smaller 240-word limit. It also refuses
compiler-selected global backing, zero initialization, spilling, scratch,
atomics, library calls, samplers, and a nonzero entry offset.

The generated descriptor carries:

| Field | Contract |
| --- | --- |
| `abi` | `GPU_QUEUE_ABI_LOCAL` |
| `coefficients` | Three system words, or seven when group counts are read |
| `local_words` | Compiler-derived local range count, from one to 240 words |
| `barriers` | Zero, or `GPU_QUEUE_BARRIER_TASK` |
| `local_size` | Two dimensions with product 32; third dimension one |

The compiler export checks that the local range begins immediately after the
system words. The compiler's total coefficient count must equal their sum.
The instrumented compiler must emit identical bytes to an uninstrumented
compiler using the same adapter. Both executable identities and the adapter
source identities appear in the compiler manifest.

At context creation, the runtime checks the descriptor and snapshots the total
common-store allocation in `common_words`. Its existing code copy and retained
context lifetime also apply to local-memory programs. Jobs reserve that saved
allocation; they do not re-read mutable caller metadata. Existing grid programs
require zero local words and zero barriers and retain their command bytes.

Program code and resource metadata remain trusted image inputs. Shape validation
and hashes cannot prove arbitrary machine code's accesses or barrier behavior.
Source review must establish bounds, initialization before every local read,
and uniform control flow at every barrier.

## Every admitted workgroup occupies one hardware task

The grid encoder flattens the local dimensions to 32 hardware instances. One
hardware task contains one workgroup. Mesa's `pco_nir_sync.c` removes workgroup
barriers when the workgroup has at most 32 instances. Its larger-workgroup
implementation uses library routines, mutex operations, and extra counters;
those requirements are refused here.

All work items must reach each source barrier. Padded rows and columns still
participate in tile loading, zero filling, and both barriers. Only final output
stores exclude padded work items. The barrier after tile consumption prevents
a subsequent depth tile from overwriting values still needed by another lane.

The PDS initializer writes group IDs and its existing group-count slots. When
only three system coefficients are used, some count slots overlap the local
range. Programs must initialize all local values they read; those initializer
writes establish no supported initial local value.

## The generic tiled GEMM exercises cooperative reuse and tails

`apps/queue-gemm/tiled_gemm.cl` computes row-major FP32 matrix multiplication
`C[M,N] = A[M,K] * B[K,N]`. Each workgroup covers eight rows and four columns.
A depth tile contains eight elements. The local arrays hold 64 values from A
and 32 from B, requiring 96 words, or 384 bytes.

Each of the 32 work items loads two A values and one B value. Out-of-range
loads contribute zero. A barrier precedes eight fused multiply-add operations
per work item, and another barrier precedes tile reuse. The compiler emits
2,800 USC code bytes, uses twenty temporaries, and allocates 99 coefficients.
Its lowered instruction log contains indexed `cf3` accesses for local data.

The returning test application selects M=13, N=11, and K=19. Six workgroups
cover the matrix, and each group processes three depth tiles. Row, column,
and depth tails all require padding. Four serial jobs vary signed integer-valued
FP32 operands so that stale output fails per-element verification. The CPU
reference accumulates integer products and encodes their exact FP32 bit patterns.
All intermediate magnitudes remain below the FP32 exact integer limit.
This check runs under the application's existing general-register CPU policy.
The test compares each result and checks that two surrounding sixteen-word
regions retain their values from before context creation.

The result is 48 little-endian bytes. It reports completed jobs, M, N, K,
last checksum, last sequence, local bytes, checked guard words, and four
`kick_to_done_us` values. Those durations measure the GPU worker's kick-to-
timeline interval. They exclude CPU verification and do not establish an
application performance result.

## Host checks and the returning image have different evidence scopes

no host check (evidence: docs/evidence/gpu-compute-local-memory.jsonl) checks the real compiler product and reproduces its
tracked descriptor. It refuses malformed compiler metadata and compares every
supported common-store allocation against the pinned Mesa CDM generator. It
checks legacy command bytes and preservation of output on resource refusal.

The same OpenCL source compiles for the host and runs on 32 threads sharing
its actual local arrays. A pthread barrier synchronizes every work item.
Cases cover full tiles, partial tiles, zero depth, negative operands, repeated
local reuse, and changing input. The test requires every lane to reach the
expected barrier count and every output location to receive one write.
This executes the source algorithm; it does not emulate USC instructions or
prove hardware task ordering.

The application fixture checks its lease, argument, geometry, result and fault
contracts. It injects an incorrect output, stale output on the fourth job, and
a changed guard. The application also compiles for AArch64. The existing grid
program checks preserve zero-local descriptor reproduction and code bounds.

The direct control, `apps/queue-gemm/direct_gemm.cl`, uses the same grid,
operands, four-job sequence, application API, CPU checks, and guards. Its
profile-three descriptor declares zero local words and zero barriers. Each
work item accumulates its output directly from global A and B values.
`SUNSTONE_GPU_GEMM_CONTROL=ON` selects its descriptor at application build time;
the default selects local memory. The result reports zero local bytes for the
control and 384 for the local case.

Physical acceptance requires four successful jobs, every output and guard
check, a normal return and the device cleanup evidence. A timeout retains the
service's resource and recovery obligations. The direct control passes before
the local-memory image is run.

## Source provenance and execution boundaries

Mesa revision `86c7176d2dd0065555521bf8c61f208bf53c75d6` supplies the compiler,
local allocator, barrier lowering, instruction translation, and command model.
The existing `gpu-compute-programs-mesa` record in `sources.lock.json` pins
its archive SHA-256 to
`ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58`.
The relevant source files are:

- `src/imagination/pco/pco_nir.c`, `pco_nir_io.c`, `pco_nir_sync.c`, and
  `pco_trans_nir.c` for local accesses, spill selection, and barriers.
- `src/imagination/vulkan/pvr_arch_pipeline.c` for system and local allocation.
- `src/imagination/vulkan/pvr_arch_cmd_buffer.c` for allocation encoding and
  the workgroup context-switch contract.
- `src/imagination/csbgen/rogue/cdm.xml` and
  `src/imagination/include/hwdef/rogue_hw_defs.h` for units and encoding limits.

These sources are available under their preserved MIT notices. Mesa and the
adapter execute on the host. Generated USC instructions execute on the GPU;
the native resource builder and queue execute under FreeRTOS. The existing
PowerVR firmware remains an opaque dependency documented by the GPU firmware
source record. This feature introduces no downloaded source or runtime compiler.
