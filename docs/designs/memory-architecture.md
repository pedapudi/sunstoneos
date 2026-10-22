# Design: device memory classes, boot mapping checks and protection

Status: stage 1 is **implemented** and checked in QEMU and on the host;
stages 2 and 3 are **design**. No part is qualified on the board, and every
cycle budget below is provisional until the board runs listed at the end
exist. Readers are assumed to know AArch64 stage-1 translation, the FreeRTOS
SMP port and non-coherent DMA.

Ten images have a 768 MiB write-back device pool, `dma32`, and a 16 MiB
Normal non-cacheable pool, `uncached`. Before stage 1, boot checked both pools
page by page with 401,408 address translations (`AT`), and nothing serialized
the pool ledger across cores. Stage 1 checks the extended pools with one `AT`
per 2 MiB block and serializes acquire, release and retain with the kernel's
critical section, while the per-access lease check stays lock-free. It also
aligns large `dma32` extents to the GPU MMU's largest page and moves the
after-acquire clean into the pool, limited to pages that can hold dirty
lines. Stage 2 adds
a 1 GiB class each for the GPU and the NPU. Stage 3 maps the DRAM above
4 GiB when a consumer claims it.

## Map and protection

Tasks start at EL0 unless the image selects EL1, and EL0 tasks call kernel
functions directly, so kernel data, stacks and heap stay EL0 read-write. MMIO
has no EL0 access in either layout, so every lease holder that programs a
device is an EL1 task. The extended layout also gives its pools no EL0
access. The local layout places its pools inside EL0 read-write data blocks,
as described after the table.

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
| `0x8000_0000`–`0x1_0000_0000` | classes `gpu`, `npu` (stage 2) | two 1 GiB blocks, WB, XN | none |
| `0x1_0000_0000`–`0x3_4000_0000` | no owner until stage 3 | invalid | none |

WB is Normal Write-Back Write-Allocate, Inner Shareable. Every writable entry
is execute-never at EL0 and EL1. The two invalid holes inside DRAM cover
114 MiB, so EL1 maps 910 MiB of DRAM in stage 1, 2,958 MiB in stage 2 and
12,174 MiB in stage 3, in five table pages. Stage 3 adds level-1 entries 4 to
12 as 1 GiB blocks and no table page.

The local layout maps only `0x4000_0000`–`0x4800_0000` of DRAM and differs from
the table above in two places. Its write-back pool, at most 2 MiB (4 MiB with
the large NPU graph), lies after BSS and below `0x41f0_0000` inside the WB
data blocks. When the image links an `uncached` pool, the level-2 entry for
`0x41c0_0000`–`0x41e0_0000` is Normal-NC and keeps the AP and XN bits of the
data block it replaces. EL0 can therefore read and write both local pools.

## Boot checks one translation per extended pool block

The QEMU fixture `checks/device-memory-capacity.{c,py}` compares all 1,024
EL1 level-1 and level-2 entries of the extended-layout table builder with the
table above, and checks that every valid pool entry is a block descriptor.
One `AT` proves a whole block only for a block descriptor, so extended pool
preparation calls `a733_mapping_check_stride()` with a 2 MiB step: one write
translation per block, 392 in all. Both extended pool ranges start and end on
2 MiB boundaries, so the stride covers every block once. A write translation
that succeeds at EL1 implies the read translation of the same descriptor, so
no read `AT` is made. By the same argument the read `AT` of the local uncached
check is redundant; removing it would cut the local count for 2 MiB pools from
1,536 to 1,024 translations, and that change is not yet made.

The local layout keeps its per-page check: one write `AT` per write-back page
and a read and a write `AT` per uncached page, 1,536 translations for 2 MiB
pools and 2,048 for the large NPU graph pool. No golden comparison covers the
local tables, so the per-page check is the only proof that each local pool
page has the expected attributes, and it stays.

