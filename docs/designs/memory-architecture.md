# Design: device memory classes, boot mapping checks and protection

Status: **implemented**, checked in QEMU and on the host, for the extended-layout pool checks, the serialized ledger, class alignment, the lock-free describe and the clean watermark (stage 1). **Open**: the `gpu` and `npu` 1 GiB classes (stage 2), pool ownership of DRAM above 4 GiB (stage 3), and every cycle count, which needs the board runs listed under open work. Nothing here is qualified on the board.

Readers are assumed to know AArch64 stage-1 translation, the FreeRTOS SMP port and non-coherent DMA.

Ten images have a 768 MiB write-back device pool, `dma32`, and a 16 MiB Normal non-cacheable pool, `uncached`. Boot checks the extended pools with one address translation (`AT`) per 2 MiB block. Acquire, release and retain are serialized with the kernel's critical section, while the per-access lease check (describe) stays lock-free. Large `dma32` extents align to the GPU MMU's largest page, and the clean after acquire lives in the pool and covers only pages that can hold dirty lines.

## Map and protection

Tasks start at EL0 unless the image selects EL1, and EL0 tasks call kernel functions directly, so kernel data, stacks and heap stay EL0 read-write. MMIO has no EL0 access in either layout, so every lease holder that programs a device is an EL1 task. The extended layout also gives its pools no EL0 access. The local layout places its pools inside EL0 read-write data blocks.

| Physical range | Use | EL1 descriptor | EL0 |
| --- | --- | --- | --- |
| `0x0`–`0x4000_0000` | MMIO | 1 GiB block, Device-nGnRnE, XN | none |
| `0x4000_0000`–`0x4100_0000` | boot0 and launcher | 2 MiB blocks, WB, XN | read-write |
| `0x4100_0000`–`0x4120_0000` | text and read-only data | 1 block, WB, read-only | read-only, executable |
| `0x4120_0000`–`0x4200_0000` | data, BSS, stacks, heap | WB, XN | read-write |
| `0x4200_0000`–`0x4220_0000` | monitor and tables | invalid | none |
| `0x4220_0000`–`0x4800_0000` | boot assets; recognizer models, input, arena | WB, XN | read-write |
| `0x4800_0000`–`0x7800_0000` | class `dma32` | 384 blocks, WB, XN | none |
| `0x7800_0000`–`0x7900_0000` | pool `uncached` | 8 blocks, Normal-NC, XN | none |
| `0x7900_0000`–`0x8000_0000` | no owner | invalid | none |
| `0x8000_0000`–`0x3_4000_0000` | no pool class (see below) | invalid, or 1 GiB WB blocks when the boot's decode check passed | none for pools |

WB is Normal Write-Back Write-Allocate, Inner Shareable. Every writable entry is execute-never at EL0 and EL1. The two invalid holes inside DRAM cover 114 MiB, so a boot without the decode check maps 910 MiB of DRAM at EL1 in five table pages. When the decode check passes, `platform/a733-smp-cached/memory.c` also maps `0x8000_0000` up to `A733_DRAM_MAP_END` (`0x3_4000_0000`) as 1 GiB blocks, which application memory uses ([application-memory-layout.md](application-memory-layout.md)); no pool class owns that range.

The local layout maps only `0x4000_0000`–`0x4800_0000` of DRAM and differs from the table in two places. Its write-back pool, at most 2 MiB (4 MiB with the large NPU graph), lies after BSS and below `0x41f0_0000` inside the WB data blocks. When the image links an `uncached` pool, the level-2 entry for `0x41c0_0000`–`0x41e0_0000` is Normal-NC and keeps the AP and XN bits of the data block it replaces. EL0 can therefore read and write both local pools.

## Boot checks: one translation per extended pool block

The QEMU fixture `checks/device-memory-capacity.{c,py}` compares all 1,024 EL1 level-1 and level-2 entries of the extended-layout table builder with the table above, and checks that every valid pool entry is a block descriptor. One `AT` proves a whole block only for a block descriptor, so extended pool preparation calls `a733_mapping_check_stride()` with a 2 MiB step: one write translation per block, 392 in all. Both extended pool ranges start and end on 2 MiB boundaries. A write translation that succeeds at EL1 implies the read translation of the same descriptor, so no read `AT` is made.

