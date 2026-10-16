# A733 compute context program producer

A native producer can use published Mesa sources for the A733's exact GPU
identity, 36.56.104.183. The required program sources and allocation rules are
available. Native program generation and semantic admission are **not yet
implemented**. This investigation establishes the next bounded implementation
step; it supplies no executable GPU artifact or physical qualification.

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
capabilities and a supported execution mode. The first producer may admit a
proven single-core mode and reject unknown or multiple-core execution. It must
not copy Mesa's assumed count into hardware admission. Complete GPU support
still requires per-core state addressing and qualification across every core.

## The next implementation must emit and admit a complete program bundle

The next component should expose `pvr_compute_programs_require` and
`pvr_compute_programs_build` for one fixed A733 feature profile. These are
proposed APIs; no declarations or placeholder implementation have been added.

1. Reproduce the two USC binaries with the pinned Mesa tools. Record nonempty
   binary lengths, temporary-register counts, source/tool hashes and licenses.
   Retain immutable reproducible artifacts for runtime use; a general shader
   compiler is not needed inside FreeRTOS for these fixed context programs.
2. Adapt the selected PDS generator branch to caller-owned offline storage.
   Generate the four store/load programs and one termination program, using
   admitted USC offsets and both state-buffer addresses. Derive exact lengths
   and the seven register images from the resulting programs.
3. Validate profile/core mode, code/data sizes, alignment, encoded address
   widths, allocation leases, VM permissions and every known overlap before
   writing. The output descriptor must retain every code, data and state lease.
   Include the 512-byte resume buffer needed by later compute commands.
4. Compare complete instruction words, data words and register images against
   unchanged upstream generation for this profile. Add focused refusal cases
   for malformed offsets, overlap and unsupported core mode. Check every output
   guard and preserve all bytes on admission refusal.
5. Pass the admitted register images to the existing context initializer.
   Keep publication, context attachment, compute command construction,
   completion fences and cleanup as separate operations with retained ownership.

This sequence produces a usable, inspectable program bundle before adding a
larger compiler interface. Hardware execution remains blocked on the physical
prerequisites, especially recovery, core-mode admission, mappings and isolation.
The investigation performed source inspection and size derivation only; it did
not build the Mesa compiler, emit USC/PDS binaries, run performance measurements,
or submit a board test.
