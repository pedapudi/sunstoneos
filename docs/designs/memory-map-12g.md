# Design: the 12 GiB EL1 map and the EL0 read-only GPU output view

Status: **implemented** and checked in QEMU; not qualified on the board. The
measured values are in the budget table. Readers are assumed to know AArch64
stage-1 translation, the cached SMP boot path and
[memory-architecture.md](memory-architecture.md). This design applies that
design's mapping stages 2 and 3 to the images built by the cached table
builder, `platform/a733-smp-cached/memory.c`: the integrated OS image, the
other cached SMP images and the cached single-core images. The GPU and NPU
pool classes of stage 2 stay with their owners.

## Changes

- EL1 maps DRAM up to `0x3_4000_0000` in both layouts, when the per-boot
  DRAM decode check passes.
- In the OS image built with GPU linear layers, `0x46C0_0000`–`0x4700_0000`
  becomes read-only at EL0 and EL1.
- `aarch64_device_translate_el0()` translates with `AT S1E0R` and `AT S1E0W`.
- Both local pools are prepared once at boot, and their getters stop
  preparing. In images whose tables come from `memory.c` or
  `platform/a733/platform.c`, the pool check translates once per 2 MiB block
  instead of once per page.

## Data flow and ownership

Core 0 at EL3, MMU off, runs `a733_cached_memory_prepare()` once. After the
existing builder `a733_cpu_coherence_tables()` and before the table sum, it
calls `a733_dram_decode_check(ranges, count)` with the ranges it is about to
map. When the check returns 1, it writes the new entries. When it returns 0,
it writes none and preparation continues, so the image keeps today's map.
Refusing the boot would stop images that have no consumer above the current
map. No allocator, pool or task owns any byte of the new range, so mapping it
grants no memory. A later consumer, such as a stage-2 class, verifies its own
range at admission with one `AT` per block and keeps the remote-writer
prerequisite of [memory-architecture.md](memory-architecture.md).

| EL1 entries | Range | Extended layout | Local layout |
| --- | --- | --- | --- |
| level-2 64–455 | `0x4800_0000`–`0x7900_0000` | unchanged pools | `A733_EL1_DRAM_BLOCK(pa)` (new) |
| level-2 456–511 | `0x7900_0000`–`0x8000_0000` | invalid | invalid |
| level-1 2–12 | `0x8000_0000`–`0x3_4000_0000` | `A733_EL1_DRAM_BLOCK(pa)` (new) | `A733_EL1_DRAM_BLOCK(pa)` (new) |
| EL3 level-1 2–12 | same | invalid | invalid |

`A733_EL1_DRAM_BLOCK(pa)` in `include/a733_device_memory_layout.h` is Normal
write-back (MAIR index 1, `0xff`), inner shareable, access flag set, EL1
read-write, no EL0 access, UXN and PXN. The memory qualification image uses
the same word. Both layouts then map 12,174 MiB, the stage-3 figure: 910 MiB
below 4 GiB plus eleven 1 GiB blocks. The stage-2 branch under
`A733_DEVICE_MEMORY_LAYOUT >= 2`, which refuses preparation, is replaced by
this one. The layout constant stays 1, because it also names the boot-record
profile that the class owners raise.

**Decode check contract.** `include/a733_dram.h` declares
`int a733_dram_decode_check(const struct a733_dram_range *ranges, unsigned
count)`, where each range is a 64-bit physical base and size, and
`include/a733_device_memory_layout.h` includes that header. `memory.c`
builds the list from the same constant table that drives its entry writes:
`0x8000_0000`–`0x3_4000_0000` in both layouts, and
`0x4800_0000`–`0x7900_0000` first in the local layout. The check returns 1
only when, for every byte of every listed range, the DRAM controller decodes
the address linearly from `0x4000_0000` with no alias, the decoded size
reaches `0x3_4000_0000`, and the firewall grants Non-secure read and write
with no protected region overlapping. Any other result, including an
unreadable register, returns 0. Its budget is controller and firewall
register reads only, with no DRAM access, at most 64 register reads and at
most 2,000 instructions. The golden comparison records the ranges passed and
asserts that they equal the entries written in each layout.

The DRAM-controller code defines the check in
`drivers/dram/a733_dram_decode.c`. That definition makes no access and
returns 0, because no pinned source documents the DRAMC and firewall
registers ([DRAM bus priority](dram-bus-priority.md)). No image links that
file, so `memory.c` keeps a weak definition that also returns 0, and every
image links one definition. Both definitions see the one declaration
in `a733_dram.h`, so their types match. An image that links the driver file
replaces the weak definition with it. The capacity check reads each built
image's symbol table and reports which definition was linked.