The local layout checks its pools in `a733_memory_local_check()` (`include/a733_memory_critical.h`) with one write `AT` at the pool start and one at each multiple of `a733_local_pool_stride` below the end. Images whose tables come from `platform/a733-smp-cached/memory.c` or `platform/a733/platform.c` map image RAM with 2 MiB level-2 blocks and define the stride as 2 MiB, which gives 2 or 3 translations per image ([memory-map-12g.md](memory-map-12g.md)).
Every other image, such as the QEMU fixtures, takes the weak 4096-byte default and checks every page (1,024 translations for 2 MiB pools, 1,536 for the large NPU graph pool).

`a733_cpu_coherence_tables()` (`platform/a733/cpu_coherence.c`) is the single table builder: it sets the outer-shareable field of the MMIO block entry `l1[0]`, and no monitor rewrites it. The capacity check scans every tracked source and patch for writes to the cached tables outside the builder and fails on any.

`a733_device_memory_prepare()` runs once on core 0 at EL1 before the scheduler starts. It prepares both extended pools, checks the installed table base, TCR, MAIR and the admitted epoch and startup deadline once, and sets a plain READY flag. A pool initialized before a refused profile check stays unreachable, because the extended getters return a pool only when READY is set. Starting the scheduler orders preparation before every task on every core, so READY needs no atomic access. The EL1 tables do not change after preparation, so READY serves as the mapping generation.

The local preparations, `a733_memory_reservations_prepare()` and `a733_memory_uncached_prepare()`, return 1 when the pool is ready or the image links none, and 0 after a refusal, which they keep. Both go through `a733_memory_prepare_once()` in `include/a733_memory_critical.h`, which keeps the result in one word: 0 before the first call, 1 for ready or absent, -1 for refused. A call that finds the word nonzero returns after one acquire load. Otherwise the helper runs the translation check outside any lock (it reads only linker symbols and the installed tables, so concurrent first callers reach
the same verdict). It then enters the kernel critical section and, if the word is still 0, initializes the pool and stores the result with a release store. The critical section holds only the pool initialization (344 instructions in the QEMU SMP fixture), never a translation. `a733_memory_local_prepare()` (`include/a733_memory_local.h`) calls both once on core 0 at EL1, after the tables are installed and before the scheduler starts: from `platform_init()` of `platform/a733/platform.c` and the QEMU platforms, and on the core-0 path of `platform/a733-smp-cached/continue.c`. A refusal stops the boot. Each pool publishes its pointer with a release store, and each getter returns that pointer after one load-acquire (5 instructions in the QEMU SMP fixture); it returns NULL before preparation, after a refusal and when the image links no pool. Extended-layout images do not call it, because `a733_device_memory_prepare()` prepares both pools.

Memory management has no interrupt or polling path: every operation is a synchronous call from boot code or a task. A local getter call costs one load-acquire, an extended getter call one plain load of READY, and a describe one `DMB`; none takes a lock. Paths that reach the pool on every pass include the display output services (the `uncached` getter), the GPU compute binding check (both getters, up to five times per job)
and the xHCI driver (two describes per register read). The camera, display,
crypto and codec drivers do so too (one describe per device access).

## Allocation: class alignment and a ledger inside the kernel critical section

**Alignment.** A pool has class NONE or DMA32. Acquire raises the caller's alignment to the class minimum, so a caller cannot under-align. DMA32 is the extended write-back pool, which holds GPU and NPU buffers. It follows the GPU MMU's 4 KiB, 64 KiB and 2 MiB pages (`docs/gpu-vm.md`): 2 MiB for requests of at least 2 MiB, 64 KiB for requests of at least 64 KiB, otherwise 4 KiB. NONE keeps the caller's alignment; the local-layout pools, the extended `uncached` pool and pools built inside a lease use it.

**Ledger.** Acquire, release and retain read and write slots inside `taskENTER_CRITICAL()` and `taskEXIT_CRITICAL()`. Argument and output checks run before the critical section. Initialization takes no lock: a boot pool is initialized before READY is set or before the scheduler starts, and a pool built inside a lease is initialized by the lease holder before any other task can reach it.

