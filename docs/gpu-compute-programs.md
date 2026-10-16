# Offline compute context programs

The native producer constructs the code and register images needed to save and
restore a private A733 compute context. It is implemented and checked against
pinned Mesa output. GPU execution and single-core admission remain unqualified.
The public interface is [pvr_compute_programs.h](../include/pvr_compute_programs.h).

The Unified Shading Cluster (USC) executes two embedded shared-register programs.
The Programmable Data Sequencer (PDS) supplies their state addresses and starts
them. Four PDS programs select store or load for each of two state buffers.
A fifth PDS program terminates the context task and frees shared partitions.

## Programs and allocation requirements

The producer accepts only BVNC 36.56.104.183 and proven single-core execution.
BVNC identifies the GPU hardware revision. Its selected feature branch uses
conditional USC dispatch, ordinary compute barriers, and no compute Morton mode.
The driver does not offer unchecked feature switches.

| Allocation | Emitted content | Required allocation bytes |
| --- | --- | ---: |
| USC store | 544 code bytes; 10 temporary registers | 576 |
| USC load | 488 code bytes; 10 temporary registers | 576 |
| PDS store/load, for each state buffer | 64 data bytes and 24 code bytes | 128 each |
| PDS termination | 16 data bytes and 20 code bytes | 64 |
| Shared state, for each of two buffers | 48 reserved bytes and 16,384 state bytes | 16,448 each |
| Compute Data Master resume state | Maximum-core layout from the selected profile | 512 |

Every GPU allocation begins on a 64-byte boundary. USC allocations include the
source-required 32 bytes of instruction prefetch space. The producer rounds
allocation sizes to cache lines and clears all padding. The complete allocation
set occupies 35,136 bytes. CPU byte views may be unaligned; configuration and
result records require their natural C alignment.

The two immutable USC binaries serve both state buffers. Their instruction bytes
come from Mesa's `cs_store_sr_1024_common` and `cs_load_sr_256_common` kernels.
The producer retains the compiler's temporary-register metadata. It emits the
PDS address constants and fixed instruction sequence natively, without a runtime
shader compiler. The seven resulting register images use the order accepted by
[pvr_compute_context_build](gpu-compute-context.md).

PDS registers contain offsets relative to the PDS heap. USC dispatch contains an
offset relative to the USC heap. Shared-state constants contain full GPU virtual
addresses. The resume address is returned separately for later context/job
integration. Constructing these bytes does not publish or attach a context.

## Admission and ownership

The caller supplies three disjoint GPU heaps: PDS, USC, and general memory.
PDS and USC heaps cannot exceed their 32-bit offset ranges. All GPU spans must fit
the 40-bit virtual address space. Shared-state allocations belong to the PDS heap;
resume state belongs to general memory.

The caller also supplies CPU and GPU ranges for the compute context, firmware
register state, client command control and ring, and VM memory record. It appends
other known conflicting allocations, up to sixteen exclusions. The producer
rejects overlapping owned spans, exclusion conflicts, truncated allocations,
overflow, invalid heap offsets, and input/result aliases before writing bytes.
These logical checks cannot detect physical aliases or prove VM permissions.

A required privileged admission callback validates a nonzero lifetime token.
It must prove all of the following before returning success:

- The active device and feature profile match the selected branch.
- Only one GPU core can execute this context for the token's lifetime.
- The context is unattached, and all supplied storage is exclusively owned.
- CPU views and GPU mappings describe the same backing with suitable permissions.
- Heap bases match the eventual USC/PDS hardware configuration.
- All backing reservations and physical aliases are excluded.

The callback must be bounded and must preserve inputs and outputs. Its context
must be disjoint from every output. The caller holds the admission proof and
storage until independent completion and cleanup authorize reuse. Publication,
cache maintenance, firmware attachment, submission, completion, and isolation
remain separate operations. Power OFF does not prove memory transactions drained.