Two monitor variants (`apps/storage-read-single-core/monitor.c` and
`apps/dsi-scanout/monitor.patch`) write level-1 entry 0 of the EL1 and EL3
tables after the builder runs. Each write sets only the outer-shareable field
of the MMIO block (`l1[0] |= 2 << 8`) and touches no pool entry. The golden
comparison of the builder's pool entries, and the per-block pool check that
relies on it, therefore still hold in those images. The capacity check scans
every tracked source and patch for writes to the cached tables outside the
builder and fails if any write has another form. The single-builder change
below removes those writers; it is no longer a merge precondition.

`a733_device_memory_prepare()` runs once on core 0 at EL1 before the scheduler
starts. It prepares both extended pools, then checks the installed table base,
TCR, MAIR and the admitted epoch and startup deadline once, and sets a plain
READY flag. A pool initialized before a refused profile check stays
unreachable, because the extended getters return a pool only when READY is
set. Starting the scheduler orders preparation before every task on every
core, so READY needs no atomic access. The EL1 tables do not change after
preparation, so READY serves as the mapping generation.

The local preparations, `a733_memory_reservations_prepare()` and
`a733_memory_uncached_prepare()`, return 1 when the pool is ready or the image
links none, and 0 after a refusal, which they keep. Both go through one
shared inline helper, `a733_memory_prepare_once()` in
`include/a733_memory_critical.h`, which keeps the result in one word: 0
before the first call, 1 for ready or absent, -1 for refused. A call that
finds the word nonzero returns after one acquire load. Otherwise the helper
runs the translation check outside any lock, because the check reads only
linker symbols and the installed tables, so concurrent first callers reach
the same verdict. It then enters the kernel critical section, and if the
word is still 0, initializes the pool and stores the result with a release
store. The critical section therefore holds only the pool initialization,
344 instructions in the QEMU SMP fixture, and never a translation.

Until `platform_init()` calls the local preparations (a primary-agent change
below), each local getter calls its preparation, so the first call prepares
the pool. A later getter call costs 17 instructions with one load-acquire
and no `SVC`, `DSB`, `ISB` or `DMB`. Once `platform_init()` prepares the
pools, the getters can test the kept result directly.

Memory management has no interrupt or polling path: every operation is a
synchronous call from boot code or a task. These paths reach the pool on
every pass rather than once at admission:

- `a733_display_framebuffer_memory()` calls the `uncached` getter on every
  pass of the HDMI, GPU display and DisplayPort output services and on every
  display-engine replay submission.
- `firmware_owned()` in `platform/a733/gpu_compute_reserved.c` calls both
  getters from each compute binding check; the GPU compute service checks a
  job's binding up to five times.
- The xHCI driver describes its lease twice per register read.
- The camera, display, crypto and codec drivers describe their lease once per
  device access.

A local getter call costs one load-acquire, an extended getter call one plain
load of READY, and a describe one `DMB`. None of them takes a lock.

## Allocation: class alignment and a ledger inside the kernel critical section

**Alignment.** A pool has class NONE or DMA32. Acquire raises the caller's
alignment to the class minimum, so a caller cannot under-align. DMA32 is the
extended write-back pool, which holds GPU and NPU buffers until stage 2. It
follows the GPU MMU's 4 KiB, 64 KiB and 2 MiB pages (`docs/gpu-vm.md`): 2 MiB
for requests of at least 2 MiB, 64 KiB for requests of at least 64 KiB,
otherwise 4 KiB. NONE keeps the caller's alignment. The local-layout pools,
the extended `uncached` pool and pools built inside a lease use NONE, so
their placement does not change. Stage 2 adds the `gpu` class with the DMA32
rule and the `npu` class together with their pools and mappings. The `npu`
minimum is 16 MiB for requests of at least 16 MiB and otherwise 1 MiB,
because the NPU MMU has no 2 MiB page (`docs/npu-mmu.md`).

**Ledger.** Acquire, release and retain read and write slots inside
`taskENTER_CRITICAL()` and `taskEXIT_CRITICAL()`. Argument and output checks
run before the critical section. Initialization takes no lock: a boot pool is
initialized before READY is set or before the scheduler starts, and a pool
built inside a lease is initialized by the lease holder before any other task
can reach it.