In the SMP image's `ARM_CR82` port the critical section raises the GIC priority mask through an `SVC`, so it works at EL0 and EL1, and takes the kernel's task and ISR spinlocks, which serialize every core. Calls nest through the kernel's per-core count, so a caller already inside a critical section stays masked on return. Before the scheduler starts, the critical section has the same effect as the kernel's own during task creation, in both the `ARM_CR82` and the single-core `ARM_AARCH64_SRE` port. The kernel asserts if the critical section is entered from an interrupt handler; no handler calls the ledger. A native translation unit uses the kernel critical section and fails to compile without `FreeRTOS.h`, unless it defines `DEVICE_MEMORY_POOL_SINGLE_THREAD`, which only one-thread fixtures do. Host checks take no lock.

Acquire is first fit: a scan for a free slot, then at most 33 placement attempts, each comparing against 32 slots. With 31 occupied slots at most 32 attempts run. The locked section touches only the pool structure and makes no call that can block or fault, so release and retain always complete.

**Describe takes no lock.** Drivers call `device_memory_describe()` as a liveness check on every device access. A slot's identity fields (owner, first page, page count) are written only by acquire, before acquire publishes the slot's generation with a release store. Release clears the generation first and orders it before the other stores with a release fence. While a lease is held nothing else changes those fields, and generations are never reused. Describe loads the generation with acquire order and refuses a mismatch, copies the fields and the state, issues a load barrier and reloads the generation. When both loads equal the lease's generation, the copy belongs to the held slot.

The ledger operations accept these changes and refuse every other one, leaving the slot unchanged:

| Operation | Accepted source state | Result |
| --- | --- | --- |
| retain | CPU, DEVICE | RETAINED |
| retain | RETAINED | unchanged, success |
| release | CPU | FREE |

**Holder state changes stay with the holders.** One task at a time holds a lease, changes its state and maintains its cache lines. A hand-off to another task needs a happens-before edge such as task creation or a queue message. The holder changes its slot's state between CPU and DEVICE, or to RETAINED, by writing `device_memory_lookup(pool, lease)->state` under its own service lock; 25 production files do this. These writes take no ledger lock and never write FREE or any other field. Describe loads `state` atomically, but holders store it with plain C assignments, which C11 treats as a data race; the design relies on the compiler emitting each store as one aligned 32-bit `STR`, which Clang does for AArch64. The ledger does not check these writes against a transition rule, so it does not enforce that a retained slot stays retained; that is a holder obligation.

The ledger trusts EL0 code like every kernel object in this map: an EL0 task that writes a pool structure or the kernel's lock words in `.bss` can corrupt ownership or hang every core.

## Cache maintenance and placement

A previous holder can leave dirty lines in a recycled extent, and their eviction during a device write would overwrite device output in DRAM. Every pool records its memory type. `device_memory_pool_init()` creates a write-back pool. The `uncached` preparations mark their pools Normal-NC, and the USB host driver marks its private pool, built inside an `uncached` lease, the same way.

Each write-back pool also records a clean watermark: the end of the highest page it has handed out. A page above the watermark has never been handed out, and no Non-secure agent writes it. EL1 code writes no pool page before acquire returns it, and the CPU never creates dirty lines speculatively. In the extended layout EL0 has no mapping of the pools. In the local layout EL0 can write them, so the watermark there also relies on the cooperative ownership rule: no task writes pool memory it does not hold. Secure dirty lines left by boot0 or the launcher before EL3 entry cannot be removed by Non-secure maintenance; the cold-entry experiment (`docs/cold-entry.md`) is a precondition for qualifying the pools. A boot pool starts with the watermark at its base. A pool built inside a lease also starts at its base, so its builder initializes it directly after acquire, before writing the lease.

Inside the critical section, acquire records the slot, computes the clean range as the part of the extent below the watermark, and raises the watermark to the extent's end. After the critical section, on a write-back pool with a non-empty clean range, acquire calls `aarch64_device_cache(CLEAN)` over that range. The helper's leading `DSB SY` orders the clean after this core's critical-section entry, which observed the previous holder's release. On a Normal-NC pool nothing runs. If the clean is refused, acquire frees the slot, which no other task can yet name, and refuses.

The clean uses EL1-only instructions (`aarch64_device_cache_stride()` reads `CurrentEL`), so acquire on a write-back pool must run at EL1. Every caller does, because each acquires memory for a device it programs, and MMIO has no EL0 mapping. An EL0 task that acquired a recycled write-back extent would take an undefined-instruction exception rather than receive a refusal.