**Launch evidence the gate does not cover.** A decode check that returns 1
shows that the controller decodes the range and the firewall admits
Non-secure access. It does not show that no boot0 payload, remote firmware
or bus master left running from an earlier boot stage uses
`0x8000_0000`–`0x3_4000_0000`. The new map grants no memory to any
allocator, so such a writer can corrupt only the clean lines that
speculative reads allocate there. A consumer that later takes ownership of
the range must first obtain that launch evidence, as the remote-writer
prerequisite of [memory-architecture.md](memory-architecture.md) requires.

**GPU output view.** When `A733_GPU_OUTPUT_EL0_READ_ONLY` is defined, the
builder rewrites local-layout level-2 entries 54 and 55 with AP[2:1] = 0b11,
the value the text block at `0x4100_0000` already uses, and sets UXN and PXN.
The attributes stay write-back and inner shareable. The macro is a compile
definition that `cmake/a733-os.cmake` adds in its GPU linear branch, which
already requires the CMake variable of the same name. The flag therefore
implies the mapping in every build. `memory.c` stops with `#error` if the
macro meets `SUNSTONE_A733_EXTENDED_DEVICE_MEMORY`. EL3 entries are unchanged,
because EL3 never touches the region. AArch64 stage-1 permissions cannot give
EL1 write access together with EL0 read-only access, so EL1 loses write
access. The GPU kernel owner's `DC CIVAC` completion
(`A733_GPU_OUTPUT_CLEAN_INVALIDATE`) is the matching consumer change.

**EL0 translation.** `aarch64_device_translate_el0()` in
`arch/aarch64/device_memory.c` has the signature of
`aarch64_device_translate()` and shares its sequence: mask DAIF, save
`PAR_EL1`, `AT`, `ISB`, read and restore `PAR_EL1`, restore DAIF. It issues
`AT S1E0R` or `AT S1E0W` instead of the EL1 forms. It runs at EL1 with the
MMU and data cache on, as the EL1 form requires.

**Local preparation.** A new inline `a733_memory_local_prepare()` in
`include/a733_memory_local.h` calls `a733_memory_reservations_prepare()` and
`a733_memory_uncached_prepare()`. The header holds weak definitions that
return 1, which the strong definitions in the pool sources replace, so an
unlinked pool source counts as an absent pool. Weak references would need a
GOT entry, which the native linker scripts do not place. Images that link no
pool source then link and boot unchanged. It is called once on core 0 at
EL1, after the tables are installed and before the scheduler starts: in
`platform_init()` of `platform/a733/platform.c` for single-core images, and
on the core-0 path of `platform/a733-smp-cached/continue.c` for cached
images. A refusal stops the boot as an extended-preparation refusal does.
The pool initialization publishes the pool pointer with a release store, and
each getter returns that pointer after one load-acquire. Before
preparation, after a refusal and when the image links no pool it returns
NULL.

Extended-layout images do not call it, because `a733_device_memory_prepare()`
prepares both pools inside its binding state machine; a second preparation
outside it makes that function refuse. `platform.c` compiles the call only
without `SUNSTONE_A733_EXTENDED_DEVICE_MEMORY`, which the GPU single-core
compute, draw and texture images inherit from the GPU render image. The
capacity check's image scan finds every `B` and `BL` to a pool preparation
in each built image and fails when an image that links
`a733_device_memory_prepare()` makes one from any other function.

In the QEMU fixtures on `platform/qemu-smp/platform.c`, the preparation runs
after the secondary cores report online. Each secondary core then waits at
most one second for the primary to start the scheduler. Those fixtures use
the per-page stride, so a 2 MiB pool and the uncached pool cost 1,024
translations, about 50,000 instructions, which TCG runs in milliseconds even
on a loaded host.

**Local pool check stride.** `a733_memory_local_check()` in
`include/a733_memory_critical.h` translates once at the pool start and once
at each following multiple of `a733_local_pool_stride` below the end. The
stride is chosen at link time. `memory.c` defines it as 2 MiB, because it
maps image RAM, and so both local pools, with 2 MiB level-2 block
descriptors; it writes entry 14 for the uncached pool, for example.
`platform/a733/platform.c` also maps image RAM with 2 MiB level-2 blocks
(entries 0–63, and the uncached pool through `a733_memory_boot_uncached()`),
so it defines the same 2 MiB value, except in cached single-core images,
which take their tables and the stride from `memory.c`. A weak definition of
4096 beside the check, which checks every page, serves every other image,
such as the QEMU fixtures. The host check links a test definition for its
2 MiB cases. A file that defines the stride does not include
`a733_memory_critical.h`; it sees the declaration in
`a733_device_memory_layout.h`. The golden local-layout comparison proves that
each checked entry of `memory.c` is a block descriptor; `platform.c` writes
only block descriptors in level 2 and has no comparison.

## Interrupts, synchronization and cache policy

