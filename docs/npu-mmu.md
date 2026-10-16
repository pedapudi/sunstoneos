# Native NPU page-table construction

The native NPU memory component is **implemented** for offline page-table
construction and **tested** with a host table walker. It compiles for AArch64.
Physical translation, MMU activation, cache publication, fault interrupts, and
inference remain untested. No NPU register access occurs in the builder.

The component prepares device virtual addresses for command, weight, scratch,
and tensor buffers. It is independent of the CPU page tables and DMA controller.
The [NPU implementation record](npu.md) retains the wider driver and application
requirements.

## Address translation and table placement

The [interface](../include/a733_npu_mmu.h) accepts physical memory ranges, device
virtual addresses, page sizes, and write permissions. The
[builder](../drivers/npu/a733_npu_mmu.c) generates a directory and second-level
tables in caller-owned memory. It allocates no heap memory and makes no OS calls.

| Property | Implemented behavior |
| --- | --- |
| Device virtual addresses | 32 bits; address zero remains unmapped |
| Physical addresses | 40 bits, including addresses above 4 GiB |
| Page sizes | 4 KiB, 64 KiB, 1 MiB, 16 MiB |
| Directory | 256 entries, each covering 16 MiB |
| Table arena | 4 KiB-aligned CPU and physical bases; capacity aligned to 64 bytes |
| Table placement | Directory and all second-level tables share physical bits 39–32 |
| Permissions | Read-only or readable/writable |
| Missing entries | Non-present, exception-enabled entry value `2` |
| Mapping limit | At most 512 input ranges per build |
| Maximum arena | 4,198,400 bytes, including a 4 KiB directory reservation |

A directory entry selects one page size for its entire 16 MiB range. The builder
rejects attempts to mix page sizes within that range. Second-level storage is
compact: each table uses its required entry count, rounded up to 64 bytes.
The first 4 KiB reserve the directory and alignment padding.

Each leaf entry stores physical bits 31–12 directly and physical bits 39–32 in
entry bits 11–4. Its low bits encode present, exception, and write permission.
The result also contains the directory value used by the reference driver's
page-directory register mode. Returning this value does not program the MMU.

## Ownership and refusal behavior

The caller must provide a reserved arena that no device can access during the
build. Input ranges must remain stable throughout the call. The caller must
pin all backing memory and verify the physical addresses before submission.
The builder cannot discover CPU aliases or establish that a physical address
belongs to usable RAM.

Virtual mappings cannot overlap. Physical aliases are accepted only when both
mappings are read-only. Payload mappings cannot expose any part of the reserved
table arena, including unused capacity. The arena, mapping metadata, and result
must occupy disjoint CPU address ranges.

All validation precedes mutation. Invalid ranges, alignment, permissions,
address overflow, page-size conflicts, aliases, or insufficient table capacity
leave both the arena and result unchanged. Successful construction writes only
the reported image length. Rebuilding an offline image clears previous entries;
it does not invalidate translations held by a running NPU.

A production service must serialize table ownership, clean CPU caches, order
writes, establish device quiescence, and complete the NPU's translation-cache
invalidation before changing an active address space. Table and payload memory
must remain reserved after a failed stop until quiescence is proven. These
runtime transitions are not implemented by this builder.

The reference driver's internal-SRAM virtual window and MMU-flush command
storage need separate setup. The builder does not infer either mapping from a
model's declared SRAM use. The 40-bit virtual-address mode is also unimplemented;
the expected A733 feature entry specifies 32-bit device virtual addresses.

## Reference and adaptation

The source revision is the board kernel pinned in [npu.md](npu.md).
The native table format follows these files under
`bsp/drivers/npu/aw_nna_vip/vip2/`:

| File | SHA-256 | Relevant implementation |
| --- | --- | --- |
| `memory/vip_drv_mmu.c` | `4f6cd9705c915682f15653ba369cc7e3674cc20a886d650b64da29fdfbf04723` | Page encoding, same-upper-byte allocation constraint, directory alignment |
| `memory/vip_drv_mmu.h` | `970d1cc4f3f2a78224363d690b7b3e83c440267cbca299023c940304e749334d` | Table geometry, page sizes, entry flags |
| `vip_drv_hardware.c` | `c02d81df77869f2b2b3c9b96cb8390723fee3bc03b1acf3740b3ce6ebec0983f` | Page-directory register encoding |

The adapted implementation retains the source's MIT notice. The native allocator
and admission policy replace the reference driver's Linux allocation paths.
The native builder fills absent entries with the reference exception-enabled
free-entry value. Hardware fault behavior still needs physical qualification.

## Focused evidence

The [host check](../checks/npu-mmu.c) interprets the generated directory and leaf
tables independently of the builder's allocation helpers. It checks translated
addresses and permission faults, including every upper physical-address byte,
the final byte of the 32-bit virtual range, and all supported page sizes.

The same run checks noncontiguous physical backing, read-only sharing,
translations across directory boundaries, unmapped guard pages, compact table
storage, and removal of previous mappings during offline reconstruction.
Fault cases verify atomic refusal for aliases, mixed page sizes, table exposure,
address overflow, invalid alignment and permissions, metadata overlap, and
insufficient capacity. AddressSanitizer and UndefinedBehaviorSanitizer report
no error.

One host run passed. The AArch64 object then compiled successfully after CMake
regenerated its target list. The initial build command could not find the new
target; no source or test change was needed. Existing DMA, model-inspection,
and mimus evidence remains reusable because their implementation inputs did
not change.

Local evidence is under `~/.cache/sunstoneos/evidence/npu-mmu-20261016/`.
It records commands, logs, compiler inputs, source identities, and artifacts.
The compiled object has SHA-256
`4d8bf0f9fd680ef9c7ab3614ec5e9c0a80dbb8cfd78f3c38405a22988754ce17`.
It contains 1,744 bytes of code, no static data, and an unresolved `memset`
reference supplied by the runtime. Its disassembled function reserves 3,184
stack bytes, excluding the called `memset` implementation's stack use.
These are object measurements, not final image or runtime measurements.

Reproduce the host check with:

```sh
clang-21 -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  checks/npu-mmu.c drivers/npu/a733_npu_mmu.c -o /tmp/npu-mmu-check
/tmp/npu-mmu-check
```

After configuring the existing AArch64 build as described in [build.md](build.md),
compile only this component with:

```sh
cmake -S . -B "$HOME/.cache/sunstoneos/build/qemu-virt"
cmake --build "$HOME/.cache/sunstoneos/build/qemu-virt" --target a733-npu-mmu
```

The object target is excluded from default builds. It is not linked into a
board image yet. The [controller core](npu-controller.md) supplies modeled register sequences
and ownership transitions. The next integration step is its FreeRTOS service,
including native buffer publication, platform resources, and fault recovery.
Physical work remains gated on independent watchdog recovery.