Three consumers (`platform/a733/gpu_compute_reserved.c`, `platform/a733/gpu_graphics_memory.c` and `apps/uart/dma.c`) write zeros over a fresh extent and clean all of it before device use, so on a recycled extent the lines below the watermark are cleaned twice at admission. A zeroed acquire that replaces their own zeroing is open work.

`uncached` owners issue `DMB OSHST` before a doorbell and `DMB OSHLD` after completion. Each transition is a holder state write plus maintenance of the (offset, length) write sets declared at admission. Device-to-device transitions need no maintenance. CPU to device cleans the write set. Device to CPU runs `DMB OSHLD` after the completion read, then `aarch64_device_cache(CLEAN_INVALIDATE)` over the write set; the CPU writes none of a DEVICE extent, so the invalidate cannot discard data unless the holder rule is broken.

Every maintenance form runs with interrupts unmasked. A `DSB` completes only the maintenance its own core issued, so if a task migrates between its `DC` loop and the trailing `DSB SY`, correctness rests on a `DSB` in the context-switch path of `patches/freertos-smp-asm.patch`.
Confirming that barrier is a precondition for every maintenance form here. The leading `DSB SY` in `aarch64_device_cache()` stays for every form, because the generated context switch executes no `DSB`, `DMB` or `ISB` and a migrating task is ordered only by the kernel's spinlock release and acquire.

## Qualified figures

Instruction and barrier counts come from QEMU (`-icount shift=0`, PMU instructions retired, and an execution trace for `SVC`, `DSB`, `ISB` and `DMB`) or from host replay counters. No cycle count is measured on the board. Every row below is in [the record](../evidence/memory-architecture.jsonl) `memory-architecture-stage1-host-qemu`.

| Metric | Figure |
| --- | --- |
| Extended pool check | 392 `AT` in one call, 22,547 instructions (budget 25,000) |
| Local pool check | 1,024 (2 MiB pools) and 1,536 (large NPU graph) `AT` at the per-page default; 2 or 3 `AT` with the 2 MiB stride |
| EL1 DRAM mapped, table memory | 910 MiB, 20 KiB (golden comparison) |
| Critical-section hold | acquire 14,206 instructions (31 occupied slots), release 31, retain 24 |
| Critical-section pair | 485 instructions, 5 `SVC`, 27 `DSB`, 7 `ISB`, 8 or 9 `DMB` |
| Local getter after preparation | 5 instructions, one load-acquire, no barrier |
| Describe; xHCI register read; camera check | 92, 413, 123 instructions with 1, 2, 1 `DMB`, no `SVC`, `DSB` or `ISB` |
| GPU MMU entries per replayed image | 24 to 137 (4,568 maximum with 4 KiB pages) |
| Bytes cleaned on first use of a page | 0, except alignment gaps at admission: 77,824 (GPU images), 1,048,576 (high-profile H.264) |

Describe, acquire, release and retain each cost more than a plain read because of the generation check and the critical-section pair. The worst-case acquire hold of 14,206 instructions (about 29 µs on core 0 at 494 MHz at one instruction per cycle) blocks every core's kernel critical sections and occurs only at admission. First use cleans alignment gaps below the watermark, because the coarse watermark treats every page below the highest handed-out page as possibly dirty and first fit with class minimums places later small extents in
those gaps. Refining the watermark to handed-out pages is the remedy if a measured admission exceeds its budget. `dma32` placement leaves up to 1,056 pages (4.1 MiB) less in the largest free extent, and no request is refused.

## Checks

