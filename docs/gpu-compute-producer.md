# A733 compute context program producer

The [native context-program producer](gpu-compute-programs.md) implements the
published A733 profile, 36.56.104.183. It emits the PDS programs and register
images and embeds reproduced USC binaries. Native admission checks their
allocations and execution profile. The [reserved compute owner](gpu-compute-reserved.md)
builds the complete context and fixed numerical job for the GPU lifecycle
service. Physical execution remains unqualified.

Mesa source is pinned to revision `86c7176d2dd0065555521bf8c61f208bf53c75d6`.
The existing board driver reference remains pinned to
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Downloaded sources and SHA-256
manifests are under `~/.cache/sunstoneos/sources/gpu-compute-producer/`.

## The published profile matches the A733 identity

Mesa's [BXM-4-64 profile](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/common/device_info/bxm-4-64.h)
explicitly contains 36.56.104.183. Relevant values are one cluster per core,
512-bit cache lines and three maximum slave cores. The profile enables
multicore capability and enhancement 35421, and lacks compute Morton capability.
Enhancement 45493 is present. These settings select the ordinary shared-register
load path; the software compute PDS barrier condition is false.

The cached firmware metadata independently agrees on the common-store size,
cluster count, cache-line width and maximum slave count. That comparison reuses
`gpu-mips-20261016/comparison.json`; it does not establish the installed core
count or confirm every feature and quirk on hardware.

PDS programs launch the small USC shader programs that save and restore shared
registers. These are separate instruction streams, with separate code and data
addresses. Both must exist before the
[offline compute context](gpu-compute-context.md) can describe executable work.

## State storage has concrete source-derived sizes

The [hardware size definitions](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/csbgen/rogue/lls.xml)
and [allocation helpers](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/include/hwdef/rogue_hw_utils.h)
establish the following logical storage requirements:

| Storage | Required logical size | Alignment |
| --- | ---: | ---: |
| Compute data-master resume state | 512 bytes | 64 bytes |
| Shared-register state A | 16,432 bytes | 64 bytes |
| Shared-register state B | 16,432 bytes | 64 bytes |
| Store/load PDS code and data | Actual generated lengths for four programs | 16-byte code/data alignment; source upload also requests cache-line alignment |
| Terminate PDS code and data | Actual generated lengths for one program | 16 bytes |
| USC store/load programs | Actual generated binary lengths | Source upload requests cache-line alignment |

The resume-state calculation aligns the 72-byte hardware record to a 64-byte
cache line, then reserves four maximum cores: `128 * 4 = 512` bytes. This helper
uses maximum capacity even when runtime execution uses fewer cores.

Each shared-register buffer contains 16,384 bytes plus 48 bytes reserved for
management, including the stored allocation count. The reserve comes from
[rogue_hw_defs.h](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/include/hwdef/rogue_hw_defs.h).
The two buffers therefore require 32,864 logical bytes before allocator rounding.
These figures exclude page tables, firmware records and program allocations.

Mesa uses a 256-byte staging bound for each context PDS program, then uploads
its actual code/data lengths. A native producer must obtain those lengths from
successful generation. The staging bound is not a substitute binary size.

## Existing source generates real store, load and termination programs

The [context producer](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/vulkan/pvr_arch_job_context.c)
constructs store and load programs for both state buffers, plus a termination
program. It emits two buffer addresses into PDS data: the management area and
the shared-register area 48 bytes later. Store/load programs begin with the
compute barrier conditional sequence.

The [PDS generators](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/vulkan/pds/pvr_pds.c)
provide the actual instruction/data encoders. The relevant operations are
`pvr_pds_generate_shared_storing_program`,
`pvr_pds_generate_compute_barrier_conditional` and
`pvr_pds_generate_fence_terminate_program`. A733's selected feature branch uses
the shared-storing generator for both store and load, with distinct USC targets.

The USC source is
[common.cl](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/pco/usclib/common.cl):
`cs_store_sr_1024_common` saves shared registers and
`cs_load_sr_256_common` restores them. Mesa builds those sources into SPIR-V
through `mesa_clc`, then generates USC binaries and temporary-register metadata
through `pco_clc`. The
[USC generation target](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/pco/uscgen/meson.build)
records that chain and its required compiler setting. The common shaders use
the producer's common target, rather than a guessed replacement instruction set.

The producer packs seven static register values using
[cr.xml](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/csbgen/rogue/cr.xml).
The [Linux compute interface](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/vulkan/winsys/powervr/pvr_drm_job_compute.c)
serializes those values in the same order consumed by SunstoneOS's initializer.
The images encode PDS heap offsets; they must not receive arbitrary full GPU
virtual addresses.

## Multicore execution requires additional implementation

Mesa's [Linux device initialization](https://gitlab.freedesktop.org/mesa/mesa/-/blob/86c7176d2dd0065555521bf8c61f208bf53c75d6/src/imagination/vulkan/winsys/powervr/pvr_drm.c)
sets runtime core count to one and explicitly leaves actual core enumeration
unfinished. The context PDS setup and USC shared-register source also identify
unfinished multicore address handling. The published maximum of four cores
does not prove that this one-core program layout safely runs on every core.

The board's
[multicore driver](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/modules/gpu/img-bxm/linux/rogue_km/services/server/devices/rogue/rgxmulticore.c)
provides a concrete enumeration reference. After power admission, it reads the
32-bit core count and each core's 64-bit capability record. The pinned register
header gives offsets `0xf308` and `0xf300`; core banks use the published
register-bank-width feature. The cached firmware supplies width 19, giving a
`0x80000`-byte stride. No registers were accessed during this investigation.

Before physical use, the coordinator must establish the installed core
capabilities and a supported execution mode. The implemented producer admits the single-core construction profile. The
[native mode observer](gpu-compute-mode.md) requires actual powered core-count
and capability observations before startup and submission. Unknown or
multiple-core observations refuse that profile. Complete GPU support
still requires per-core state addressing and qualification across every core.

## Native generation and service integration are implemented

The [program implementation](../drivers/gpu/pvr_compute_programs.c) contains
reproduced USC store/load binaries, native PDS instruction and data generation,
and complete register-image construction. Its public builder validates the
selected profile, spans, heap offsets, exclusions and a required native
admission callback before writing. Fixed allocation sizes are exposed by
[pvr_compute_programs.h](../include/pvr_compute_programs.h).

The existing source comparison checks complete programs and register images
against the pinned Mesa generators. Its [evidence and reproduction procedure](gpu-compute-programs.md#compiler-reproduction-and-source-comparison)
record source, compiler, artifact and refusal-case identities. Those checks
establish offline generation, not physical execution.

The [native compute reservation](gpu-compute-reserved.md) supplies actual retained
parents and runs the program, VM, context and job builders. The
[compute service](gpu-compute-service.md) publishes those resources, serializes
submission and validates acquired output for the fixed numerical workload.
General shaders, wider workloads and any additional observed core modes remain
required. Physical recovery, startup, numerical execution and complete external
transaction drain still require qualification.