In the SMP image's `ARM_CR82` port the critical section raises the GIC
priority mask through an `SVC`, so it works at EL0 and EL1, and takes the
kernel's task and ISR spinlocks, which serialize every core. Calls nest through
the kernel's per-core count, so a caller already inside a critical section
stays masked on return. Before the scheduler starts, the critical section has
the same effect as the kernel's own critical sections during task creation at
boot, in both the `ARM_CR82` port and the single-core `ARM_AARCH64_SRE` port.
The kernel asserts if the critical section is entered from an interrupt
handler; no handler calls the ledger.

The lock is selected explicitly. A native translation unit (AArch64, not
Linux) uses the kernel critical section and fails to compile without
`FreeRTOS.h`, unless it defines `DEVICE_MEMORY_POOL_SINGLE_THREAD`, which only
one-thread fixtures do. Host checks take no lock.

Acquire keeps the current first fit: a scan for a free slot, then at most 33
placement attempts, each comparing against 32 slots. With 31 occupied slots at
most 32 attempts run, because each failed attempt passes at least one
occupied extent and none is passed twice. The measured worst case holds the
critical section for 14,206 instructions; release holds it for 31 and retain
for 24. The locked section touches only the pool structure and makes no call
that can block or fault, so release and retain, the bounded-failure
fallbacks, always complete. The QEMU check below runs them on another core
during worst-case acquires.

**Describe takes no lock.** Drivers call `device_memory_describe()` as a
liveness check on every device access: the xHCI driver twice per register
read, and the camera, display, crypto and codec drivers once per call. A
slot's identity fields (owner, first page, page count) are written only by
acquire, before acquire publishes the slot's generation with a release store.
They are cleared only by release, which clears the generation first and orders
it before the other stores with a release fence. While a lease is held,
nothing else changes those fields, and generations are never reused. Describe
loads the generation with acquire order and refuses a mismatch, copies the
fields and the state, issues a load barrier and reloads the generation. When
both loads equal the lease's generation, the copy belongs to the held slot.
The critical section is therefore redundant for describe, which costs 92
instructions with one `DMB` and no `SVC`, `DSB` or `ISB`.

The ledger operations accept these changes and refuse every other one,
leaving the slot unchanged:

| Operation | Accepted source state | Result |
| --- | --- | --- |
| retain | CPU, DEVICE | RETAINED |
| retain | RETAINED | unchanged, success |
| release | CPU | FREE |

**Holder state changes stay with the holders.** One task at a time holds a
lease, changes its state and maintains its cache lines. A hand-off to another
task needs a happens-before edge such as task creation or a queue message.
The holder changes its slot's state between CPU and DEVICE, or to RETAINED,
by writing `device_memory_lookup(pool, lease)->state` under its own service
lock, as before stage 1; 25 production files do this. These writes take
no ledger lock. They never write FREE or any other field, so acquire's
free-slot scan and placement, which run under the lock, see the same occupied
set. Describe loads `state` atomically, but the holders store it with plain
C assignments, which C11 treats as a data race. The design relies on the
compiler emitting each such store as one aligned 32-bit `STR`, which Clang
does for AArch64; the locked `set_state` below removes that assumption. The ledger does not
check these writes against a transition rule, so it does not enforce that a
retained slot stays retained; that remains a holder obligation, as before.
A locked `set_state` with a transition table returns together with the merge
that moves those holders onto it and removes `device_memory_lookup()`.

The ledger trusts EL0 code in the same way as every kernel object in this map:
an EL0 task that writes a pool structure or the kernel's lock words in `.bss`
can corrupt ownership or hang every core. Protecting the ledger needs an
EL1-only data region (see Left out).

## Cache maintenance and placement

A previous holder can leave dirty lines in a recycled extent, and their
eviction during a device write would overwrite device output in DRAM. Every
pool records its memory type. `device_memory_pool_init()` creates a
write-back pool. The `uncached` preparations mark their pools Normal-NC, and
the USB host driver marks its private pool, which it builds inside an
`uncached` lease, the same way.