Nothing here takes an interrupt or polls. Table writes happen on core 0
before the existing record barrier in `continue.c` releases the other cores.
The new entries enter the table sum, so each core's readback check covers
them. The tables do not change after preparation, so no TLB maintenance is
added. The local preparation runs before the scheduler, which orders it
before every task on every core. `a733_memory_prepare_once()` keeps its
critical section, acquire load and release store unchanged: the prepare
functions stay public and fixtures call them directly, so the lock still
serializes any caller outside the boot path. Getters never take it.

Table pages are cleaned and invalidated before the first descriptor write by
the existing `a733_cpu_coherence_flush()` call; the page set is unchanged. A
newly mapped write-back block can receive speculative reads, which allocate
clean lines only. A device that later writes the range follows the pool rule
of [memory-architecture.md](memory-architecture.md): invalidate the written
bytes before the CPU reads them.

## Reused and removed

- Reused: the single table builder and its later-writer scan, the QEMU
  trace parser and `instruction_sites()` of `checks/harness.py`,
  `A733_EL1_DRAM_BLOCK`, the table sum, `a733_cpu_coherence_flush()`, the
  `TRANSLATE` sequence of `aarch64_device_translate()`, and the QEMU golden
  comparison in `checks/device-memory-capacity.{c,py}`.
- Added to that comparison: level-1 entries 2–12 for both decode results,
  and a second build of `memory.c` in the local layout with and without the
  uncached pool and the read-only view. A further case runs `AT S1E0R` and
  `AT S1E0W` on a read-only block and on an EL1-only block.
- Removed: the lazy preparation in both local getters, and the stage-2
  mapping branch that refused preparation.

## Performance budgets

Instruction and barrier counts use the QEMU fixtures under `-icount shift=0`
with an execution trace, as in [memory-architecture.md](memory-architecture.md).

Baseline values come from the same fixtures built against the integrated
tree before the change (revision `dfa5174`), with the decode-check
assertions removed because that builder never calls the check. Trace counts
vary by up to about ten instructions between runs because the coherence flush
polls the counter.

| Metric | Baseline | Target | Measured | Method |
| --- | --- | --- | --- | --- |
| DRAM mapped at EL1 | 910 MiB extended, 126 MiB local | 12,174 MiB in both when the check passes; baseline when it fails | 12,174 MiB in both when it passes; 910 and 126 MiB when it fails | golden comparison |
| Translation-table memory | 20,544 bytes | unchanged, under 64 KiB | 20,544 bytes in every fixture image | `__tables_end - __tables_start` |
| Descriptor stores added at boot | 0 | 11 extended, 403 local, plus 2 for the read-only view | 11 extended, 403 local (valid blocks 455 to 466 and 63 to 466), 2 for the view | fixture count and golden comparison |
| `AT`, `DC` and `TLBI` added at boot | 0 | 0 | 0 `AT`, 0 `TLBI`, 321 `DC` with and without the map | execution trace of `a733_cached_memory_prepare()` |
| Instructions in `a733_cached_memory_prepare()`, decode check excluded | 20,699 extended, 15,636 local | at most 2,000 above baseline | 20,785 extended (+86); 16,882 local (+1,246); 39 and 1,227 above the decode-fail path | QEMU execution trace |
| Extended pool check | 392 `AT` | 392 `AT` | 392 `AT`; 22,541 instructions | translation callback count |
| Local pool check, tables from `memory.c` | 1,024 `AT` (1,536 with the large NPU graph) at the first getter call | 2 `AT` (2 to 3 with the large NPU graph), once at boot | 3 for an unaligned 2 MiB pool and the uncached pool, 2 for a 2 MiB-aligned pool, 3 with the large NPU graph, 1 for a 64 KiB pool alone | host callback count, QEMU trace |
| Local pool check, tables from `platform.c` | 1,024 `AT` (1,536 with the large NPU graph) at the first getter call | 2 `AT` (2 to 3 with the large NPU graph), once at boot | same as tables from `memory.c`: the stride is the same 2 MiB definition | host callback count |
| Local getter after preparation | 17 instructions, one load-acquire | at most 8 instructions, one load-acquire, no barrier | 5 instructions in the QEMU SMP fixture (PMU count); 6 in the local-layout fixture trace, which also counts one caller instruction; one load-acquire, no barrier | QEMU SMP fixture, local-layout fixture trace |
| Critical-section hold of first local preparation | 344 instructions at the first getter call | 344 plus the publishing store, at most 347, once at boot | 347 instructions, once at boot | QEMU SMP fixture |
| `a733_dram_decode_check()` | none | controller and firewall register reads only, no DRAM access; at most 64 register reads and 2,000 instructions | no access; 2 instructions (8 bytes) in the driver and in the weak default | `checks/dram-controller.py` code size |
| `aarch64_device_translate_el0()` per call | none | equal to `aarch64_device_translate()`; one `ISB`, no `DSB` | 48 instructions in both functions; traced call 40 against 41, one `AT`, one `ISB`, no `DSB` | QEMU fixture |

