# Design: the 12 GiB EL1 map and the EL0 read-only GPU output view

Status: **implemented** and checked in QEMU; not qualified on the board. The address-test definition of the decode check is backed by a board run of the memory qualification image's address test (see "Address-test definition"), but no OS image with the high map has booted on a board. Readers are assumed to know AArch64 stage-1 translation, the cached SMP boot path and [memory-architecture.md](memory-architecture.md). This design applies that design's mapping stages 2 and 3 to the images built by the cached table builder, `platform/a733-smp-cached/memory.c`: the integrated OS image, the other cached SMP images and the cached single-core images. The GPU and NPU pool classes of stage 2 stay with their owners.

## Behavior

- EL1 maps DRAM up to `0x3_4000_0000` in both layouts, when the per-boot DRAM decode check passes.
- In the OS image built with GPU linear layers, `0x46C0_0000`–`0x4700_0000` is read-only at EL0 and EL1.
- `aarch64_device_translate_el0()` translates with `AT S1E0R` and `AT S1E0W`.
- Both local pools are prepared once at boot, and their getters do not prepare. In images whose tables come from `memory.c` or `platform/a733/platform.c`, the pool check translates once per 2 MiB block instead of once per page.

## Data flow and ownership

Core 0 at EL3, MMU off, runs `a733_cached_memory_prepare()` once. After the
existing builder `a733_cpu_coherence_tables()` and before the table sum, it
calls `a733_dram_decode_check(ranges, count)` with the ranges it is about to
map. When the check returns 1, it writes the new entries. When it returns 0,
it writes none and preparation continues, so the image keeps the map below `0x4800_0000` and its extended pools.
Refusing the boot would stop images that have no consumer above that map. No allocator, pool or task owns any byte of the new range, so mapping it
grants no memory. A later consumer, such as a stage-2 class, verifies its own
range at admission with one `AT` per block and keeps the remote-writer
prerequisite of [memory-architecture.md](memory-architecture.md).

| EL1 entries | Range | Extended layout | Local layout |
| --- | --- | --- | --- |
| level-2 64–455 | `0x4800_0000`–`0x7900_0000` | unchanged pools | `A733_EL1_DRAM_BLOCK(pa)` |
| level-2 456–511 | `0x7900_0000`–`0x8000_0000` | invalid | invalid |
| level-1 2–12 | `0x8000_0000`–`0x3_4000_0000` | `A733_EL1_DRAM_BLOCK(pa)` | `A733_EL1_DRAM_BLOCK(pa)` |
| EL3 level-1 2–12 | same | 1 GiB blocks in the OS image, invalid elsewhere | 1 GiB blocks in the OS image, invalid elsewhere |

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

**Address-test definition.** `drivers/dram/a733_dram_decode_probe.c` defines
the check by an address test and links into cached images when the CMake
option `SUNSTONE_A733_DRAM_DECODE_PROBE` is on. The memory qualification
image's run on port 3-1 passed its window scan over 6,258,688 words and its
page signature over 3,129,344 pages in `0x4400_0000`–`0x3_4000_0000`, with no
page holding another page's signature (an alias) and no beacon; the training
records of both boards report 12,288 MiB. The definition refuses a range
outside that tested map. On the core 0 EL3 path it then writes a distinct token
to the first and last word of each range, to every 2 MiB step of the first
gigabyte, to each power-of-two offset from 2 MiB to 8 GiB and to each gigabyte
boundary, issues one `DSB SY`, and reads every token back. About 600 probes
cover the 11 GiB range. An aliased, wrapped, stuck or absent address returns
another value and the check returns 0. The test runs with the MMU off, so it
shows decode, not Non-secure firewall access; the qualification run showed
that for the tested map. The host check `checks/dram-decode-probe.py` runs the
real definition against a memory model with seven decode faults. The
definition in `a733_dram_decode.c` stays for a controller register source.

**Launch evidence the gate does not cover.** A decode check that returns 1
shows that the controller decodes the range and the firewall admits
Non-secure access. It does not show that no boot0 payload, remote firmware
or bus master left running from an earlier boot stage uses
`0x8000_0000`–`0x3_4000_0000`. The map grants no memory to any
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

**Local preparation.** The inline `a733_memory_local_prepare()` in
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


## Qualified figures

QEMU execution traces and the golden comparison (record `memory-map-12g-qemu` in [the evidence file](../evidence/memory-map-12g.jsonl)); no board measurement.

| Metric | Figure |
| --- | --- |
| DRAM mapped at EL1 | 12,174 MiB in both layouts when the check passes; 910 MiB (extended) and 126 MiB (local) when it fails |
| Translation-table memory | 20,544 bytes in every fixture image |
| Descriptor stores added at boot | 11 extended, 403 local, plus 2 for the read-only view |
| `AT` and `TLBI` added at boot | 0 and 0 (321 `DC` with and without the map) |
| Instructions in `a733_cached_memory_prepare()`, decode check excluded | 20,785 extended, 16,882 local |
| Local pool check | 2 or 3 `AT` once at boot (3 for an unaligned 2 MiB pool and the uncached pool; 1 for a 64 KiB pool alone) |
| Local getter after preparation | 5 instructions, one load-acquire, no barrier |
| `aarch64_device_translate_el0()` | 48 instructions, one `AT`, one `ISB`, no `DSB` |

## Open work

1. **Board evidence.** The decode check returns 1 on the board, and the memory qualification image's address test passes over its 12,224 MiB tested map. One boot of each image class with the high map and SError unmasked reaches the scheduler with no SError. In the GPU linear OS image, EL0 and EL1 stores into `0x46C0_0000` raise permission faults, `DC CIVAC` on the region completes, and the `mimus_board_pins` check passes. `CPU_CYCLES` around `a733_cached_memory_prepare()` on core 0 replaces the instruction estimate. The prediction is the record `memory-map-12g-board`.
2. **DRAM-controller code.** Make `a733_dram_decode_check()` read the controller and firewall registers to the contract above once a register source exists, and link `drivers/dram/a733_dram_decode.c` into the images that build `memory.c`.
3. **Other designs.** [cpu-table-walks.md](cpu-table-walks.md) and [cpu-coherence.md](../cpu-coherence.md) should replace their `0x8000_0000`–`0xFFFF_FFFF` requirement with this contract.

The OS image maps every gigabyte and every 2 MiB block of DRAM that the decode check passed at EL3 too, as normal write-back memory with the attributes of a level-2 block, because the return cleans at EL3 the device-visible ranges that the OS publishes (`apps/os/device-ranges.h`), and the OS chooses them at run time. The fixtures that build the extended layout map nothing there.