Each write-back pool also records a clean watermark: the end of the highest
page it has handed out. A page above the watermark has never been handed out,
and no Non-secure agent writes it. EL1 code writes no pool page before acquire
returns it, and the CPU never creates dirty lines speculatively. In the
extended layout EL0 has no mapping of the pools. In the local layout EL0 can
write them, so the watermark there also relies on the cooperative ownership
rule below: no task writes pool memory it does not hold. Secure dirty lines left by boot0 or the launcher before EL3
entry cannot be removed by Non-secure maintenance at all; the cold-entry
experiment below covers them. A boot pool starts with the watermark at its
base. A pool built inside a lease also starts at its base, so its builder
initializes it directly after acquire, before writing the lease.

Inside the critical section, acquire records the slot, computes the clean
range as the part of the extent below the watermark, and raises the watermark
to the extent's end. After the critical section, on a write-back pool with a
non-empty clean range, acquire calls `aarch64_device_cache(CLEAN)` over that
range. The helper's leading `DSB SY` orders the clean after this core's
critical-section entry, which observed the previous holder's release, so the
clean finds any line that holder left dirty. On a Normal-NC pool nothing runs.
If the clean is refused, acquire frees the slot, which no other task can yet
name, and refuses; the raised watermark only causes more cleaning later.

The clean uses EL1-only instructions (`aarch64_device_cache_stride()` reads
`CurrentEL`), so acquire on a write-back pool must run at EL1. Every current
caller does, because each acquires memory for a device it programs, and MMIO
has no EL0 mapping in either layout. The pool header states the rule. An EL0
task that acquired a recycled write-back extent would take an
undefined-instruction exception at `MRS CurrentEL` rather than receive a
refusal. Only the local layout maps its pools at EL0, and no local-layout
task acquires without programming a device.

Three consumers, `platform/a733/gpu_compute_reserved.c`,
`platform/a733/gpu_graphics_memory.c` and `apps/uart/dma.c`, still write zeros
over a fresh extent and clean all of it before device use. On a recycled
extent, the lines below the watermark are therefore cleaned twice at
admission: once by acquire and once by the consumer. This is an explained
regression in admission time only. A zeroed acquire (`DC ZVA` over the extent,
then a clean of the whole extent on a write-back pool) returns with the merge
that moves those three consumers onto it and deletes their zeroing and clean.
Adding it earlier would leave a function without a caller.

`uncached` owners issue `DMB OSHST` before a doorbell and `DMB OSHLD` after
completion. Transitions run at session open, per 60 Hz GPU frame, per NPU
inference and per DMA period of up to 200 Hz. Each transition is a holder
state write plus maintenance of the (offset, length) write sets declared at
admission. Device-to-device transitions need no maintenance. CPU to device
cleans the write set with the helper. Device to CPU runs `DMB OSHLD` after the
completion read, then `aarch64_device_cache(CLEAN_INVALIDATE)` over the write
set. The CPU writes none of a DEVICE extent, so `DC CIVAC` costs what
`DC IVAC` costs and cannot discard data if the holder rule is broken. The
helper's leading `DSB SY` is stronger than this path needs: under the Arm ARM
rules for ordering data-cache maintenance by address with Device accesses, a
`DMB` between the completion read and the `DC` would suffice.

Every maintenance form runs with interrupts unmasked. A `DSB` completes only
the maintenance its own core issued, so if a task migrates between its `DC`
loop and the trailing `DSB SY`, correctness rests on a `DSB` in the
context-switch path of `patches/freertos-smp-asm.patch`. Confirming that
barrier is a precondition for every maintenance form in this design.

## Reused, transitional and removed

- Reused: the first-fit scan and placement rule,
  `device_memory_acquire_aligned()`, the kernel critical section,
  `aarch64_device_cache()` unchanged, `a733_cpu_coherence_tables()`, the
  version-2 boot record, `a733_mapping_check_stride()` for both layouts, and
  the QEMU fixture with its golden array.
- Removed: the batched page-scan state machine in `device_memory_layout.c`
  with `a733_device_memory_commit_allowed()`, `same_layout()`,
  `a733_memory_reservations_commit_verified()` and
  `a733_memory_uncached_commit_verified()`. The GPU, display and DisplayPort
  checks that called or hooked them model READY and call the two pool
  preparations instead.