- Host replay (`checks/device-memory-capacity.c`) of the admission order of every extended-layout image and of `apps/gpu-finite`: comparisons per acquire, placement against 4 KiB first fit, alignment of every extent, device-MMU entries, the largest free extent before each later allocation, bytes cleaned per acquire, and free extents unchanged after every session opens and closes.
- Host ledger check (`checks/memory-reservations.c`): retain and release from every state, refused changes leave the slot unchanged, stale leases are refused, describe enters no lock, the clean watermark, and the local preparations' translation counts and kept refusal.
- QEMU SMP fixture (`checks/memory-architecture-smp.{c,py}`): two Cortex-A55 cores with GICv3 and the patched `ARM_CR82` port, every task at EL1. It measures the rows above, the worst-case acquire hold, nesting (an acquire and a release inside an enclosing critical section return with `ICC_PMR_EL1` still raised), and
  the getter. Under multithreaded TCG it runs 200,000 rounds of release and retain on core 1 against long acquires on core 0, then 100,000 rounds of slot reuse against describes on the other core (every describe refuses or returns the published extent and state). It fails when a prepared getter call exceeds 32 instructions or contains a barrier, or when a first call holds the critical section for more than 15,000 instructions.
- QEMU cached-devices fixture (`checks/cached-device-reservations.{S,py}`): the pre-scheduler path in which core 0 acquires and retains under the kernel critical section.

## Limitations and open work

Left out, with the condition that reopens each:

- Interrupt-handler callers of the ledger: no consumer needs one.
- A pool-private lock, if the kernel critical section's hold hurts scheduling.
- A sorted index of occupied slots (acquire hold of about 600 instructions), if the acquire hold delays a running deadline; the 5 ms audio DMA period is the tightest.
- Removing the leading `DSB SY` from `aarch64_device_cache()`, which needs board qualification under `docs/shared-helpers.md`.
- Splitting large zeroing or cleaning across cores 6 and 7.
- Execute-never read-only data, guard pages and an EL1-only region for kernel and pool structures (all need level-3 tables or a privileged data block); `SCTLR_EL1.WXN` (the golden comparison already excludes writable executable entries); read-only mapping of admitted NPU models (needs a mapping generation).

Open work:

1. **Stage 2.** A 1 GiB `gpu` class and a 1 GiB `npu` class at `0x8000_0000`–`0x1_0000_0000` (the `npu` minimum alignment is 16 MiB for requests of at least 16 MiB, otherwise 1 MiB, because the NPU MMU has no 2 MiB page). Prerequisites: an EL3 check that DRAM decodes linearly and that protected regions allow Non-secure access; profile word 0 of the version-2 boot record becomes 2; launch evidence that no payload, remote firmware or bus master left running by boot0 uses the range; one `AT` per class; `DEVICE_MEMORY_MAX_PAGES` raised to 262,144 and the size product cast to `size_t`. EL1 level-1 entries 2 and 3 become `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP[2:1] = 0b00.
2. **Stage 3.** Pool ownership of `0x1_0000_0000`–`0x3_4000_0000` once a consumer claims it, with the same launch evidence.
3. **Locked `set_state`.** Move every holder state write onto a locked `set_state` with a transition table, remove `device_memory_lookup()`, and move the three zero-and-clean consumers onto a zeroed acquire (`DC ZVA` over the extent, then a clean of the whole extent on a write-back pool).
4. **Cold-entry experiment.** Specify the launcher's dirty-line experiment in `docs/cold-entry.md`; its passing result is a precondition for qualifying the pools on the board.
5. **`apps/gpu-finite`** cleans the fixed range `0x4800_0000`–`0x4808_6000` at exit; a rebuild against `dma32` places its 88-page payload at `0x4803_0000`–`0x4808_8000`, so the range must come from the actual extents first.
6. **Ledger NC board qualification.** The OS enables cacheable walks and
   publishes mutable tables through their protected EL1 alias. The free-chunk
   converter and manifest-sized USB backing are host-tested; the empty-port
   board run remains pending ([design](ledger-nc.md)).
7. **Board runs** that fix the provisional rows, each recording the CPU-L, CPU-B and DSU clocks: extended pool check cycles on core 0; worst-case acquire and an empty critical-section pair on cores 0 and 6; describe and one xHCI register read; plain clean per absent line over 1 GiB and 64 KiB; stage 2 first and second GPU session open on core 6 (target 0 lines cleaned on first open, one 1 GiB clean of at most 80 M A76 cycles on reopen); the recognizer at EL0 with each forbidden access raising its expected fault class; the GPU completing checksummed jobs through 2 MiB device-MMU entries; the stage 2 DRAM check for 12,288 MiB with patterns above `0x8000_0000` not aliasing into low DRAM. The predictions are the `memory-architecture-board-*` records in [the evidence file](../evidence/memory-architecture.jsonl).