No production implementation of the single-core proof exists in this component.
Mesa's runtime placeholder of one core does not establish that proof. Both the
published PDS setup and USC kernels lack per-core state addressing. Multicore
program generation and physical qualification remain required for full hardware
support. The 512-byte resume allocation preserves the profile's maximum-core
layout; it does not make shared-register programs multicore-safe.

## Compiler reproduction and source comparison

The source is Mesa revision `86c7176d2dd0065555521bf8c61f208bf53c75d6`.
The [source archive](https://gitlab.freedesktop.org/mesa/mesa/-/archive/86c7176d2dd0065555521bf8c61f208bf53c75d6/mesa-86c7176d2dd0065555521bf8c61f208bf53c75d6.tar.gz)
has SHA-256 `ca5e3ac4b61b8dc659cc489c7eb3c7bcc750f6820fbf39f0b831ac87fb00da58`.
The implementation preserves the applicable MIT notices.

The compiler build uses LLVM/Clang 21.1.8, SPIR-V tools 2026.1, the LLVM/SPIR-V
translator package 21.1.5, Meson 1.12.1, Ninja 1.13.2, and Python 3.14.
Development libraries were extracted into a private cache on the development
host. No system package installation was required.

Configure the pinned source with the following Meson options, then build only
`src/imagination/pco/uscgen/pco_uscgen_programs.c` with Ninja:

```text
-Dbuildtype=release -Dgallium-drivers=[] -Dvulkan-drivers=[]
-Dtools=imagination -Dplatforms=[] -Dglx=disabled -Degl=disabled
-Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled
-Dshared-llvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled
-Dinstall-mesa-clc=true -Dbuild-tests=false -Dvideo-codecs=[]
-Dshader-cache=disabled -Dglvnd=disabled --wrap-mode=nodownload
```

This builds `mesa_clc`, compiles the published OpenCL kernels to SPIR-V, and runs
`pco_clc` with the upstream `PCO_DEBUG=no_pred_cf` setting. Kernels ending in
`_common` use Mesa's common instruction profile. No unsupported device option or
substitute shader was supplied. The resulting USC code has these identities:

| Program | SHA-256 |
| --- | --- |
| Store | `e714ee57e8df229297c96d8a25cbdff5059c945e9414302b913c76ad22ad5992` |
| Load | `ac7e9fd6a11d9acf7bfeae7ef3fc3faa3e6109431b56be6e2130bcc9e4878eed` |

The focused check consumes the existing compiler output; it never rebuilds the
compiler automatically:

```sh
python3 checks/pvr-compute-programs.py \
  --mesa-source ~/.cache/sunstoneos/gpu-producer-build/mesa \
  --mesa-build ~/.cache/sunstoneos/gpu-producer-build/build \
  --evidence ~/.cache/sunstoneos/evidence/gpu-compute-programs-20261016
```

The native and reference outputs match at ordinary and near-limit heap offsets.
Each comparison covers all allocations and the 64-byte result record: 35,200
bytes. The reference uses unchanged PDS routines, generated register packers,
and the compiler's original metadata structure and USC arrays. The fixture
clears padding and state allocations for deterministic comparison; those zeros
are the native allocation policy, not additional upstream program instructions.

The sanitizer check passes 109 atomic refusals plus five alias/null cases.
A later focused check covers two typed-record alignment refusals. The final
freestanding AArch64 object compiles with strict alignment and uses 96 bytes of
static stack in the builder, excluding its external admission callback.
The full byte comparison was reused after adding alignment guards; only the
changed refusal path ran again. No board access or performance measurement ran.

Evidence resides in `~/.cache/sunstoneos/evidence/gpu-compute-programs-20261016/`.
It records source and tool identities, compiler configuration, generated artifact
hashes, exact check commands, comparison hashes, and final target inputs.
Link `drivers/gpu/pvr_compute_programs.c` into the eventual privileged GPU owner;
its only executable dependency is the caller's admission callback. Physical
recovery, admission, memory publication, context attachment, job completion,
shutdown integration, and multicore addressing remain prerequisites.