- Removed from the emulator checks of extended-layout codec images: their own
  pool construction with `device_memory_pool_init()`, which no such image
  links any longer and which built `boot_pool` as class `none` and
  `uncached_pool` as write-back. `checks/h264-encode-native.py`,
  `checks/deinterlace-native.py`, `checks/h264-high-allocation.py`,
  `checks/h264-fullhd-native.py` and `checks/h264-high-native.py` now model
  READY, the pool translations and the kernel critical section, and call the
  two pool preparations. Under `dma32` their write-back extents move to 64 KiB
  or 2 MiB boundaries. The encoder's write-back use grows from 3,022,848 to
  3,080,192 bytes and the High decoder's from 21,823,488 to 26,181,632 bytes;
  the expected values in those checks and in `docs/h264-encode-native.md`,
  `docs/h264-native.md` and `docs/deinterlace-native.md` changed with them.
  `apps/gpu-finite/return-check.py` stays frozen with the composition it
  checks, as described under "Changes the primary agent must make".
- Transitional, removed in this order:
  1. The lazy preparation in both local getters, once `platform_init()` calls
     both local preparations; the getters then test the kept result.
  2. `a733_device_memory_tables_admitted()` and
     `a733_device_memory_tables_complete()`, once the table builder reads the
     layout constants directly.
  3. `device_memory_lookup()` and direct `state` writes, in the merge that
     adds a locked `set_state` and moves every holder onto it. The zeroed
     acquire lands in the same or a later merge with its three consumers.

## Performance budgets

Instruction and barrier counts come from QEMU (`-icount shift=0`, PMU
instructions retired around the call, and an execution trace for `SVC`,
`DSB`, `ISB` and `DMB`) or from host replay counters. Cycles come from PMU
`CPU_CYCLES` on the board and name the core type: core 0 is a Cortex-A55 on
CPU-L, core 6 a Cortex-A76 on CPU-B. Rows with no board measurement are
provisional and unqualified.

| Metric | Before stage 1 | Target | Measured | Method |
| --- | --- | --- | --- | --- |
| Extended pool check | 401,408 `AT` in 3,136 calls; 27,474,026 instructions | 392 `AT` in one call; at most 25,000 instructions; 78,400 A55 cycles | 392 `AT`; 22,547 instructions; cycles provisional | QEMU; board core 0 |
| Local pool check | 1,536 `AT` (2 MiB pools), 2,048 (large NPU graph) | unchanged | 1,536 and 2,048 | callback count |
| DRAM mapped at EL1; table memory | 910 MiB; 20 KiB | 910, 2,958, 12,174 MiB by stage; 20 KiB | 910 MiB; 20 KiB | QEMU golden comparison |
| Slot comparisons per acquire | 1,056 bound | unchanged bound | 160 worst replayed | host replay |
| Critical-section hold | no serialization | acquire at most 15,000 instructions; release and retain at most 100 | acquire 14,206 (31 occupied slots, 32 attempts); release 31; retain 24 | QEMU SMP fixture |
| Critical-section pair per acquire, release or retain | none | one pair in today's port | 485 instructions, 5 `SVC`, 27 `DSB`, 7 `ISB`, 8 or 9 `DMB` | QEMU SMP fixture |
| Local getter call after preparation | one plain load | no `SVC`, `DSB`, `ISB` or `DMB` | 17 instructions, one load-acquire, no barrier | QEMU SMP fixture |
| Critical-section hold of a local pool's first preparation | none | at most 15,000 instructions | 344 instructions (pool initialization only) | QEMU SMP fixture |
| Describe | 71 instructions, no barrier | no `SVC`, `DSB` or `ISB` | 92 instructions, 1 `DMB` | QEMU SMP fixture |
| xHCI register read (`rd()`, two describes) | 371 instructions | no `SVC`, `DSB` or `ISB` | 413 instructions, 2 `DMB` | QEMU SMP fixture |
| Camera memory check (`memory_owned()`, one describe) | 102 instructions | no `SVC`, `DSB` or `ISB` | 123 instructions, 1 `DMB` | QEMU SMP fixture |
| GPU MMU entries for the replayed extents | 354 to 4,568 per image (4 KiB pages) | 2 MiB and 64 KiB pages where aligned | 24 to 137 per image | host replay |
| Largest free extent before each allocation | 4 KiB alignment | no request newly refused | none refused; up to 1,056 pages (4.1 MiB) smaller | host replay |
| Bytes cleaned on first use of a page | callers clean whole extents | 0 | 0, except alignment gaps below the watermark: 77,824 (GPU images) and 1,048,576 (high-profile H.264), at admission only | host replay counter |
| Plain clean cost | done by callers | 4 cycles per absent line on A55 and A76 | provisional, unmeasured | board, cores 0 and 6 |
| GPU session open, NPU model load (stage 2) | none | first open: 0 lines cleaned; reopen: one 1 GiB clean, at most 80 M A76 cycles | provisional, unmeasured | board core 6 |
| `DC` per device-to-CPU transition | full READ and WRITE buffers (NPU) | declared write set | not changed in stage 1 | GPU and NPU host models |