The getter loads a pool pointer that the preparation publishes with a release
store, instead of the kept result and the pool's initialized flag; that change
brings the getter from 8 instructions to 4 plus the call.

## Sources and unproven sequences

- DRAM base `0x4000_0000`: `SUNXI_DRAM_BASE` in the pinned TF-A
  `plat/allwinner/sun60i_a733/include/sunxi_mmap.h`. The 12 GiB extent
  follows [memory-qualification.md](../memory-qualification.md); linear decode
  without aliasing is unproven until that image's address test runs.
- The pinned vendor `sun60iw2p1.dtsi` reserves `0x4800_0000`–`0x4900_0000`
  for the vendor BL31. The SunstoneOS TOC1 package omits that monitor
  ([remote-writers.md](../remote-writers.md)).
- Descriptor fields, AP encodings, `AT S1E0R`/`AT S1E0W` and the `PAR_EL1`
  layout come from the Arm Architecture Reference Manual (DDI 0487), which is
  not pinned. The encodings repeat ones already in the tree: the text block's
  AP value, `A733_EL1_DRAM_BLOCK` and the `PAR_EL1` decode of
  `aarch64_device_translate()`.
- **Unproven:** the boot0 firewall registers and the 120 MiB secure region
  have no pinned source; the decode check carries that risk. The response of
  a Cortex-A55 or A76 linefill to a firewall refusal, which could raise an
  SError, has no pinned source. `DC IVAC` faulting on a read-only page comes
  from DDI 0487 as quoted in [recognition-dispatch.md](recognition-dispatch.md).

## Left out

- GPU and NPU pool classes, boot-record profile 2 and
  `DEVICE_MEMORY_MAX_PAGES`: the class owners' stage 2.
- `0x7900_0000`–`0x8000_0000` and the monitor block: no owner, and the boot0
  secure region is not located.
- EL3 entries above `0x8000_0000`: EL3 does not access that range.
- A query that reports whether the high map exists: no consumer; a consumer
  translates its own range at admission.
- An EL0 read-only rule in `a733_mapping_check.h`: the shared-helper owner.
- The 128 MiB map of the non-cached returning images: they are neither OS nor
  cached images, and the memory qualification image already maps all DRAM.

A stray EL1 store above `0x4800_0000` no longer faults once the high map is
installed. EL0 has no access to the new range.

## Board evidence required

- The decode check returns 1 on the board, and the memory qualification
  image's address test passes over its 12,224 MiB tested map.
- One boot of each image class with the high map installed and SError
  unmasked reaches the scheduler with no SError.
- In the GPU linear OS image, EL0 and EL1 stores into `0x46C0_0000` raise
  permission faults, `DC CIVAC` on the region completes, and the
  `mimus_board_pins` check passes.
- `CPU_CYCLES` around `a733_cached_memory_prepare()` on core 0 replaces the
  instruction estimate.

## Changes outside the owned files

These go to the primary agent as one patch:

- `platform/a733-smp-cached/continue.c`: call `a733_memory_local_prepare()` on
  core 0 in the branch without `SUNSTONE_A733_EXTENDED_DEVICE_MEMORY`, before
  `a733_smp_primary_el1()`.
- `platform/qemu-smp/platform.c`: the same call at the end of
  `platform_init()`, for the QEMU fixtures that link local pools.
- `cmake/a733-os.cmake`: `A733_GPU_OUTPUT_EL0_READ_ONLY=1` as a compile
  definition of `sunstone-a733-os` in the GPU linear branch.
- `checks/harness.py`: the QEMU trace parser and the barrier classifier that
  `checks/device-memory-capacity.py` and `checks/memory-architecture-smp.py`
  share.
- `apps/storage-read-single-core/platform.patch` needed no change: it still
  applies with `--fuzz=0` at an offset.
- `checks/cached-device-reservations.c`, the one fixture that calls a local
  getter on neither platform path, calls `a733_memory_local_prepare()` on core
  0 as `continue.c` does; it is an owned file.

These go to other owners as requests:

- DRAM-controller code: make `a733_dram_decode_check()` read the controller
  and firewall registers to the contract above once a register source
  exists, and link `drivers/dram/a733_dram_decode.c` into the images that
  build `memory.c`.
- Owners of [cpu-table-walks.md](cpu-table-walks.md) and
  [cpu-coherence.md](../cpu-coherence.md): replace the
  `0x8000_0000`–`0xFFFF_FFFF` requirement with this contract, and remove the
  text that has `memory.c` refuse preparation in a branch compiled out at
  layout profile 1, which this design replaces.
