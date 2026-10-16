# GPU firmware preparation and remaining implementation

GPU support is **partially implemented and host-tested**. Native firmware
decoding, staging, and [MIPS memory/boot preparation](gpu-mips.md) work for the
pinned A733 image. The GPU has not booted
firmware or executed a graphics or compute workload in SunstoneOS.
Physical qualification remains outstanding.
The [native firmware queue transport](gpu-queues.md) also has host evidence for
submission, result ownership, events, deadlines, and retained failures.
Native [initialization records](gpu-fw-state.md) connect the queues to backed
firmware resources and match the pinned reference initializers.

This document assumes familiarity with device drivers, memory mappings, and
ELF executable files. PowerVR identifies GPU revisions with four numbers,
called BVNC. Matching the product name alone does not establish compatibility.

## Published sources identify an implementation path

The [board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f)
contains an upstream-style driver under `bsp/drivers/gpu/imagination/` and
an older driver under `bsp/modules/gpu/img-bxm/`. The older driver's
`linux/rogue_km/build/linux/sunxi_linux/Makefile` selects **36.56.104.183**.
Its generated configuration selects the corresponding core header.
This is the board software's target; the attached GPU's identity still needs
confirmation from its registers after watchdog recovery is qualified.

[Mesa's PowerVR documentation](https://docs.mesa3d.org/drivers/powervr.html),
inspected 2026-10-16, lists BXM-4-64 revision 36.56.104.183 under active
development. Its stated Vulkan conformance for BXM-4-64 names revision
36.52.104.182. Do not transfer that conformance claim to the A733 revision.
Mesa provides a candidate shader compiler and graphics/compute runtime;
SunstoneOS still needs native memory, submission, synchronization, and recovery
interfaces. Linux DRM interfaces are development references, not runtime requirements.

References are cached under `~/.cache/sunstoneos/sources/gpu/`.
The board source manifests retain original notices, Git blob identities,
source URLs, and SHA-256 values. Format references at the pinned board revision are:

| File under `bsp/drivers/gpu/imagination/` | SHA-256 |
| --- | --- |
| `pvr_fw_info.h` | `d79454e2cb1e2dfc8e8a12ce6d455e6d18e7d9c0b33b31359ed913ee6fe187a6` |
| `pvr_fw.c` | `186dbc931f33d17c6e05bc8d92ec41735901d1e1afe621dc178bacfdfd2b46a1` |
| `pvr_fw_util.c` | `685aca8210d6d457af823121381c81572f3552eeb8be18c6a63f3e9b0a05ac69` |
| `pvr_rogue_fwif_dev_info.h` | `d3d9c39a514a0b90dcd8756e9058e89e059fc51c19670766ed30d9d0cc4c84a5` |

## Matching firmware runs on an internal MIPS processor

Imagination publishes `powervr/rogue_36.56.104.183_v1.fw` at firmware revision
`8a58f81883f7be458daa34e418cc4079f995b279`. The 131,072-byte file has SHA-256
`1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2`.
It contains ELF32 little-endian MIPS code, firmware version 1.1 build 6976702,
and a version-3 information block. Its entry is `0xbfc00001`; the low bit
selects an instruction mode, and the byte address lies in the boot-code segment.

[sources.lock.json](../sources.lock.json) pins the firmware and its accompanying
license. Fetch the two host-side artifacts with:

```sh
python3 tools/fetch-sources.py gpu-firmware gpu-firmware-license
```

The firmware remains a binary distribution under `LICENSE.powervr`; its
open-driver compatibility flag does not make the firmware source available.
Retain that license with distributed firmware. The native loader accepts an
in-memory byte span, so firmware preparation requires no runtime filesystem.

The file's final 4 KiB contains its information header and six memory-layout
entries. A separate 4 KiB device-information block precedes that footer.
Its counts describe one erratum-mask word, one enhancement-mask word,
two feature-mask words, and 29 feature-parameter words.

| Firmware section | MIPS virtual address | Allocation | Offset in allocation | Reserved bytes |
| --- | --- | --- | ---: | ---: |
| Main code | `0xc0000000` | Code | 0 | 94,208 |
| Exception code | `0x9fc02000` | Code | 94,208 | 4,096 |
| Boot code | `0xbfc00000` | Code | 98,304 | 4,096 |
| Private data | `0xc0032000` | Data | 0 | 12,288 |
| Boot data | `0xbfc01000` | Data | 12,288 | 4,096 |
| Stack | `0xcf600000` | Data | 16,384 | 4,096 |

Code totals 102,400 bytes and data totals 20,480 bytes. These are CPU staging
buffers. The [MIPS preparation component](gpu-mips.md) now constructs firmware
page tables and boot parameters from caller-supplied physical pages. Platform
allocation and GPU startup remain outstanding.

## Native staging validates bounds before changing buffers

The interface is [pvr_firmware.h](../include/pvr_firmware.h), implemented in
[pvr_firmware.c](../drivers/gpu/pvr_firmware.c). Adapted format/loading semantics
retain their [MIT notice](../drivers/gpu/LICENSE.pvr).

`pvr_fw_requirements()` checks the expected BVNC, format sizes, firmware ABI
major, page size, compatibility flags, device-information bounds, and MIPS
processor selection. It validates layout IDs, address ranges, allocation limits,
and nonoverlap. It then validates ELF load-source bounds, destination membership,
code/data permissions, load nonoverlap, and an entry within loaded boot code.
Unaligned source bytes are supported.

The component limits images to 4 MiB, each allocation to 16 MiB, and program
headers to 32. It supports the observed MIPS layout and firmware ABI major 1.
Other processors and layout families remain unsupported by this component.
It bounds device feature arrays but leaves their complete interpretation to
the future initializer. Decoding does not authenticate an image or prove
runtime compatibility; callers must preserve its pinned identity.

`pvr_fw_stage()` revalidates the source and requires two exact-sized buffers.
Buffers must be separate from the source, metadata, and each other. It
validates all inputs before writing, zeroes each complete allocation, then
copies the five ELF load segments. Refusal preserves both buffers.
The caller owns memory lifetime, physical aliases, and exclusive access.

Staging performs no MMIO, cache maintenance, allocation, or firmware execution.
It does not write the MIPS boot configuration. Loading these buffers into a
running GPU requires the remaining initialization and recovery work below.

## Focused checks cover staging and malformed-image refusal

The [C check](../checks/pvr-firmware.c) passes under address and undefined-behavior
sanitizers. It covers the pinned firmware, unaligned input, allocation guards,
39 malformed-image refusals, unchanged results on refusal, and invalid buffer
sizes and overlaps. Both complete output buffers remain unchanged on the
malformed-image cases.

The [independent comparison](../checks/pvr-firmware-reference.py) uses pyelftools
to read ELF segments and compares every byte of both native allocations,
including zero-fill and padding. Its first invocation found an incorrectly
transcribed exception-code address in the reference table. Correcting that
table produced a match without changing or repeating the native sanitizer check.

Clang 21 compiles the component for AArch64 with strict alignment and
freestanding options. The object contains 6,708 bytes of code/read-only data
and no writable static data. The plan occupies 976 bytes; calls use stack
scratch, with no heap allocation. This build is a standalone component;
the FreeRTOS GPU service has not been integrated.

Input copies, hashes, commands, sanitizer output, staged buffers, comparison,
and object are under `~/.cache/sunstoneos/evidence/gpu-firmware-20261016/`.
No device access or performance measurement occurred. Existing OS, mimus,
NPU, and recovery evidence was reused.

## Device boot and workload submission remain outstanding

The board device tree assigns GPU registers at `0x01800000`, GPU interrupt
SPI 63, DVFS interrupt SPI 64, GPU/bus clocks, a bus reset, the GPU power
domain, and regulator DCDC4. These are source descriptions, not verified
live state. Clock/voltage changes require the platform power implementation.

The next implementation steps are:

1. Implement GPU memory contexts and integrate the prepared firmware resources.
   Connect the checked command transport to shared allocations, cache
   publication, FreeRTOS serialization, interrupt/event handling, and bounded
   start/stop behavior. GPU job completion needs separate synchronization.
   Use the checked MIPS preparation component for firmware mappings and boot data;
   interpret additional feature parameters as initialization requires them.
2. Integrate platform power, clocks, reset, and identity checks after independent
   watchdog recovery is qualified. Boot the pinned firmware and verify its
   startup/completion protocol on the device.
3. Adapt the shader compiler and command-generation runtime to native services.
   Execute bounded compute and offscreen graphics with known results, then
   integrate display buffers and synchronization.
4. Qualify faults, cancellation, isolation, concurrent workloads, and power
   transitions. Optimize performance after complete board functionality,
   following [validation.md](validation.md).

[Issue 8](https://github.com/pedapudi/sunstoneos/issues/8) retains full display
and GPU acceptance requirements. Firmware staging is an intermediate component.