The reviewed form of stage 1 also locked describe. In the same QEMU fixture
that form cost 633 instructions, 5 `SVC`, 27 `DSB` and 7 `ISB` per describe,
and 1,495 instructions with 10 `SVC`, 54 `DSB` and 14 `ISB` per xHCI register
read. The lock-free describe removes that cost from every device access.

These rows are explained regressions:

- Describe costs 21 more instructions and one `DMB` than the plain read before
  stage 1. The generation check that costs them is what makes describe
  correct without the callers' service locks.
- Acquire, release and retain each add one critical-section pair. Serializing
  the ledger across cores is a correctness requirement, and the kernel
  critical section is the existing mechanism for it. The worst-case acquire
  hold of 14,206 instructions, about 29 µs on core 0 at 494 MHz if one
  instruction retires per cycle, blocks every core's kernel critical sections
  and occurs only at admission. The pair cost falls when the primary agent
  meets the critical-section budget in `docs/performance-architecture.md`.
- First use cleans alignment gaps below the watermark. The coarse watermark
  treats every page below the highest handed-out page as possibly dirty,
  including gap pages that were never handed out, and first fit with class
  minimums places later small extents in those gaps. The largest free extent
  shrinks for the same reason: a 2 MiB-aligned extent leaves a gap that only
  smaller extents can use. These cleans happen at admission only. If a
  measured admission exceeds its budget, the watermark is refined to cover
  only handed-out pages, so gap pages below a 2 MiB-aligned extent stay
  uncleaned; the high-profile H.264 admission, which cleans 16,384 lines in
  gaps, is the first to measure.
- On a recycled extent, the three zero-and-clean consumers clean the lines
  below the watermark a second time, as described above.
- A local getter call costs 17 instructions with one load-acquire, where the
  getter before stage 1 was one plain load. The getter calls its preparation
  so that the first call prepares the pool until `platform_init()` does; the
  cost falls to a plain test of the kept result after that change.

## Fixtures

- Host replay (`checks/device-memory-capacity.c`) of the admission order of
  every extended-layout image and of `apps/gpu-finite`. It counts comparisons
  per acquire, placement against 4 KiB first fit, alignment of every extent,
  device-MMU entries, the largest free extent before each later allocation,
  no newly refused request, bytes cleaned per acquire, and free extents
  unchanged after every session opens and closes once.
- Host ledger check (`checks/memory-reservations.c`): retain and release from
  every state, refused changes leave the slot unchanged, stale leases are
  refused, describe enters no lock, and the clean watermark; plus the local
  preparations' translation counts and kept refusal.
- QEMU SMP fixture (`checks/memory-architecture-smp.{c,py}`), two Cortex-A55
  cores with GICv3 and the patched `ARM_CR82` port, every task at EL1:
  - measures the QEMU rows above under `-icount shift=0`, against the pool
    source before stage 1 and the reviewed locked form;
  - measures the worst-case acquire hold with 31 occupied slots;
  - checks that an acquire and a release inside an enclosing critical section
    return with `ICC_PMR_EL1` still raised;
  - measures a getter built on `a733_memory_prepare_once()`: the hold of its
    first call and the cost and barriers of a later call;
  - under multithreaded TCG, runs 200,000 rounds of release and retain on
    core 1 against long acquires on core 0. Both must be attempted while
    core 0 holds an acquire and must complete, and no two held slots may
    overlap;
  - then runs 100,000 rounds in which core 0 releases a lease and acquires
    the same slot for another owner and a different extent, while core 1
    describes the released lease. Every describe must refuse or return the
    published extent and state, and a refused lease must stay refused. The
    counts vary between runs under multithreaded TCG; each recorded run
    accepted more than one million describes and refused more than ten
    million, and its `result.json` holds the counts;
  - fails when a prepared getter call exceeds 32 instructions, contains any
    `SVC`, `DSB`, `ISB` or `DMB`, or when its first call holds the critical
    section for more than 15,000 instructions. The result records the commit
    it was built from and whether the tree differed from that commit.
- QEMU cached-devices fixture (`checks/cached-device-reservations.{S,py}`):
  the pre-scheduler path in which core 0 acquires and retains under the
  kernel critical section. Its EL1 vectors service the critical section's
  `SVC #107`, `#108` and `#109`.

Results, logs and the measured images are in
`~/.cache/sunstoneos/evidence/memory-architecture-20261022/revision-4/`. Its
`README.json` names the commit the results were produced from; the rebuilt
codec images and the results of the five rebound codec checks are under
`codec/`.

## Left out

- Interrupt-handler callers of the ledger: no consumer needs one, and the
  kernel critical section asserts outside handlers.
- A pool-private lock. If a measurement shows the kernel critical section's
  hold hurting scheduling, the pool gets a lock again, built as a shared
  helper together with the counter-clock lock.
- A sorted index of occupied slots, which would cut acquire's hold to about
  600 instructions. It returns only if the measured acquire hold delays a
  running deadline, the 5 ms audio DMA period being the tightest today.
- Removing the leading `DSB SY` from `aarch64_device_cache()`. Under
  `docs/shared-helpers.md` a barrier change to a shared helper needs board
  qualification, and the saving is one barrier per transition. It returns if a
  per-frame measurement shows the barrier matters.
- A boot clean of the `uncached` pool. The image enters EL1 with the MMU and
  data cache off, and both the EL3 and the EL1 tables map the pool Normal-NC
  from their first installation, so no Non-secure cacheable alias exists.
- Splitting large zeroing or cleaning across cores 6 and 7: one core is
  expected to meet the session-open target; a missed measurement reopens it.
- Cacheable translation-table walks, the contiguous hint on `dma32` and the
  walk A/B run belong to the separate package described below. These pools
  do not depend on it.
- Execute-never read-only data, guard pages and an EL1-only region for kernel
  and pool structures: all need level-3 tables or a privileged data block.
- `SCTLR_EL1.WXN`: the golden comparison already excludes writable executable
  entries.
- Read-only mapping of admitted NPU models: changes the tables after
  preparation and needs a mapping generation.

## Stage prerequisites

- Both stages: the DRAM-controller package provides an EL3 check that DRAM
  decodes linearly to `0x3_4000_0000` and that its protected regions allow
  Non-secure access to the newly mapped range.
- Stage 2: profile word 0 of the version-2 boot record becomes 2, and the GPU
  and NPU bases come only from header constants. Launch evidence shows that
  no boot-loader payload, remote firmware or bus master left running by boot0
  uses `0x8000_0000`–`0x1_0000_0000`. Stage 2 adds one `AT` per class, raises
  `DEVICE_MEMORY_MAX_PAGES` to 262,144 and casts the size product in
  `device_memory_pool_init()` to `size_t`.
- Stage 3: the same launch evidence for `0x1_0000_0000`–`0x3_4000_0000`.

## Changes the primary agent must make

- **Context-switch barrier.** Confirm that the context switch in
  `patches/freertos-smp-asm.patch` executes `DSB SY` before a task resumes on
  another core, or add it. This is a precondition for every maintenance form.
- **Local preparation.** Call `a733_memory_reservations_prepare()` and
  `a733_memory_uncached_prepare()` from `platform_init()` after the identity
  map is installed and before `vTaskStartScheduler()`; both return 1 for a
  ready or absent pool. The lazy getter path is then removed.
- **One final table builder.** Set the outer-shareable field of `l1[0]` in
  `a733_cpu_coherence_tables()` and delete the later writes in
  `apps/storage-read-single-core/monitor.c` and
  `apps/dsi-scanout/monitor.patch`. The capacity check guards those writes
  until then.
- **Entry cache state.** A Secure dirty line over any pool at EL3 entry cannot
  be removed by Non-secure EL1 maintenance, and the watermark assumes none
  exists. Specify the launcher's dirty-line experiment in
  `docs/cold-entry.md`; its passing result is a precondition for qualifying
  the pools on the board.
- `platform/a733-smp-cached/memory.c` reads the layout header constants
  directly.
- **Single preparation call.** `a733_device_memory_prepare()` returns 1 or
  -1 and never 0. Replace the `do { ... } while (prepared == 0)` loops in
  `platform/a733-smp-cached/continue.c`,
  `apps/storage-read-single-core/runtime.c` and
  `apps/dsi-scanout/runtime.patch` with one call that requires 1.
- Stage 2: EL1 level-1 entries 2 and 3 become
  `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP[2:1] = 0b00; EL3
  entries stay invalid. Stage 3 adds entries 4 to 12 the same way.

Consumer owners, in a later merge, move every holder state write onto a
locked `set_state`, remove `device_memory_lookup()`, and move the three
zero-and-clean consumers onto a zeroed acquire. `apps/gpu-finite` cleans the
fixed range `0x4800_0000`–`0x4808_6000` at exit. A rebuild against the
`dma32` class places its 88-page payload at `0x4803_0000`–`0x4808_8000`, so
that range must come from the actual extents before such a rebuild. The
composition links frozen pool objects today and is not yet affected.

**Separate package: cacheable translation-table walks.** The cached-table
path walks its tables uncached. Setting IRGN0 = ORGN0 = 0b01 in
`A733_CPU_COHERENCE_TCR` changes `platform/a733-smp-cached/continue.S`,
`platform/a733/cpu_coherence.S`, `boot/a733/native-el1-cache-probe.S` and the
`SUNSTONE_CACHED_SINGLE_CORE` branch of
`apps/storage-read-single-core/platform.patch`. About 15 host checks and tools
repeat the TCR value as a literal; that package first makes them read
`A733_CPU_COHERENCE_TCR` from its header, or from one Python constant shared
by the tools. It cleans the table pages before the first descriptor write and
invalidates the EL1 tables through the Non-secure alias after the last EL1
table write, and adds the same maintenance to the tables `platform_init()`
writes. It specifies the Non-secure dirty-line experiment over the tables and
runs the walk A/B measurement (`CPU_CYCLES`, `DTLB_WALK`, `ITLB_WALK` for a
GPU frame, an NPU inference and a recognizer inference on cores 0 and 6). It
depends on the single-builder change above.

## Board runs that would fix the provisional rows

Each run records the CPU-L, CPU-B and DSU clock rates beside its result.

- Extended pool check: `CPU_CYCLES` around `a733_device_memory_prepare()` on
  core 0.
- Critical-section hold and pair: `CPU_CYCLES` for a worst-case acquire and
  for an empty pair on cores 0 and 6.
- Describe and one xHCI register read: `CPU_CYCLES` on cores 0 and 6.
- Plain clean: `CPU_CYCLES` per absent line over 1 GiB and over 64 KiB on
  cores 0 and 6.
- Stage 2 session open: first and second GPU session open on core 6.
- The recognizer at EL0, with each forbidden access raising its expected
  fault class.
- The GPU completing checksummed jobs through 2 MiB device-MMU entries.
- Stage 2: the DRAM check passing for 12,288 MiB, patterns written above
  `0x8000_0000` not aliasing into low DRAM, and the GPU and NPU completing
  checksummed jobs inside their classes.
