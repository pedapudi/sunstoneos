# Performance optimizations

This report lists performance changes for SunstoneOS on the Orange Pi Zero 3W
(Allwinner A733). It is ordered by impact on the project goal in
[docs/goal.md](../goal.md): complete warm mimus speech recognition in at most
0.05 times the audio duration. The first part covers recognition latency. The
second covers data movement: the USB evidence channel, DMA, storage, radio and
device streaming. The third covers boot time and build time. A sequenced roadmap
closes the report.

Each finding has an identifier such as `cpu-startup-3`, made of the audit area
and a number. Related findings that propose the same change are grouped under
one heading, with all their identifiers listed.

No change in this report has been measured on the board. Every speed-up figure
is an estimate derived from code, instruction counts or published operating
points, unless the text names a recorded measurement. Each finding states how
to measure it.

## Terms used in this report

- **Tick.** One FreeRTOS scheduler period. The tree sets `configTICK_RATE_HZ`
  to 1000, so a tick is 1 ms. `vTaskDelay(1)` sleeps until the next tick.
- **Tick pacing.** A driver that performs one bounded step per call, called by
  an owner task that sleeps one tick between calls. Its throughput is one step
  per millisecond regardless of hardware speed.
- **PIO.** Programmed I/O: the CPU moves every data word through a device FIFO
  register, as opposed to DMA.
- **Normal non-cacheable (NC) memory.** A memory type the CPU accesses without
  caching. The tree uses an NC pool for xHCI rings, codec buffers and display
  buffers. Each CPU load from it is a separate bus transaction.
- **SVC and SMC.** Exception-generating instructions that enter the kernel
  (EL1) and the secure monitor (EL3) respectively. FreeRTOS tasks in the SMP
  images run at EL0, so several kernel helpers cost an SVC round trip.
- **Operating point (OPP).** A supported pair of CPU frequency and supply
  voltage. The vendor table also has a column per silicon speed bin (the
  "vf bin"), read from the chip's eFuse.
- **DCDC3, DCDC5.** Switching regulators in the AXP8191 power-management chip
  (PMIC). The pinned board device tree assigns DCDC5 to the Cortex-A55 cluster
  and the DynamIQ Shared Unit (DSU, which contains the L3 cache), and DCDC3 to
  the Cortex-A76 cluster.
- **SDOT.** The Armv8.2 signed dot-product instruction. One 128-bit SDOT
  performs 16 int8 multiply-accumulates into four int32 lanes.
- **Warm recognition.** Recognition with the model already loaded and the
  recognizer constructed. The goal measures it from PCM input to formatted
  transcription.

## Baseline and the size of the gap

The only physical recognition timing was taken on one Cortex-A55 core. The CPU
clock was the rate boot0 leaves configured, decoded from registers as
26 MHz × 38 ÷ 2 = 494 MHz; no independent frequency measurement exists. One
NEON run took 19.48 s of recognition compute for 8.87 s of audio, which is
about 2.2 times the audio duration ([docs/mimus.md](../mimus.md)).

The goal for that recording is 0.05 × 8.87 s = 0.44 s. Reaching it needs a
speed-up of about 19.48 / 0.44 = 44 times.

The profile of that run splits compute as follows.

| Stage | Time | Share |
| --- | --- | --- |
| Packed int16 matrix product (`linear_gemm`) | 14.9 s | 76% |
| Activation quantization (`linear_quant`) | 1.6 s | 8% |
| Attention matrix products | 1.6 s | 8% |
| Softmax | 0.7 s | 4% |

The matrix product executes 4.17 × 10⁹ multiply-accumulates in 14.9 s, which
is 280 million per second, or 0.57 per cycle at 494 MHz. An earlier portable
build spent 83% of its 39.09 s in linear products.

No single change closes a 44-times gap. Four independent levers multiply:

1. raise the CPU clock;
2. make the inner matrix kernel cheaper per multiply-accumulate;
3. run recognition on more and faster cores, including the two Cortex-A76
   cores (cores 6 and 7);
4. remove serial overheads that sit outside the matrix product.

The following arithmetic shows how the levers could combine. It is an
illustration of scale. None of the factors has been measured.

| Step | Assumption | Compute for the 8.87 s recording |
| --- | --- | --- |
| Today | one A55 at 494 MHz | 19.5 s |
| Kernel and quantization rewrite | `linear_gemm` 14.9 s to about 5 s, quantization 1.6 s to about 0.3 s | about 8 s |
| Clock | 494 MHz to 1794 MHz, compute-bound share only | about 2.5–3 s |
| Two A76 cores in place of two A55 cores | unmeasured per-core gain of about 2 times | about 1.3–1.5 s |
| More workers and SDOT kernels | engine changes in the mimus repository | needed for 0.44 s |

The table shows that the target needs every lever, including engine changes to
use more than two workers. Clock and kernel work alone leave a gap of roughly
five to seven times.

## Recurring causes

Four patterns account for most findings in this report. Fixing a pattern once,
in shared code where it already exists, is cheaper than fixing each instance.

- **Tick pacing.** Most device owners advance one register access, one FIFO
  burst or one protocol phase per tick. Instances include USB0 (`usb-1`),
  storage (`storage-1`), radio (`radio-net-2`), the PMIC (`clock-power-pins-6`),
  the GPU (`gpu-1`), video (`video-4`), HDMI EDID (`display-8`) and the PCIe
  PHY (`buses-pcie-6`). The common remedy keeps each driver step bounded. The
  owner task calls the step again while it reports progress, up to a fixed time
  or step budget, and sleeps only on a genuine hardware wait. The per-step
  deadline and liveness checks stay inside the loop. `taskYIELD` only yields to
  tasks of equal or higher priority, so a high-priority owner must not spin on
  it while waiting.
- **Software SHA-256 and CRC on repeated paths.** The tree hashes multi-megabyte
  regions with a portable C SHA-256 in
  [drivers/storage/storage_digest.c](../../drivers/storage/storage_digest.c) and
  private copies of it. Instances: per-segment model hashing (`bluetooth-2`),
  per-submit NPU hashing (`npu-1`), per-poll USB health frames (`npu-2`),
  firmware hashing one block per tick (`radio-net-7`), frame re-hashing
  (`display-7`), boot hashing (`security-misc-11`, `supervisor-apps-11`) and the
  bit-serial model CRC (`mimus-perf-4`). The remedy is to verify immutable data
  once, then add a faster hash path.
- **Per-access validation.** Several drivers re-run ownership, clock-tree,
  mapping or table checks on every register access. Instances: storage
  (`storage-2`), DSI (`display-6`), GPU queues (`gpu-10`), DMA (`dma-iommu-8`),
  UART and SPI (`buses-pcie-3`), I2S (`audio-10`) and xHCI (`usb-9`). The remedy
  validates at admission or at a phase boundary and keeps only cheap checks per
  access. Each change must keep the driver's documented failure contract.
- **Byte-wise copies and uncached scans.** The runtime `memcpy` copies one byte
  at a time (`runtime-memory-2`, `build-system-12`). Video, camera and display
  paths read megabytes of uncached guard padding byte by byte (`video-3`,
  `camera-5`, `display-7`).

# Part 1. Warm recognition latency

## Summary

| Findings | Change | Estimated gain | Prerequisite |
| --- | --- | --- | --- |
| `cpu-startup-3`, `bringup-gaps-6` | Raise the CPU clock through a fixed operating point | up to 3.6× on compute-bound code; 2.4× at the first safe step | PMIC write support for DCDC5/DCDC3, eFuse bin read, thermal policy, physical approval |
| `mimus-perf-2` | Multi-row int32 NEON matrix kernel | 2.5–3.5× on `linear_gemm` | mimus repository change, parity checks |
| `mimus-perf-3` | Exact SDOT kernel for int16 × int8 | about 2× over the int32 kernel | `mimus-perf-2`, compiler flags, per-core feature check |
| `smp-6`, `runtime-memory-3`, `mimus-perf-10`, `build-system-1` | Separate compiler flags for recognition code | a few percent to about 10%; enables SDOT and CRC32 instructions | parity re-check |
| `smp-3`, `bringup-gaps-8`, `x-concurrency-2` | Run recognition on the A76 cores, then on more workers | unmeasured; largest multicore lever | qualified A76 task execution and sustained SMP; engine change for more than two workers |
| `mimus-perf-7` | Install the engine's asynchronous job interface | 0.5–1 s (3–5%) | none beyond the two-worker images |
| `mimus-perf-8` | NEON quantization and maximum-absolute scans | 1.6 s to about 0.3 s | none |
| `mimus-perf-9` | Rate-limit the per-row yield hook | 1–5% | none |
| `mimus-perf-4` | Fast model CRC | 0.4–0.8 s per request outside compute time | none |
| `bluetooth-2`, `audio-8`, `bringup-gaps-7` | Load and verify models once; measure the full warm path | removes per-utterance reload; makes the goal metric measurable | retained-object contract update |
| `smp-4`, `smp-5`, `x-concurrency-7`, `smp-10`, `cpu-startup-11`, `usb-4`, `supervisor-apps-12` | Reduce kernel-entry, lock and polling overhead on recognition cores | small today; grows as kernels speed up | measurement first |

## Raise the CPU clock

**Findings:** `cpu-startup-3` (high), `bringup-gaps-6` (high).
**Files:** [docs/mimus.md](../mimus.md),
[docs/bringup-sequence.md](../bringup-sequence.md),
[drivers/power/a733_pmic.c](../../drivers/power/a733_pmic.c),
[apps/mimus/recorded.cc](../../apps/mimus/recorded.cc).

**Current behavior.** No SunstoneOS stage changes the CPU supply voltage or
reprograms a cluster PLL. The only CPU clock code is a read-only decode of the
little-core PLL in `recorded.cc`. Both clusters run at the rates boot0 leaves.
The A55 cluster decodes to 494 MHz. The A76 cluster rate is never read.
[docs/hardware.md](../hardware.md) has a status line titled "Thermal and
frequency control: implemented", but only the thermal sensors and alarms
exist. That line should be corrected.

**Available operating points.** The pinned vendor table
`sun60iw2p1-cpu-vf.dtsi` (cached under `~/.cache/sunstoneos/sources/npu-platform/`)
has separate voltage columns for a 24 MHz and a 26 MHz reference. This board uses
a 26 MHz reference, so only the `opp-microvolt-26m-vfXXXX` columns apply.

- The highest 26 MHz entry for the A55 cluster is 1794 MHz. It is valid only
  for bins vf0000, vf0200, vf0400, vf0500, vf0401 and vf0501, at 900–1050 mV.
  Bins vf0100, vf0300 and vf0600 list 0 there, which means unsupported.
- The 1196 MHz entry needs 800 mV for most bins and 850 mV for vf0100. It is
  the recommended first step. Boot0 may already supply enough voltage for it.
- The A76 cluster has a different table that reaches between 1.944 and
  2.002 GHz depending on the bin.
- The DSU has its own table (`dsu_opp_table`). Its current rate is not decoded.
  Matrix products that miss the L1 cache depend on it.

**Proposed change.** Add one fixed-board step that runs before recognition,
under a single owner of each cluster:

1. Read the vf bin from the eFuse through the table's `vf_mapping_table`. No
   reader exists today. Refuse to change the clock if the bin is unknown.
2. Add DCDC5 and DCDC3 selector decoding, a write path and rail-ledger entries
   to the PMIC driver, checked against the pinned AXP8191 sources. The current
   provider reads only DCDC1, DCDC2, DCDC4, DCDC8 and DCDC9
   ([docs/pmic-runtime.md](../pmic-runtime.md)). DCDC5 also supplies the DSU,
   so A55 and DSU voltage changes must be coordinated.
3. Read the present DCDC5 selector. Raise the voltage first if the target point
   needs more, and wait for the ramp.
4. Switch the cluster clock mux to the 24 MHz oscillator, reprogram the PLL
   factors following the pinned `ccu-sun60iw2-cpupll.c` sequence, wait for lock
   with a deadline, and switch back. Read every write back.
5. Record the DSU rate beside the cluster rates.

The step can run in the EL3 boot path before secondary cores start, or later
in the startup sequence, as long as one owner controls each cluster.

**Expected gain.** Linear matrix products are compute-bound on the in-order
A55, so their time scales close to inversely with the clock. The bound for the
A55 cluster is 1794 / 494 = 3.6 times on bins that support 1794 MHz. The first
step gives 1196 / 494 = 2.4 times. Parts limited by DRAM or the DSU gain less.
This is the largest single lever and it applies to single-core and multicore
runs alike.

**Prerequisites.**

- Hardware risk. A wrong voltage or PLL sequence can hang the only connected
  board and lose its USB connection permanently. Prove the sequence in a
  host-modeled check first. Apply it on the board only with explicit physical
  test approval, starting at 1196 MHz.
- Thermal. The thermal driver reports alarm and critical events but does not
  throttle or stop work. Points above the first step need a thermal policy
  first.
- Goal ordering. [docs/goal.md](../goal.md) places optimization after hardware
  functions work. CPU rate control is itself a listed hardware function, so the
  driver is bring-up work first and a performance lever second.

**How to measure.** Offline: a register-model host check of voltage-before-
frequency ordering, PLL lock wait and timeout, and mux order against the pinned
driver. Physical: time a fixed instruction loop against the 24 MHz counter
(`CNTPCT_EL0`) to confirm the frequency independently, then re-run the recorded
mimus workload and compare compute time.

## Rewrite the inner matrix kernel

### Accumulate in int32 and block several rows

**Finding:** `mimus-perf-2` (high).
**Files (mimus repository at commit 3e05158, pinned by
[apps/mimus/source.json](../../apps/mimus/source.json)):**
`firmware/components/asr/src/aarch64/sum8.h`,
`firmware/components/asr/src/gemm_combine_portable.cc`,
`firmware/components/asr/src/kernels.cc`.

**Current behavior.** The NEON kernel `Sum8` processes one activation row per
call. For each input index k it broadcasts one int16 activation, loads 16 bytes
of int16 weights, multiplies with `SMULL`/`SMULL2` and adds into four int64
accumulators with `SADDW`/`SADDW2`. That is about eight 128-bit instructions per
eight multiply-accumulates. `CombinedSums` calls it with `rows=1`, so the same
weights are reloaded for every activation row. Block sizes come from ESP32
constants (12 KiB and 48 KiB) instead of the A55 cache sizes. The engine already
computes a flag, `fits32`, that proves when an int32 sum cannot overflow, but the
summation still widens to int64. [docs/mimus.md](../mimus.md) documents the int64
widening as a deliberate exactness choice.

**Proposed change.** Add an AArch64 kernel to the mimus port that processes four
rows by eight output channels. It loads eight activations per row and uses
`vmlal_laneq_s16` into int32 lanes. The flush interval to int64 must come from
each matrix's real weight bound:

- For layers with int8 weights (|w| ≤ 128) and int16 activations (|x| ≤ 32767),
  one product is at most 2²². Up to 511 terms fit in int32, so `fits32` layers
  need no flush or a flush every 511 terms.
- Packed layers also include int4 weights re-expanded to int16 (`m.rescale`).
  Their weights reach 7 × 3000 = 21000 in magnitude, so only about three
  products fit in int32. These layers keep the int64 path.

The P4 build already has a multi-row branch in `RowsSerial`, guarded by
`CONFIG_IDF_TARGET_ESP32P4`. It does not reuse weights, because `GemmCombine`
loops over rows on the outside, so a new kernel is still needed. The four-row
blocks must not cross a worker's `ForRowRanges` boundary, and the row-level
`MaybeYield` calls must stay. Retune block sizes for the A55 after confirming its
L1 and L2 sizes from the A733 records.

**Expected gain.** Arithmetic drops from about eight to about two 128-bit
instructions per eight multiply-accumulates. Four-row blocking cuts weight loads
four times. The estimate is 14.9 s to about 4–6 s for the packed share at the
same clock, or 2.5–3.5 times. `linear_gemm` also contains output scaling and
non-packed paths, so the stage as a whole gains less.

**Prerequisites.** A change in the mimus repository. The kernel must reproduce
the recorded parity CRCs, and the exactness section of
[docs/mimus.md](../mimus.md) must be updated.

**How to measure.** Extend the mimus Unicorn kernel check
(`tools/aarch64_kernel_check.py`) with multi-row cases at the accumulator
extremes (|x| = 32767, w = −128, k = 511, 512 and 1024) and compare bytes with the
portable kernel. Run [checks/mimus-linear.cc](../../checks/mimus-linear.cc) and
`checks/mimus-parity.py` in QEMU and require identical feature and
log-probability CRCs. Physical timing follows on the board.

### Use SDOT for int16 activations and int8 weights

**Finding:** `mimus-perf-3` (high).
**Files:** mimus `kernels.cc` and `expand.cc`;
[cmake/smp-runtime.cmake](../../cmake/smp-runtime.cmake),
[cmake/cxx-runtime.cmake](../../cmake/cxx-runtime.cmake).

**Current behavior.** The deployed model stores int8 weights and quantizes
activations to int16. Every linear layer widens the int8 weights to int16 and
multiplies int16 by int16, which doubles weight bytes. The build passes
`-march=armv8-a`, so the compiler cannot emit SDOT.

**Proposed change.** Split each int16 activation x into two int8 values,
h = x >> 8 and l = (x & 255) − 128. Then
Σ x·w = 256·Σ h·w + Σ l·w + 128·Σ w. Each partial sum is at most K·2¹⁴, so int32
SDOT lanes cannot overflow for K < 131072, and the int64 combination is exact.
Implementation steps:

- Pack int8 weights once in a four-channel by four-k interleave, and store each
  channel's Σ w.
- Split each quantized activation row into the h and l planes once per row.
- Select the kernel at compile time. The mimus asr `CMakeLists.txt` forbids a
  runtime registry. Compile only that source with `-march=armv8.2-a+dotprod` or a
  target attribute.
- At startup, read `ID_AA64ISAR0_EL1.DP` on every core and stop if any core lacks
  it. [docs/hardware.md](../hardware.md) records core types only for cores 2–7.
- Use it only for int8-weight packed blocks. Int4 rescaled blocks stay on the
  int16 path.

**Expected gain.** One SDOT performs 16 int8 products, which is eight effective
int16-by-int8 products after the split, against four for a widening multiply-
accumulate. Weight traffic halves. The estimate is about two times over the
int32 kernel of `mimus-perf-2`, and 5–7 times over today's kernel. The estimate
is unmeasured; memory bandwidth may bound it.

**Prerequisites.** `mimus-perf-2` (the row-blocking structure), a per-file
compiler flag override in [cmake/mimus-smp.cmake](../../cmake/mimus-smp.cmake),
and a Unicorn build that executes dot-product instructions.

**How to measure.** Unicorn or QEMU (`-cpu max`) comparison against the
portable reference over the extremes x ∈ {−32767, −1, 0, 1, 32639, 32640, 32767},
w ∈ {−128, 127} and K up to 4096, with byte-identical float outputs. Then QEMU
parity on the bench and paced bundles.

## Give recognition code its own compiler flags

**Findings:** `smp-6` (low), `runtime-memory-3` (medium), `mimus-perf-10`
(medium), `build-system-1` (medium).
**Files:** [cmake/smp-runtime.cmake](../../cmake/smp-runtime.cmake),
[cmake/cxx-runtime.cmake](../../cmake/cxx-runtime.cmake),
[cmake/mimus.cmake](../../cmake/mimus.cmake),
[cmake/mimus-smp.cmake](../../cmake/mimus-smp.cmake),
[cmake/a733-mimus-native.cmake](../../cmake/a733-mimus-native.cmake).

**Current behavior.** `sunstone_smp_compile` and `sunstone_cxx_compile` apply
kernel flags to every target, including the recognition libraries `mimus_asr`
and `mimus_asr_smp`: `-march=armv8-a -mstrict-align -ffreestanding -fno-builtin
-ffp-contract=off -O2`, with no `-mcpu` or `-mtune`. The same flag list is
copied into many other CMake files.

The effects on recognition are:

- Clang schedules for a generic core instead of the in-order dual-issue A55.
- `-march=armv8-a` hides CRC32, dot-product and LSE atomic instructions, which
  both core types implement.
- `-fno-builtin` turns `std::abs`, `sqrtf`, `fmaf`, `memmove` and small fixed
  `memcpy` calls into function calls.
- `-mstrict-align` makes `MimusQuantizeS16` load floats as scalar 32-bit values
  through the stack.

The hot matrix kernels are NEON intrinsics, which these flags barely affect.

**Proposed change.** Define one compute option set and apply it only to the
recognition sources: `mimus_asr`, `mimus_asr_smp`, the `apps/mimus` C++ sources
and the targets that compile mimus sources directly (`a733-mimus-native`,
`a733-mimus-board`, `a733-i2s-application`). The set:

- `-mcpu=cortex-a55`, which produces code that also runs on the A76;
- `+crc` and `+dotprod`, after the per-core feature check in `mimus-perf-3`;
- no `-fno-builtin` and no `-mstrict-align`;
- `-ffp-contract=off`, `-fno-exceptions` and `-fno-rtti` kept;
- no FP16, which would threaten float parity.

Boot, monitor, driver and kernel C keep the current flags. Relax alignment only
after confirming that every buffer the library touches (arena, model data, input
buffers) is mapped Normal cacheable in each image. `-march=armv8.2-a` enables LSE
atomics by default in Clang; that is acceptable only if every atomic the library
uses lives in Normal cacheable memory. Prebuilt newlib and libstdc++ do not change.
Update the build identity strings that hard-code the flags, such as
`armv8-a;O2` in [cmake/a733-smp-cached.cmake](../../cmake/a733-smp-cached.cmake).

**Expected gain.** A few percent to about 10% on scalar-heavy stages
(front end, softmax, residuals, epilogues). The main value is that the CRC32 and
SDOT changes need these flags.

**Prerequisites.** None for tuning. Re-check parity, because `-mcpu` tuning can
change float code generation even with contraction disabled.

**How to measure.** Build both ways. Check with `llvm-objdump` that hot
functions contain no `bl memcpy`, `bl abs` or `fmadd`/`fmla`. Run whole-recording
and paced parity in QEMU with `-cpu cortex-a55`. Compare instruction counts under
QEMU as a proxy until board timing is possible.

## Run recognition on more and faster cores

### Move the existing two workers to the A76 cores

**Findings:** `smp-3` (medium), `bringup-gaps-8` (medium), `x-concurrency-2`
(medium).
**Files:** [apps/mimus/parallel_runtime.cc](../../apps/mimus/parallel_runtime.cc),
[apps/mimus/main.cc](../../apps/mimus/main.cc),
[cmake/a733-mimus-board.cmake](../../cmake/a733-mimus-board.cmake),
[platform/a733/cpu_entry.c](../../platform/a733/cpu_entry.c),
[docs/mimus-smp.md](../mimus-smp.md).

**Current behavior.** The parallel runtime asserts the recognition task on
core 0 and pins a single worker to core 1. Both are Cortex-A55. Each
`ParallelFor` call splits the range in half and synchronizes through two
one-slot FreeRTOS queues. `workers()` returns 2 because the pinned mimus engine
fixes `kMaxWorkers = 2` in `parallel.h` and sizes its per-worker scratch to it.
Several call sites pass exactly two items.

The native eight-core image links this runtime. The integrated recognition
images (mimus-board, I2S transcription and the Bluetooth headset) do not, so
they run inference serially on whichever core takes the task. Cores 6 and 7 are
Cortex-A76 (MIDR part `0xd0b`) and do no recognition work.

**Proposed change.** In two steps.

1. Within SunstoneOS, keep two workers and bind the recognition task and the
   worker to cores 6 and 7. This means updating the core assertions
   (`parallel_runtime.cc` lines 28, 43 and 81), the affinity masks
   (`main.cc` line 149, `parallel_runtime.cc` line 78), the `core_mask=3` report
   (line 88) and the acceptance tooling. Paced capture is pinned to core 1 at
   priority 4 and must be accounted for. Link the runtime into the three
   integrated images.
2. In the mimus repository, raise `kMaxWorkers`, resize the per-worker scratch
   and contexts, and replace the fixed two-item call sites with counts derived
   from the worker count. The A55 and A76 cores differ in speed, so the split
   must allow unequal shares. The matrix row split computes a separator row
   after the join, so an atomic-cursor chunking scheme needs redesign. Every
   output keeps its serial arithmetic order, as
   [docs/mimus-smp.md](../mimus-smp.md) requires.

Replacing the queue handshake with a sequence counter and acquire/release
completion is a separate micro-optimization. At about 126 jobs per second today,
queue cost is negligible; it matters only near the target.

**Expected gain.** Unmeasured. The A76 is out of order with two 128-bit NEON
pipes, so a gain of about two to three times per core over the A55 at the same
clock is plausible. The A76 cluster clock has never been read, so this must be
measured first. With an N-way engine, two A76 and six A55 cores could provide
several times the capacity of two A55 cores on the parallel share.

**Prerequisites.** Physical qualification of task execution on cores 6 and 7,
of worker task bodies and of sustained SMP scheduling. These are listed as
outstanding in [docs/handoff.md](../handoff.md) and
[docs/mimus-smp.md](../mimus-smp.md). Step 2 needs coordinated changes to the
mimus engine and its pin in `source.json`.

**How to measure.** QEMU parity for the recorded and paced fixtures with two
workers and, after step 2, with four or more, comparing CRCs, words and frames
with the host reference. Physical speed-up is measured on the board with the
`WORKERS` record showing the core mask.

### Install the asynchronous job interface

**Finding:** `mimus-perf-7` (medium).
**Files:** [apps/mimus/parallel_runtime.cc](../../apps/mimus/parallel_runtime.cc);
mimus `parallel.cc`, `encoder.cc`, `kernels.cc`.

**Current behavior.** `mimus_parallel_bind` calls `SetParallelFor` but never
`SetAsync`, so `AsyncAvailable()` always returns false. Attention therefore
runs scores, softmax and context on one core, although the engine has a tested
two-core attention pipeline gated on that flag.

**Proposed change.** Implement start, wait and available functions on the
existing worker queues. Start posts a job with a task-function tag; wait
receives the matching sequence number; available returns whether the worker is
idle. Keep the core-1-only, no-nesting and single-submitter assertions and the
context lifetime rule. Count asynchronous jobs in the `WORKERS` record. Weight
expansion overlap stays off: it also needs `SetPipelineMode`, and the engine
reports little gain from it.

**Expected gain.** The matrix products in attention stay on the owner core; only
softmax and scalar block work move. The saving is bounded by softmax (0.70 s)
plus the scalar part of attention, minus synchronization: about 0.5–1 s, or
3–5% of the bench.

**Prerequisites.** None beyond the existing two-worker images. The input handoff
and other asynchronous users also switch on, so parity must be re-run.

**How to measure.** QEMU `smp-mimus-model` and `smp-mimus-replay-model` parity
through `tools/check-mimus-smp.py`, with a nonzero asynchronous job count.

## Remove serial overheads in recognition

### Vectorize quantization

**Finding:** `mimus-perf-8` (medium).
**Files:** mimus `kernels.cc`, `encoder.cc`, `include/mimus/asr/fpu.h`.

**Current behavior.** On AArch64, `fpu::RoundToInt` uses the add-and-subtract
magic-constant rounding plus an integer conversion. `MaxAbs` uses four scalar
lanes. The compiler vectorizes part of `MimusQuantizeS16`, but `-mstrict-align`
turns its loads into scalar loads and its stores into halfword stores.
`QuantizeS16WithMax`, used for attention rows, keeps its unrolled path only for
the ESP32 targets.

**Proposed change.** Add an `__aarch64__` branch to `RoundToInt` using
`FCVTNS`, which rounds to nearest even like the magic constant for |y| < 2²².
Add explicit NEON implementations of `MimusQuantizeS16`, `MimusQuantizeS8` and
`MaxAbs` using `vld1q_f32`, `vcvtnq_s32_f32`, `SQXTN` and `vst1_s16`. These
instructions are permitted under strict alignment. `vmaxq_u32` over the float
bits shifted left by one reproduces `MaxAbs` bit for bit. Enable the
`QuantizeS16WithMax` fast path on AArch64 and keep its NaN-preserving fallback.

**Expected gain.** `linear_quant` from 1.6 s to about 0.3 s, roughly 6–7% of
total. The `QuantizeS16WithMax` change benefits attention time separately.

**Prerequisites.** None.

**How to measure.** Host or Unicorn comparison of codes and scales against the
portable reference over random rows and edge values (zero, denormals, ±qmax,
NaN and infinity rows), then QEMU bench parity.

### Stop yielding on every matrix row

**Finding:** `mimus-perf-9` (medium).
**Files:** [apps/mimus/recorded.cc](../../apps/mimus/recorded.cc),
[apps/mimus/main.cc](../../apps/mimus/main.cc),
[include/FreeRTOSConfig.h](../../include/FreeRTOSConfig.h); mimus
`gemm_combine_portable.cc`, `gemm_portable.cc`, `parallel.h`.

**Current behavior.** The engine documents `MaybeYield` as a coarse hook, but
the matrix kernels call it once per activation row. Because `CombinedSums`
passes one row per call, that is roughly 1.7 × 10⁵ to 7 × 10⁵ calls per bench.
The SunstoneOS hook runs the native checkpoint and then `taskYIELD`
unconditionally. That is an SVC (an SMC in secure-monitor builds) that saves and
restores the full integer and 32 × 128-bit FPU context and, in SMP images, takes
the global kernel locks. Preemption and time slicing are already enabled.

**Proposed change.** Keep the supervised checkpoint and call `taskYIELD` only
when a counter quantum, such as 1 ms, has passed since the last yield. The
quantum must stay well inside the checkpoint's watchdog window. Install the hook
from one helper instead of three copies.

**Expected gain.** At an estimated 600–1500 cycles per yield, 0.2–1 s per bench
(1–5%). The share grows as the kernels get faster.

**Prerequisites.** None.

**How to measure.** QEMU parity unchanged; a yield counter in the `PROFILE` or
`WORKERS` output should drop to about one per millisecond of compute.

### Replace the bit-serial model CRC

**Finding:** `mimus-perf-4` (medium).
**Files:** mimus `model.cc`, `crc32.cc`, `pipeline/src/crc32.cc`;
[apps/mimus/service.cc](../../apps/mimus/service.cc),
[tools/usb-mimus.py](../../tools/usb-mimus.py).

**Current behavior.** The span overload of `mimus::asr::Crc32` in `model.cc`
runs eight shift-and-xor steps per byte. Clang partly vectorizes it, but it
still costs about 25 instructions per byte. `Model::Load(..., verify_crc=true)`
applies it to both models (14.06 MB) on every recognition request, and the USB
service applies it again when committing an asset. The library already has a
slice-by-4 implementation in `crc32.cc`, and the pipeline component has a third
copy.

**Proposed change.** Make the span overload forward to the slice-by-4
implementation, and add a path using `__crc32d`/`__crc32b` compiled with `+crc`
for that one file only, with the portable path kept for host checks. Consolidate
the three copies. Stopping re-verification of already verified models is a
separate decision, because [docs/mimus.md](../mimus.md) documents that
`Model::Load` checks tensor CRCs; see the next section.

**Expected gain.** About 0.4–0.8 s per 14 MB pass today. The CRC32 instruction
runs at about 0.3–0.5 cycles per byte, which reduces a pass to about 10–15 ms.
Slice-by-4 alone gives about five to six times. This time lies outside
`compute_us` but inside the latency a user sees.

**Prerequisites.** The compiler flag change for `+crc`.

**How to measure.** A host test that the bitwise, slice-by-4 and instruction
results agree over random buffers and the real model files, with QEMU or Unicorn
for the instruction path. The QEMU bench parity record must not change.

### Load and verify models once per session

**Findings:** `bluetooth-2` (medium), `audio-8` (low), `bringup-gaps-7` (low).
**Files:** [apps/bluetooth/recognition.cc](../../apps/bluetooth/recognition.cc),
[apps/i2s/recognition.cc](../../apps/i2s/recognition.cc),
[apps/mimus-board/recognition.cc](../../apps/mimus-board/recognition.cc),
[apps/mimus/recorded.cc](../../apps/mimus/recorded.cc),
[apps/mimus/paced.cc](../../apps/mimus/paced.cc).

**Current behavior.**

- `mimus_transcribe_recorded` calls `Model::Load` with CRC verification and
  `Recognizer::Create` on every call, then destroys both.
- The Bluetooth repeated-recognition loop also SHA-256-hashes both models
  (7,501,968 and 6,560,592 bytes) for every segment. The mimus-board keyboard
  loop hashes once but still reloads for every trigger.
- The I2S task hashes both models only after the capture is published, so the
  hash sits between end of speech and transcript.
- The recorded path times only `Recognize`. It excludes WAV parsing, punctuation
  and `FormatSequence`, and it never reports load time.
- The paced replay path already holds one engine across segments and reports
  `complete_us` from PCM through formatting.

**Proposed change.**

- In the repeated paths (mimus-board and Bluetooth), reuse the paced pattern:
  load and create once, then call `Recognize` per segment. Hash the models once
  before the first segment, and load with `verify_crc=false` after the SHA-256
  check succeeds. Keep the live and deadline cancellation checks.
- Retained warm objects need a contract change. The current contract destroys
  all temporaries before return ([docs/bluetooth-native.md](../bluetooth-native.md),
  [docs/mimus.md](../mimus.md)). Retained objects must also follow the retention
  policy after uncertain failures.
- Do not rely on `board_application_pins` as proof that the model range is
  read-only. It checks identity mapping, attributes and shareability only. Rely
  instead on the documented exclusion of remote writers, or re-verify once at
  teardown.
- In the I2S task, call both model checks before waiting for capture.
- In `recorded.cc`, add `load_us` around load and create, and `total_us` from
  WAV parsing through `FormatSequence::Finish`, beside `compute_us`.
- Leave the USB recorded path rebuilding per start, because its models may
  change between starts ([docs/mimus-usb.md](../mimus-usb.md)).

**Expected gain.** Portable SHA-256 on the A55 at 494 MHz takes an estimated
90–230 ms for 14.06 MB. Adding the CRC pass and model repacking, each repeated
segment pays several hundred milliseconds before recognition. That is small
today against about 3.3 s of recognition per 1.5 s segment. At the goal it
exceeds the budget: 75 ms for a 1.5 s wideband (mSBC) segment, or 150 ms for a
3.0 s narrowband (CVSD) segment. The I2S change removes 0.1–0.25 s from end of
speech to transcript. The timing change makes the goal metric measurable.

**Prerequisites.** The retained-object contract update.

**How to measure.** Run `checks/bluetooth-recognition-repeated.cc` and
`apps/mimus-board/check-repeated.cc` with several segments. Assert one hash and
one load, transcript parity per segment and `total_us ≥ compute_us`. For I2S,
`checks/i2s-mimus.py` must keep identical PCM and text hashes and log the model
check before capture publication.

## Reduce kernel and scheduling overhead on recognition cores

These items cost little at today's 19.5 s compute time. They grow in relative
weight as the kernels speed up and as recognition spreads across cores. Measure
each one before changing it.

### Avoid SVCs for core identity and thread-local state

**Findings:** `smp-4` (low), `x-concurrency-7` (low).
**Files:** [platform/a733-smp-cached/continue.S](../../platform/a733-smp-cached/continue.S),
[runtime/newlib_smp.c](../../runtime/newlib_smp.c),
[patches/freertos-smp-asm.patch](../../patches/freertos-smp-asm.patch),
[patches/freertos-smp-c.patch](../../patches/freertos-smp-c.patch),
[patches/freertos-smp-macro.patch](../../patches/freertos-smp-macro.patch).

**Current behavior.** Tasks run at EL0, so `portGET_CORE_ID`, the interrupt
enable and disable helpers and newlib's `__getreent` (SVC 118) each take an SVC.
One critical-section enter and exit pair costs four SVCs. The interrupt-mask
path writes the GIC priority mask and then runs `DSB SY` and `ISB`. Upstream
`vTaskEnterCriticalFromISR` and `portEND_SWITCHING_ISR` call `portGET_CORE_ID`,
so interrupt handlers also take SVCs, although the patched
`ucPortGetCoreIDFromIsr` already reads `tpidr_el1` directly. Every SVC and IRQ
on every core also runs `clear_bhb`, a 24-iteration branch loop plus `DSB SY`
and `ISB`. This loop mitigates Spectre-BHB, a branch-history attack, for the
A76.

**Proposed change.**

1. Route the two upstream interrupt-path call sites to the existing
   `FROM_ISR` core-ID variant.
2. At EL1 entry, write the logical core index into `TPIDRRO_EL0`, which EL0 can
   read, and make the task-context core-ID helper a single `MRS`.
3. Write the current task's reentrancy pointer into a per-core EL0-readable
   register in the context-switch restore path, before the return to the task.
   This keeps the migration safety that SVC 118 provides. Do not share one
   register between the core ID and the reentrancy pointer.
4. Run `clear_bhb` only on cores 6–7 and on the EL3-entry and fault paths
   (`continue.S` lines 228 and 243). Removing it from A55 handlers needs a
   citation from Arm's list of affected CPUs.
   [platform/a733/cpu_coherence.c](../../platform/a733/cpu_coherence.c) line 42
   already sets the requirement only for the A76.

**Expected gain.** An estimated 150–300 cycles per SVC and a few microseconds
per queue send and receive pair. Unmeasured.

**Prerequisites.** None, but measure first.

**How to measure.** QEMU SMP context and runtime checks, including migration
with live `errno` and `strtok` state. A disassembly check that interrupt paths
contain no `svc`. A QEMU microbenchmark counting critical-section pairs per
second before and after.

### Lighten the SMP spinlock and separate per-core state

**Finding:** `smp-5` (low).
**Files:** [patches/freertos-smp-c.patch](../../patches/freertos-smp-c.patch),
[patches/freertos-a733-smp.patch](../../patches/freertos-a733-smp.patch),
[cmake/a733-smp-cached.cmake](../../cmake/a733-smp-cached.cmake).

**Current behavior.** The pinned port's recursive lock wraps every owner and
count access in helpers that issue `DSB SY`. Acquire adds two `DMB SY`; release
adds `DMB SY`, `DSB SY`, `SEV`, `ISB` and `DMB SY`. Every task critical section
takes both the task and ISR locks. The per-core arrays
(`ullCriticalNestings`, `ullPortYieldRequired`, `ullPortInterruptNestings`,
`ullOwnedByCore`, `ullPortTaskHasFPUContext`) each fill one 64-byte cache line
that all eight cores write.

**Proposed change.** Take the lock with `LDAXR`/`STXR` and release it with
`STLR`, then `DSB ISHST` and `SEV`. The project's barrier before `SEV` is a
correctness fix, so some DSB must remain between the releasing store and `SEV`.
Use plain accesses for fields written only by their owner. Pack per-core state
into one cache-line-aligned structure per core. Several platform drivers read
`ullCriticalNestings` and `ullPortInterruptNestings` by name
(`shared_locks.c`, `maintenance_watchdog.c`, `hdmi_native.c`, `typec_native.c`,
`a733_spi_platform_native.c`), so they change too. Keep the conservative
barriers in the uncached a733-smp image.

**Expected gain.** Unmeasured. It shortens lock hold time under contention.

**Prerequisites.** A cycle-counter measurement of
`vTaskEnterCritical`/`vTaskExitCritical` on the cached eight-core image.

**How to measure.** QEMU SMP kernel, context, runtime and cleanup cases at two
and eight cores; the native all-core workload instruction model; a litmus-style
review of the acquire and release ordering.

### Let idle cores sleep

**Finding:** `smp-10` (low).
**Files:** [include/FreeRTOSConfig.h](../../include/FreeRTOSConfig.h),
[platform/a733-smp-cached/FreeRTOSConfig.h](../../platform/a733-smp-cached/FreeRTOSConfig.h),
[platform/a733-smp-cached/continue.S](../../platform/a733-smp-cached/continue.S).

**Current behavior.** Both idle hooks are disabled. The cached EL1 entry sets
`SCTLR_EL1 = 0x30d41805`, which leaves `nTWI` clear, so EL0 cannot execute
`WFI`. Every core without work spins its idle task.

**Proposed change.** Enable the idle and passive idle hooks with a hook that
executes `WFI`, through a small SVC or by setting `nTWI`. Keep the priority mask
open so SGI 0 and the tick wake the core. Update the four host models that
expect the current `SCTLR_EL1` value.

**Expected gain.** Power and heat only. Lower heat leaves more thermal headroom
once a thermal policy and higher clocks exist.

**Prerequisites.** Secondary cores have no local tick and wake only on SGI 0
from core 0, so the wake path must be requalified on hardware.

**How to measure.** QEMU SMP cases with idle `WFI`; the native instruction model
showing idle cores entering `WFI` and waking on SGI 0 and the timer interrupt.

### Remove the lock and divides from the shared counter clock

**Finding:** `cpu-startup-11` (low).
**Files:** [platform/a733/counter_clock.c](../../platform/a733/counter_clock.c),
[include/a733_counter_clock.h](../../include/a733_counter_clock.h).

**Current behavior.** `a733_counter_clock_now` is the time source for locks,
the clock registry, DMA, USB, the NPU, the PMIC and the watchdog. Each call masks
interrupts, takes a spin lock bounded at 256 attempts and does three runtime
64-bit divisions. A lost race permanently fails the clock for all clients.

**Proposed change.** Keep the frequency check but convert with the constant
24 MHz, as ticks / 24, so the divisions become multiplications. If the lock is
removed, treat a sample older than the stored maximum as a benign interleaving
and return the stored maximum. Do not adopt a rule that faults on an older
sample: without the lock, ordinary cross-core interleaving produces such samples.
Alternatively keep the lock and make the wait unbounded, since holders run with
interrupts masked. Update the header contract to match.

**Expected gain.** About 100–200 cycles per call on a path every device service
uses, and removal of a contention-only failure mode.

**Prerequisites.** None.

**How to measure.** A host check with concurrent callers using C11 atomics:
monotonic results, no failure under contention, and a fault on an injected
regression. Re-run the lock, watchdog and DMA host checks.

### Keep service tasks off recognition cores

**Findings:** `usb-4` (low), `supervisor-apps-12` (low).
**Files:** [apps/board/main.c](../../apps/board/main.c),
[drivers/usb/a733_typec.c](../../drivers/usb/a733_typec.c),
[include/a733_typec.h](../../include/a733_typec.h),
[drivers/i2c/a733_twi.c](../../drivers/i2c/a733_twi.c).

**Current behavior.**

- The Type-C monitor advances one I2C state per call, about 15 states per byte
  at 100 kHz. Its freshness window (10 ms) and service gap (2 ms) do not allow
  sleeping between states. The board task therefore yields in a loop at
  priority 5 while a transaction is in flight, about half of one core.
- The PMIC task at priority 4 uses the same pattern.
- The board service tasks (PMIC, USB0, Type-C, power, USB2, radio) poll every
  tick with no core affinity. Their failure paths park in
  `for(;;) vTaskDelay(1)`.

The recognizer runs at priority 3. With `configRUN_MULTIPLE_PRIORITIES`, it is
not forced onto the same core today. Contention matters once recognition uses
all cores.

**Proposed change.**

- Sample the Type-C interrupt line (PL9) each tick, a cheap GPIO read, and read
  the status registers over I2C only when it is asserted or at a slower refresh.
  The refresh must still complete both reads and the publish within the 10 ms
  freshness window; the publish extends the VBUS watchdog limit. Keep the 2 ms
  service-gap check.
- Give the TWI engine one shared wait mechanism, interrupt-driven or a bounded
  sub-tick wait, used by both Type-C and the PMIC.
- Pin the service tasks to core 0, or to two service cores, so that the Type-C
  spin cannot hold off the PMIC and watchdog tasks on one core.
- Change only the pure park and stop loops (PMIC after a fault, USB0 stop) to
  `vTaskSuspend(NULL)`. The power task documents that it never suspends, and
  the radio owner and Type-C tasks must keep observing; give those longer delays.

**Expected gain.** About half of one spare core today, and fewer wakeups on
worker cores once recognition is parallel.

**Prerequisites.** Multicore recognition for the benefit to appear.

**How to measure.** In the modeled Type-C and TWI check, count polls and I2C
transactions per 100 ms of steady monitoring. In the linked board cases, count
one-tick delays per modeled second in idle and parked states and check affinity
masks.

# Part 2. Data movement

## Summary

| Findings | Path | Change | Estimated gain |
| --- | --- | --- | --- |
| `usb-1`, `supervisor-apps-4`, `x-concurrency-1`, `x-bounds-2`, `video-5`, `mimus-perf-11` | USB0 evidence channel | Bounded drain loop in one shared transport | up to N times for N passes per tick |
| `npu-2` | USB0 owners | Stop hashing a health frame on every poll | an estimated 20–60% of one core in two owners |
| `host-tools-2` | Host session journal | One journal entry per buffer | removes about 2000 fsyncs per HEVC clip |
| `dma-iommu-1`, `dma-iommu-8` | DMA service | Poll before replying; validate once per iteration | about one tick per transfer |
| `x-coherence-9`, `audio-10` | I2S capture | Drop redundant cache and register operations | small CPU saving |
| `storage-1`, `storage-2`, `storage-5` | SD/eMMC | FIFO-level bursts, burst callback, faster update profile | 40–60 KB/s to several MB/s |
| `runtime-memory-2`, `build-system-12`, `usb-9` | Copies, USB host | Word-wise `memcpy`; looped xHCI polls | several times on copies |
| `radio-net-2`, `radio-net-7`, `radio-net-8`, `radio-net-14` | Wi-Fi and Bluetooth radio | Progress loop, faster hash, coalesced table upload | 20–30 s of bootstrap at the current bus clock |
| `buses-pcie-2`, `buses-pcie-3`, `buses-pcie-7`, `buses-pcie-12` | UART, SPI | Half-FIFO trigger, batched drain, ISR wake | Bluetooth receive from a few KB/s to about 64 KB/s |
| `gpu-1`, `gpu-10`, `x-coherence-8`, `npu-1`, `npu-3`, `x-coherence-5` | GPU, NPU | Run CPU-only steps back to back; verify immutable data once | over 100 times on GPU dispatch overhead |
| `video-3`, `video-4`, `display-6`, `display-7`, `display-9`, `camera-5` | Video, display, camera | One guard scan per frame; wide copies; batched programming | one to two orders of magnitude on copy phases |

None of these paths is on the warm-recognition metric, except the microphone
inputs (Bluetooth UART and I2S). They decide how long each supervised board
session spends moving evidence, and they gate devices that later feed mimus.

## USB0 evidence channel

### Drain the controller while it makes progress

**Findings:** `usb-1` (medium), `supervisor-apps-4` (medium),
`x-concurrency-1` (medium), `x-bounds-2` (medium), `video-5` (medium),
`mimus-perf-11` (low).
**Files:** [drivers/usb/diagnostic.c](../../drivers/usb/diagnostic.c),
[drivers/usb/tusb_config.h](../../drivers/usb/tusb_config.h),
[patches/tinyusb-a733-dcd.patch](../../patches/tinyusb-a733-dcd.patch),
[apps/board/usb0.c](../../apps/board/usb0.c),
[apps/camera/usb0.c](../../apps/camera/usb0.c),
[apps/h264/stream-usb.c](../../apps/h264/stream-usb.c),
[apps/gpu-display/usb.c](../../apps/gpu-display/usb.c),
[apps/hdmi-audio/usb.c](../../apps/hdmi-audio/usb.c).

**Current behavior.** USB0 is the board's only link to the host and carries all
evidence. The A733 patch to the TinyUSB MUSB device driver replaces the
controller interrupt with a task poll. About 27 applications copy the same loop:
`tud_task(); a733_usb_prepare(); dcd_int_handler(0); exchange(); vTaskDelay(1);`.
Four limits combine:

- The MUSB driver writes one 512-byte packet per transmit-complete event. It
  sees that event only on the next `dcd_int_handler` call.
- Endpoint FIFOs are single-buffered; the double-packet-buffer bit (DPB) is not
  set.
- The vendor class completes a transfer every 512 bytes.
- The task sleeps one tick between passes.

Each endpoint therefore moves about one packet per millisecond, roughly
0.3–0.5 MB/s, against about 40 MB/s for high-speed bulk. The mimus diagnostic
protocol also allows one outstanding request of at most 2048 bytes of body.
The gpu-display and hdmi-audio applications already run eight passes per tick.

**Proposed change.** In order of value:

1. **Bounded drain loop.** Repeat `tud_task_ext(0,false)`, `dcd_int_handler(0)`
   and `exchange()` while the controller reports transmit or receive progress
   or the stack has queued events. Bound the loop by counter time (about
   100–200 µs) or a pass count, and sleep one tick only when idle. A fixed count
   of eight passes is not enough: each pass runs microseconds after the packet
   write, before the host has read it. Inside the loop, keep the liveness checks
   (`board_live`, `camera_live`), the `last_progress` update used by the 5 s
   stall check, and the 2 s secure recovery-timer renewal (SMC `0x734` through
   `a733_usb_prepare`), rate-limited but never skipped. Put the loop in one
   shared USB0 transport so the change is made once.
2. **Same-pass response.** In `diagnostic.c`, write the response in the same
   pass that dispatched the request. This saves one tick per round trip with no
   protocol change.
3. **Larger transfers.** Raising `CFG_TUD_VENDOR_RX_EPSIZE` alone does not
   lengthen receive transfers. The vendor class sizes them to one packet unless
   `CFG_TUD_VENDOR_RX_NEED_ZLP=1`. That option requires the host to end every
   request whose length is a multiple of 512 with a zero-length packet, a
   protocol change for `tools/usb-mimus.py` and the other host tools. Larger
   transmit transfers also need the 512-byte caps removed from
   [apps/jpeg-encode/usb0.c](../../apps/jpeg-encode/usb0.c) and
   [apps/h264-encode/usb0.c](../../apps/h264-encode/usb0.c).
4. **Double buffering.** Setting DPB allows at most two packets per poll and
   needs driver changes that are untested. It helps only after step 1.
5. **Larger mimus messages.** Raising `BODY_MAX` also requires the vendor buffer
   sizes, the 2048-byte check and 4096-byte read in `usb-mimus.py`, the 2044-byte
   cap for command `0x15` in `service.cc`, and the request buffers and stack in
   `diagnostic.c`. A versioned header or a capability field in status command
   `0x10` lets the host detect the new limit.
6. **Interrupt wake.** Routing the USB0 interrupt to the owner task reverses a
   deliberate decision in the driver patch and depends on USB0 interrupt
   delivery, which is outstanding. It is a separate, hardware-qualified step.

The H.264 image takes its TinyUSB configuration from
`platform/a733-storage-update/tusb_config.h`, where the vendor buffers are
2048 bytes. Its per-fragment storage revalidation is a documented contract
([docs/h264-native.md](../h264-native.md)); moving it to command boundaries
requires updating that document.

**Expected gain.** At most N times for N passes per tick, minus the cost of PIO
FIFO writes and the host's polling rate. A several-megabyte camera, NPU or
storage export drops from seconds per megabyte to a fraction of that. Small
frames of about 1 KB gain little. Request-response protocols with 4 KiB
requests are also bounded by the host's per-chunk work; see `host-tools-2`.
The native mimus image reads its assets from storage at `0x43000000`, so model
upload over USB affects only the USB mimus image.

**Prerequisites.** None for step 1. Steps 3, 5 and 6 need host tool or hardware
changes.

**How to measure.** Extend the modeled-endpoint USB0 cases
([checks/board-application.py](../../checks/board-application.py),
`checks/usb-zlp.py`) with a transmit model that completes after N µs. Assert
more than one packet per pass, unchanged request and acknowledgement order, and
completion of a 4168-byte response in one or two modeled ticks. Physical
throughput is timed by the host in a later bounded board session.

### Stop hashing a health frame on every poll

**Finding:** `npu-2` (medium).
**Files:** [apps/npu-runtime-large/usb.c](../../apps/npu-runtime-large/usb.c),
[apps/npu-runtime-large/runtime.c](../../apps/npu-runtime-large/runtime.c),
[apps/accelerators/usb.c](../../apps/accelerators/usb.c),
[apps/npu-runtime/usb.c](../../apps/npu-runtime/usb.c).

**Current behavior.** To make request expiry latch while a host stalls,
`a733_observe_privileged()` builds and discards a full 1024-byte response
frame on every poll. Building it ends in a SHA-256 over 992 bytes, which is 16
compression rounds. The large-graph and accelerator owners call it eight times
per tick, about 128 compressions per millisecond. The plain NPU runtime calls it
once per tick.

**Proposed change.** Export an `npu_runtime_expire()` that only runs the expiry
check, and call it at the same point in the loop. Build and hash a frame only
when a response is queued. Merging the three owners is a separate design item,
because the accelerator owner also multiplexes a GPU protocol with its own
failure handling.

**Expected gain.** An estimated 20–60% of one core for the large-graph and
accelerator owners after USB preparation, depending on the unmeasured cost of
one compression on the A55; about one eighth of that for the plain runtime.

**Prerequisites.** None.

**How to measure.** In the host USB poll checks (`npu-runtime-host.py`,
`npu-large-host.py`), assert that expiry of an abandoned upload still latches,
that sent frames are byte-identical, and that idle polls make no digest calls.

### Journal host sessions per buffer

**Finding:** `host-tools-2` (medium).
**Files:** [tools/check-a733-smp.py](../../tools/check-a733-smp.py),
[apps/hevc/host.py](../../apps/hevc/host.py),
[apps/g2d/host.py](../../apps/g2d/host.py).

**Current behavior.** `save()` serializes the whole session record with
indentation to a temporary file, fsyncs it, renames it and fsyncs the
directory. The HEVC host calls it before every 512-byte upload write, about 1025
times for a 524,288-byte clip. Each pending status poll costs about ten fsyncs
and writes two frame files. Python's indented JSON encoder is the slow pure
Python path. All of this runs inside the native session's fixed lifetime of
59 s. The G2D and deinterlace hosts were already changed to one entry per
buffer for this reason.

**Proposed change.** Port the G2D pattern to the HEVC host for upload, query
and acknowledgement sends: one entry per buffer holding the attempt offset and
accepted bytes, saved once per buffer. Keep `ack_sent` saved once the
acknowledgement write completes. For pending polls, durably keep frames whose
state changes plus a count and hash of identical replies. An append-only
JSON-lines journal is an optional later redesign.

**Expected gain.** Several seconds of a 59 s window returned to the device, and
fewer false capture-window expiries on slow disks. Each expiry costs one of the
scarce single-attempt board runs.

**Prerequisites.** None.

**How to measure.** Run `capture_device` against the HEVC fake device with a
maximum-size upload and assert that `save()` calls scale with buffers instead of
packets. Time the save path on tmpfs and on disk.

The host-tools audit area also records that the shared launch path gives every
FEL transfer a fixed 15 s timeout, which a 14 MB preload with full readback
exceeds. That must be fixed before any physical mimus timing run.

## DMA and audio capture

### Report current DMA state and notify on completion

**Finding:** `dma-iommu-1` (low).
**Files:** [drivers/dma/dma_service.c](../../drivers/dma/dma_service.c),
[apps/dma-copy/main.c](../../apps/dma-copy/main.c),
[apps/uart/dma.c](../../apps/uart/dma.c),
[apps/i2s/client.c](../../apps/i2s/client.c).

**Current behavior.** The DMA worker polls all channels, blocks up to one tick
for a request, then executes it without polling again. STATUS, PERIOD_GET and
REAP replies can therefore report the phase from before the worker blocked.
Clients poll with `vTaskDelay(1)`. A transfer that finishes in microseconds is
observed about one tick later, sometimes two. DMA interrupt delivery is not
implemented.

**Proposed change.** Before `execute()`, poll the channel of the request's job
when it is progressing. Add an optional per-client task notification on entry to
RESULT or RECOVERY, so clients block on `ulTaskNotifyTake` with a finite
timeout. Change the clients to block on the reply or notification; the worker
change alone gains little. The reply and ticket protocol stays as it is.

**Expected gain.** About one tick less per transfer. These are bring-up
applications; the benefit matters for back-to-back transfers and I2S period
hand-off.

**Prerequisites.** None.

**How to measure.** Extend [checks/dma-service.c](../../checks/dma-service.c)
with a transfer that completes after the pre-block poll. Assert that the next
STATUS reports RESULT without an intervening tick and that the notification
fires once per terminal transition.

### Validate DMA ownership once per worker iteration

**Finding:** `dma-iommu-8` (low).
**Files:** [platform/a733/dma_native.c](../../platform/a733/dma_native.c),
[platform/a733/dma_pool_session.c](../../platform/a733/dma_pool_session.c),
[arch/aarch64/device_memory.c](../../arch/aarch64/device_memory.c),
[drivers/dma/dma_memory.c](../../drivers/dma/dma_memory.c).

**Current behavior.** Every controller register read runs the full worker
ownership check (about 15 comparisons, startup state, three atomic loads,
scheduler state, current task and exception level), then an address check and
`DSB SY`. Writes carry two `DSB SY`. One poll does 7–12 register reads. Every
cache-maintenance call re-reads `CurrentEL`, `SCTLR_EL1` and `CTR_EL0`.

**Proposed change.** Run the ownership check once per worker iteration and keep
it on every entry path that can run outside one. Keep the cheap address check
per access. Drop the trailing barrier on reads and the leading barrier on
ordinary writes, and keep the barriers before doorbell and enable writes.
Device-nGnRE accesses to one peripheral stay in order without a DSB. Cache the
cache-line stride after the MMU and caches are enabled, and keep returning 0
before that, because the per-call `SCTLR_EL1` check is how maintenance refuses
with caches off. Keep the per-submit descriptor rebuild, which is cheap and
revalidates each job.

**Expected gain.** Two to four times less CPU per poll, which is microseconds per
tick, under 1% of a core.

**Prerequisites.** None.

**How to measure.** Run [checks/dma-controller.c](../../checks/dma-controller.c),
`dma-service.c` and the pool-session check; count callbacks per poll from the
model's MMIO trace.

### Trim cyclic I2S receive and verified writes

**Findings:** `x-coherence-9` (low), `audio-10` (low).
**Files:** [drivers/dma/a733_dma_controller.c](../../drivers/dma/a733_dma_controller.c),
[drivers/audio/a733_i2s_stream.c](../../drivers/audio/a733_i2s_stream.c),
[drivers/audio/a733_i2s_platform.c](../../drivers/audio/a733_i2s_platform.c).

**Current behavior.** Cyclic receive invalidates each 4 KiB period, copies it
with volatile byte loads, then invalidates it again. The application copies the
data twice more. For every verified register write, the platform reads the
register back under the register mutex, and the stream's `checked()` reads it
again under a second mutex entry. One `requests()` or `control()` call costs
three mutex enter and leave pairs.

**Proposed change.** Drop the post-copy invalidate and replace the byte loop with
`memcpy` after the invalidate and barrier. Both the documented contract in
[docs/i2s-stream.md](../i2s-stream.md) and the count asserted in
[checks/i2s-cyclic.c](../../checks/i2s-cyclic.c) must change with it. Either
drop the stream's re-read and state in the I/O contract that a successful write
has been read back, or drop the platform read-back and keep its flush-bit
masking. A multi-slot client ring is optional; at 16 kHz the four-period raw
ring already gives about 256 ms of slack.

**Expected gain.** A few microseconds per period boundary. This is CPU
cleanliness more than a latency fix.

For mimus microphone capture, the audio audit area recommends one large finite
DMA transfer per utterance. That avoids per-period re-arming within one FIFO
depth (2–4 ms at 16 kHz) on a 1 ms polling worker.

**How to measure.** The I2S stream and cyclic host models must capture identical
bytes, with one invalidate per period and the expected drop in I/O count per
boundary.

## Storage

### Move data in FIFO-level bursts

**Finding:** `storage-1` (medium).
**Files:** [drivers/storage/sunxi_mmc.c](../../drivers/storage/sunxi_mmc.c),
[include/sunxi_mmc.h](../../include/sunxi_mmc.h),
[platform/a733-storage-update/worker.c](../../platform/a733-storage-update/worker.c),
[apps/storage-read/native.c](../../apps/storage-read/native.c).

**Current behavior.** The SD/eMMC core moves at most 16 FIFO words (64 bytes)
per poll and reads STATUS before every word. Every owner polls once per tick.
Each command phase also costs at least one tick. A 1 KiB read takes about
24 ms and a 4 KiB read about 72 ms, so every bus mode gives about 40–60 KB/s;
the 50 MHz high-speed mode buys almost nothing. Writes are single-block CMD24
followed by CMD13 and are slower still. The controller FIFO holds 256 words,
and STATUS bits 27:17 report its fill level.

**Proposed change.**

1. Read STATUS once per step, take the FIFO level, and move up to one 512-byte
   block without per-word status reads. For writes, free space is 256 minus the
   level. Keep the per-step bound well below 256 words so fault checks still
   bound work after a failure.
2. While a data or CMD13 phase is active, run a fixed number of bounded steps
   per tick, or wake on a notification. Sleep a full tick only on genuine waits
   such as the 1 ms OCR and programming retries. Do not busy-yield at high
   priority: `taskYIELD` does not let lower-priority tasks run.
3. A separate read limit larger than 4 KiB needs coordinated changes in
   `sunxi_mmc_read`, `a733_mmc_platform.c`, the storage-read service limit
   (`STORAGE_READ_MAX_BLOCKS`), its borrowed buffer, and the update path's
   two-block chunk.

The 16-word bound is a documented contract ([docs/mmc.md](../mmc.md)) and an
assertion in [checks/sunxi-mmc.c](../../checks/sunxi-mmc.c); both change with
the code.

**Expected gain.** From about 0.06 MB/s to several MB/s, then bounded by the
per-access cost addressed in `storage-2`.

**Prerequisites.** Model support for the FIFO level, and hardware confirmation of
the level field's meaning on this controller.

**How to measure.** Extend `checks/sunxi-mmc.c` and `checks/sunxi-mmc-sd-modes.c`
so the model exposes the FIFO level. Assert that an eight-block read finishes in
at most eight data polls with existing completion, CRC and retained-fault cases
passing under sanitizers.

### Add a burst FIFO callback

**Finding:** `storage-2` (medium).
**Files:** [drivers/storage/a733_mmc_platform_native.c](../../drivers/storage/a733_mmc_platform_native.c),
[drivers/storage/a733_mmc_platform.c](../../drivers/storage/a733_mmc_platform.c).

**Current behavior.** Each FIFO word makes two guarded accesses, one for STATUS
and one for data. Each access runs the task-context check twice, about four
clock samples, `xSemaphoreGetMutexHolder` (which enters a kernel critical
section) and two `DSB SY`. In the eight-core image, that takes the global kernel
locks once per four bytes of card data. Each poll also re-verifies the clock tree
with about 20–25 register reads.

**Proposed change.** Add `read_fifo` and `write_fifo` callbacks to
`sunxi_mmc_ops`. Check task, mutex ownership and aperture once per burst, read
the FIFO level once and transfer at most that many words with plain device
accesses and one barrier at the end. Keep the per-burst retained check and the
update-only gate on FIFO writes. Cache "the owner holds the register mutex" at
`enter()`, verify it at `leave()`, and clear it on retain. Move clock-tree
re-verification to command admission and completion.

**Expected gain.** An estimated five to ten times less CPU per byte, and no
cross-core lock traffic on the data path.

**Prerequisites.** `storage-1`.

**How to measure.** Extend [checks/a733-mmc-platform.c](../../checks/a733-mmc-platform.c)
and `checks/a733-storage-native-mmc.c` to refuse a wrong task, an unowned mutex,
an out-of-aperture address and an oversize burst, and assert callback counts per
step.

### Speed up the write-capable maintenance path

**Finding:** `storage-5` (medium).
**Files:** [platform/a733-storage-maintenance/session.c](../../platform/a733-storage-maintenance/session.c),
[include/a733_storage_update.h](../../include/a733_storage_update.h),
[drivers/storage/a733_storage_update.c](../../drivers/storage/a733_storage_update.c),
[tools/storage-update.py](../../tools/storage-update.py).

**Current behavior.** The maintenance image is the only one allowed to write.
Its session decoder never sets an SD bus profile, so it runs 1-bit 400 kHz. The
protocol moves two sectors (1 KiB) per USB round trip, and the host tool
hard-codes the same stride in four places. A 4 MiB package takes about 100 s
for the backup session plus about 6 minutes to apply. eMMC grants require
profile 0, so eMMC stays at 400 kHz as well.

**Proposed change.** Add a profile word in the reserved session bytes (456–487)
and pass it to `b.mmc.sd_profile`, with profile 0 as the default. Raise the
chunk to 4096 bytes, with `A733_STORAGE_BODY` at least 88 + 32 + 4096. Report the
chunk size in the INFO response so the host derives its stride from one value.
Keep responses off multiples of 512 bytes or send a zero-length packet. Keep the
pre-write backup re-read, which is a deliberate safety check. Regenerate the
commitment-chain fixtures.

**Expected gain.** Four times fewer round trips and journal writes; at faster
profiles the bus time also drops. A 4 MiB update could plausibly take tens of
seconds. eMMC needs its own bus-width work.

**Prerequisites.** New modeled write and CMD13 checks at profiles 1 and 2,
which are host-checked only for reads.

**How to measure.** Run `checks/a733-storage-update.py` and
`checks/a733-storage-maintenance.py` with 4 KiB chunks and profile 2 under
sanitizers, plus a host-tool unit test that the stride comes from INFO.

## Memory copies and the USB host

### Copy eight bytes at a time

**Findings:** `runtime-memory-2` (medium), `build-system-12` (medium).
**Files:** [runtime/memory.c](../../runtime/memory.c),
[runtime/memmove.c](../../runtime/memmove.c),
[drivers/usb/a733_xhci.c](../../drivers/usb/a733_xhci.c),
[cmake/a733-smp-cached.cmake](../../cmake/a733-smp-cached.cmake).

**Current behavior.** The board, storage, cached diagnostic and most device
images link `runtime/memory.c` and `runtime/memmove.c`, whose `memcpy`,
`memmove` and `memset` are byte loops. `-mstrict-align` and `-fno-builtin` keep
the compiler from widening them. Copies from the NC pool, such as xHCI bounce
buffers, issue one uncached load per byte. The mimus images link newlib's
routines instead.

**Proposed change.** When source and destination share 8-byte alignment, copy
with aligned 64-bit general-register loads and stores; otherwise fall back to
bytes. Do not use a shift-merge for the misaligned case: it reads whole aligned
words outside the source range, which is a side-effect risk on Device memory.
Use no SIMD and no `DC ZVA`, so the routines stay valid at EL3 and under strict
alignment. Keep `-fno-builtin` so the compiler cannot turn the loops into calls
to themselves.

**Expected gain.** Reads from NC memory gain the most, up to about eight times
fewer bus transactions. Stores to NC memory gain less, because the A55 write
buffer merges them. Cached copies gain several times. The 100–200 ns cost per
uncached read has not been measured on the A733.

**Prerequisites.** None.

**How to measure.** A host differential test against byte loops for every
source and destination offset modulo 16 and lengths 0–300, plus overlapping
`memmove` in both directions; a disassembly check for no SIMD and only aligned
accesses; the existing QEMU kernel and board fixtures.

### Loop xHCI polls while they progress

**Finding:** `usb-9` (low).
**Files:** [drivers/usb/a733_xhci.c](../../drivers/usb/a733_xhci.c),
[drivers/usb/a733_xhci_tinyusb.c](../../drivers/usb/a733_xhci_tinyusb.c),
[apps/board/client.c](../../apps/board/client.c).

**Current behavior.** In polling mode the USB host owner calls
`a733_usb2_native_poll` once per tick. Each call performs one xHCI event or one
submission. Each register access runs the platform guard twice. A mass-storage
sector read needs at least six polls, about 6 ms.

**Proposed change.** Call the poll repeatedly while it reports progress, up to
32 iterations per tick, matching the interrupt profile. Keep the per-access
guards: detecting detach or provider loss before the next doorbell is part of
the bounded-failure design. Multi-descriptor rings with larger bounce buffers
wait for a multi-block consumer.

**Expected gain.** Ten times or more on enumeration and short transfers. No
current consumer depends on it.

**How to measure.** In the host model, count owner-loop ticks for high-speed
mass-storage enumeration plus a 64-sector read before and after.

## Wi-Fi and Bluetooth radio

### Repeat transport steps while they progress

**Finding:** `radio-net-2` (medium).
**Files:** [apps/board/main.c](../../apps/board/main.c),
[drivers/net/a733_radio_session.c](../../drivers/net/a733_radio_session.c),
[drivers/net/aic8800_sdio_transport.c](../../drivers/net/aic8800_sdio_transport.c),
[drivers/storage/sunxi_sdio.c](../../drivers/storage/sunxi_sdio.c).

**Current behavior.** The radio owner runs one transport step per tick: one
SDIO command admission, one controller poll of at most 16 FIFO words, or one
record parse. At the current 1-bit 400 kHz bus (about 50 KB/s), the bus itself
is the throughput limit, so the 16-word budget does not bind. The tick costs
time around small control steps, mainly the 2880 Bluetooth patch-table word
writes. The cold bootstrap's bus time alone is about 43 s, longer than the 40 s
bootstrap deadline in [apps/board/radio_profile.c](../../apps/board/radio_profile.c).

**Proposed change.** Report "made progress" or "waiting" from the transport and
dispatcher. In the radio owner, repeat the session poll while it progresses,
within a per-tick CPU budget, and yield when the device reports idle. Keep the
fault observation before and after each step and the documented per-call bound.
Retune the FIFO budget only together with a faster bus profile, and update
[docs/sdio.md](../sdio.md) and [docs/radio-transport.md](../radio-transport.md).

**Expected gain.** About 15–25 s of bootstrap at the current clock. After a
4-bit or faster mode is qualified, ten to a hundred times lower latency per
operation. The bootstrap still exceeds its deadline without a faster bus.

**Prerequisites.** A faster SDIO mode for most of the gain. SMP scheduling is
unqualified, so a busy budget takes CPU from other work on the same core.

**How to measure.** In the transport and firmware host model with a
tick-accurate clock, count ticks to start confirmation and for a 100-frame
exchange; require at least two times fewer ticks with existing checks passing.

### Hash firmware in larger steps

**Finding:** `radio-net-7` (medium).
**Files:** [drivers/net/aic8800_firmware.c](../../drivers/net/aic8800_firmware.c),
[drivers/net/aic8800_runtime.c](../../drivers/net/aic8800_runtime.c),
[drivers/storage/storage_digest.c](../../drivers/storage/storage_digest.c).

**Current behavior.** The firmware engine compresses one 64-byte SHA-256 block
per step, one step per tick. The four firmware images total about 391 KB, about
6110 blocks, so 6.1 s pass before the first firmware command. The same
compression function is implemented in the firmware engine, the runtime binding
verifier and `storage_digest.c`.

**Proposed change.** Hash a bounded budget per step; a few KiB already reduces
verification below 100 ms. Replace both private copies with
`storage_digest.c`, which means adding it to the radio source lists in
[cmake/a733-board.cmake](../../cmake/a733-board.cmake) and
[cmake/radio-network.cmake](../../cmake/radio-network.cmake).

**Expected gain.** About 6 s off radio startup and about 60 lines of duplicated
code.

**How to measure.** The radio firmware host check still accepts the published
hashes and rejects a mutated byte; count steps from verification to preparation.

### Coalesce the patch-table upload

**Finding:** `radio-net-8` (medium).
**Files:** [drivers/net/aic8800_firmware.c](../../drivers/net/aic8800_firmware.c),
[docs/radio-firmware.md](../radio-firmware.md).

**Current behavior.** The Bluetooth patch table is uploaded as one confirmed
memory-write command per address and value pair, each padded to a 512-byte
block. In the published table, 2766 of the 2793 trap pairs form one run of
consecutive RAM addresses from `0x0020b43c`. The engine already implements
confirmed block writes for images.

**Proposed change.** While uploading, detect runs of consecutive RAM word
addresses and emit block writes of up to 256 words. Keep register addresses and
record order as word writes. The pinned vendor source writes this table word by
word, so block writes into this region are not established. Gate the change
behind a profile flag, keep the word path as the default, and qualify it by
reading back a sample of the region on hardware.

**Expected gain.** About 2755 fewer confirmed round trips, about 28 s of bus time
at the current clock. The byte saving shrinks on a faster bus; the round-trip
saving remains.

**Prerequisites.** Hardware readback qualification.

**How to measure.** In the host model, the final radio memory image must match
with and without coalescing, and the command count must fall from 2880 to about
125. Physically, compare readbacks of the run.

### Copy Ethernet frames fewer times

**Finding:** `radio-net-14` (low).
**Files:** [drivers/net/aic8800_network.c](../../drivers/net/aic8800_network.c),
[drivers/net/aic8800_data.c](../../drivers/net/aic8800_data.c).

**Current behavior.** A transmitted frame is copied five times and a received
frame three times, partly with byte loops. Each of the eight queue slots holds a
1.5 KB frame copy.

**Proposed change.** After the SDIO speed work, have the data encoder write
directly into the transport buffer, or have the transport point at the encoder's
buffer, and replace byte loops with `memcpy`. Queuing FreeRTOS+TCP descriptors
instead of frames saves more but needs care: the stack keeps ownership when
`bReleaseAfterSend` is false, every drop path must release the descriptor, and
retaining resources after a fault would hold shared network buffers.

**Expected gain.** Negligible against today's bus; about 7.5 KB of byte copies
per transmitted frame once the bus is faster.

**Prerequisites.** A faster SDIO mode.

**How to measure.** The data and network host check compares transmitted bytes
with the reference vectors and confirms the network buffer count returns to its
initial value.

## UART and SPI

### Raise the UART receive trigger level

**Finding:** `buses-pcie-2` (medium).
**Files:** [drivers/uart/a733_uart.c](../../drivers/uart/a733_uart.c),
[drivers/uart/a733_uart_platform_native.c](../../drivers/uart/a733_uart_platform_native.c),
[include/bt_h4.h](../../include/bt_h4.h).

**Current behavior.** Every FIFO control value the driver writes (1, 7 and 9)
sets a receive trigger of one character. The Bluetooth UART runs at 1.5 Mbaud
with automatic flow control. On this controller the RTS line goes inactive when
the receive FIFO reaches the trigger and returns only when the FIFO is empty. The
owner drains at most 64 bytes per tick (`BT_H4_BUDGET`). The peer is therefore
throttled after about every byte, and sustained receive falls to roughly one to
three bytes per millisecond. The headset path carries 16-bit PCM at 8 kHz, which
needs about 16 KB/s plus headers. The pinned Linux driver uses a one-character
trigger only at 9600 baud or below.

**Proposed change.** Use one fixed rule: a quarter-FIFO trigger (64 bytes),
matched to the 64-byte drain budget, or a half-FIFO trigger with a larger
budget, whenever automatic flow control is on. Write `(rt << 6) | 1` and
`(rt << 6) | 7`. Make the native allowlist accept exactly the planned values.
Leave the UART2 DMA value (9) unchanged, because in DMA mode the trigger also
sets the DMA request threshold. Expect the receive-timeout interrupt cause to be
the normal end of a burst in interrupt mode. Document the rule in
[docs/uart.md](../uart.md).

**Expected gain.** Bluetooth receive from a few KB/s to about 64 KB/s with the
current budget, which covers SCO audio for the mimus microphone path.

**Prerequisites.** A board measurement of the threshold behavior.

**How to measure.** Extend `checks/a733-uart.py` and the sanitized model to
assert the planned FIFO control writes and refusal of any other value. Physical
throughput needs a Bluetooth test.

### Drain UART bytes in batches

**Finding:** `buses-pcie-3` (medium).
**Files:** [drivers/uart/a733_uart.c](../../drivers/uart/a733_uart.c),
[drivers/uart/a733_uart_platform.c](../../drivers/uart/a733_uart_platform.c),
[drivers/spi/a733_spi.c](../../drivers/spi/a733_spi.c),
[drivers/spi/a733_spi_platform.c](../../drivers/spi/a733_spi_platform.c).

**Current behavior.** Each received UART byte reads LSR and then RBR. Each read
runs about four time samples and about eight task-context checks, which mask
interrupts and query the scheduler. The SPI loop already reads the FIFO level
once but runs the timing, context and mutex-owner checks per byte.

**Proposed change.** Read LSR once. If its FIFO-error bit (bit 7) is clear, read
the receive FIFO level register (offset `0x84`) or USR, and drain that many
bytes. Fall back to per-byte status when an error is present. Both address
allowlists must admit the level register. Check the deadline before and after
each batch. Hoist context checks to the exchange or interrupt entry, where
ownership cannot change. For SPI, move the per-byte checks to once per batch.

**Expected gain.** Several times less CPU per byte. At 150 KB/s the current
cost is an estimated 7–20% of one core.

**How to measure.** In [checks/a733-uart.c](../../checks/a733-uart.c), assert at
most 66 MMIO reads and four time samples for a clean 64-byte burst, with the
error-flag cases passing.

### Wake the UART task from its interrupt

**Finding:** `buses-pcie-7` (low).
**Files:** [apps/uart/irq.c](../../apps/uart/irq.c),
[apps/uart/run.c](../../apps/uart/run.c).

**Current behavior.** The UART interrupt handler masks the source and sets a
flag but does not notify the task, which runs on `vTaskDelay(1)`. Interrupt mode
therefore has polling latency. The SPI application already notifies from its
handler.

**Proposed change.** Call `vTaskNotifyGiveFromISR` and `portYIELD_FROM_ISR` in
the handler and wait with `ulTaskNotifyTake(pdTRUE, 1)`, as
[apps/spi/irq.c](../../apps/spi/irq.c) does. BUSY and PENDING results still wait
on the timeout. Add a shared helper only when a second consumer adopts it.

**Expected gain.** Up to 1 ms less per interrupt. The application is a 32-byte
bring-up test, so this matters as a template for future interrupt-mode users.

**How to measure.** A host check asserting that the handler notifies the owner
and the task blocks on the notification with a one-tick fallback.

### Remove the duplicate SPI profile check

**Finding:** `buses-pcie-12` (low).
**Files:** [drivers/spi/a733_spi_platform.c](../../drivers/spi/a733_spi_platform.c),
[drivers/spi/a733_spi.c](../../drivers/spi/a733_spi.c).

**Current behavior.** While a transfer is active, each poll validates the clock,
reset and interrupt profile twice under the same mutex hold, about 16 register
reads, and moves at most 64 bytes.

**Proposed change.** In the ready state, call `profile()` only when the core will
not run its own admission check, or drop the redundant `parent()` call in
`registers()`. Raising the budget and adding interrupt-paced transfers waits for
a flash or display consumer.

**How to measure.** In `checks/a733-spi-platform.py`, count clock-register reads
per poll (one instead of two) with unchanged wire traces.

## GPU and NPU

The mimus audit area concludes that GPU and NPU offload is not needed for the
0.05x target and would be hard to keep bit-exact. These findings matter when a
GPU or NPU workload enters a pipeline.

### Run GPU CPU-only steps back to back

**Findings:** `gpu-1` (medium), `gpu-10` (low), `x-coherence-8` (low).
**Files:** [drivers/gpu/pvr_gpu_service.c](../../drivers/gpu/pvr_gpu_service.c),
[drivers/gpu/pvr_render_service.c](../../drivers/gpu/pvr_render_service.c),
[apps/accelerators/gpu.c](../../apps/accelerators/gpu.c),
[platform/a733/gpu_memory.c](../../platform/a733/gpu_memory.c),
[arch/aarch64/device_memory.c](../../arch/aarch64/device_memory.c).

**Current behavior.** The GPU worker advances one micro-step per tick. A compute
dispatch takes about 6–13 ms of tick waits, a 1080p readback hundreds of
milliseconds, and session opening hundreds of milliseconds. The ready state
counts as active, so an idle session wakes every tick, and the demo client
sends STATUS every tick. Every 4-byte queue access runs two address
translations per page and separation tests against 22 buffers. GPU cache
maintenance is issued per 4 KiB page, with two `DSB SY` each.

**Proposed change.**

- Have each sub-step report progress or a device wait. Loop CPU-only steps
  within a documented step or time budget, preserving the rule that request
  traffic cannot starve the worker.
- Block on the request queue or the GPU interrupt when only GPU progress is
  outstanding. Let the ready state block indefinitely only when no work or
  deadline is pending.
- Notify the client on completion instead of STATUS polling.
- Keep the mask-and-rearm interrupt protocol in
  [docs/gpu-interrupts.md](../gpu-interrupts.md); skip the pair only when no
  event was processed.
- Verify queue-page mappings at initialization, publication and compute data
  binding, and keep only bounds, phase and destination checks per word.
- Pass contiguous spans, within the 16-page budget, to one cache-maintenance
  call, splitting at guard pages.

**Expected gain.** Over 100 times on CPU-only dispatch overhead; idle sessions
stop waking every tick.

**How to measure.** In the host service checks with a modeled clock, count
worker iterations from compute acceptance to completion and from open to ready;
assert no timed wakeups in idle ready state and zero translations per
steady-state control poll.

### Verify immutable NPU data once

**Findings:** `npu-1` (medium), `npu-3` (low), `x-coherence-5` (low).
**Files:** [drivers/npu/npu_convolution.c](../../drivers/npu/npu_convolution.c),
[drivers/npu/a733_npu_memory.c](../../drivers/npu/a733_npu_memory.c),
[include/npu_convolution.h](../../include/npu_convolution.h).

**Current behavior.** Every submit and every startup poll recomputes SHA-256 over
the 1 MiB model copy and the NN, coefficient, command and bootstrap buffers. The
command and bootstrap buffers are 512 KiB each and mostly padding. In the
NN-pool profile that is 2,930,304 bytes per job, an estimated 90–150 ms at
494 MHz. Each submit also re-translates every page, rescans every table word and
cleans all immutable buffers, about 2 MB of already clean lines.

**Proposed change.**

- Check the bootstrap buffer and the model copy only on the bootstrap publish
  at OPEN.
- Hash the command body and footer and check the padding structurally.
- Replace the private SHA-256 with `storage_digest_bytes`, which removes
  duplication; it is not faster.
- Add an INPUT role so each submit cleans only INPUT and POOL and
  clean-invalidates only WRITE buffers.
- Keep the per-submit table and mapping checks unless a documented policy
  change replaces them; the code states "Do not trust a hash" for tables.
- Read-only CPU mappings would conflict with the writable-mapping requirement
  in [docs/npu-convolution.md](../npu-convolution.md); treat that as a separate
  design.
- Fix the stale "512 KiB" input comment.

**Expected gain.** Most of the per-submit digest and cache cost, tens of
milliseconds per job.

**How to measure.** In [checks/npu-convolution.c](../../checks/npu-convolution.c)
and [checks/npu-memory.c](../../checks/npu-memory.c), count digest bytes and
cache callbacks per submit; mutated coefficient and command cases must still be
refused before publication.

## Video, display and camera

### Scan guards once per frame and copy wide

**Findings:** `video-3` (high), `camera-5` (medium), `display-7` (low).
**Files:** [apps/h264/native.c](../../apps/h264/native.c),
[apps/h264-encode/storage.c](../../apps/h264-encode/storage.c),
[apps/isp0/native.c](../../apps/isp0/native.c),
[apps/isp0/memory.c](../../apps/isp0/memory.c),
[apps/dsi-scanout/input.c](../../apps/dsi-scanout/input.c),
[apps/dsi-scanout/native.c](../../apps/dsi-scanout/native.c).

**Current behavior.**

- In the optional High-profile H.264 build, every 32-row copy callback re-reads
  the unused tail of each device span byte by byte from NC memory, several
  megabytes per call, about 55 calls per 1080p frame. Pixels are copied with
  volatile byte loads in every build, about 3.1 MB per 1080p frame.
- The ISP0 copy scans about 71 KB of uncached guards twice per 4 KiB chunk,
  300 scans per frame, an estimated 2–3 s of a 20 s session.
- DSI frame upload re-hashes each completed 8 MB slot from uncached memory
  inside the USB task, and allocation hashes slot 0 again.

**Proposed change.**

- H.264: keep the cheap per-call guard-page checks and scan the large tails only
  at the start and end of a copy and in the refill bracket, with aligned 64-bit
  compares. Update the per-invocation guard contract in
  [docs/h264-native.md](../h264-native.md). Copy pixels with `memcpy` or wide
  loads from the NC alias, handling unaligned rows caused by the crop offset.
  Keep the 32-row bound. Zeroing only the stale slice tail on refill needs an
  explicit high-water mark reset on failure.
- ISP0: scan guards once before the first chunk and once after the last, with
  64-bit compares. A fault during the copy is then reported at the end of the
  stage, with the same retention outcome.
- DSI: hash each fragment with `storage_digest_add` as it arrives. For format
  version 2, take the frame hash from the admitted input and document that it is
  the admitted hash rather than a measurement of the bank.

**Expected gain.** One to two orders of magnitude on the High-build copy phase;
about 1–3 s per ISP0 session; removal of a 150–250 ms USB stall per DSI slot.
The uncached-load latencies behind these estimates are unmeasured.

**How to measure.** Add guard-byte counters to the H.264 High and full-HD models
and require at most two full scans per frame with unchanged digests. Count
`isp0_guards()` calls (2 instead of over 300) and add a mid-copy guard
corruption case that still faults. Assert that `storage_digest_bytes` is never
called on DSI frame memory.

### Batch codec and display programming

**Findings:** `video-4` (medium), `display-6` (medium), `display-9` (low).
**Files:** [drivers/video/a733_video_h264.c](../../drivers/video/a733_video_h264.c),
[drivers/video/a733_video_jpeg_run.c](../../drivers/video/a733_video_jpeg_run.c),
[apps/jpeg/native.c](../../apps/jpeg/native.c),
[drivers/display/a733_display_native.c](../../drivers/display/a733_display_native.c),
[drivers/display/a733_display_pll.c](../../drivers/display/a733_display_pll.c).

**Current behavior.**

- The video drivers perform one register access per poll and every application
  sleeps a tick after each poll. JPEG decode writes 659 registers, at least
  0.66 s per picture. H.264 High needs about 222 programming polls plus 215
  table polls, at least about 0.45 s per picture.
- Every DSI register access re-validates all 8192 IOMMU table words from
  uncached memory and translates every page of the frame bank, an estimated few
  milliseconds per access.
- The display PLL holds the global register mutex for about 150 ticks, one
  register access per tick, blocking USB host and other register users.

**Proposed change.**

- Video: keep the one-operation-per-poll drivers and loop in the task while a
  poll advances state, bounded by a step or time budget. Stop on hardware waits
  and resource BUSY results, and call `board_live()` each iteration.
- DSI: validate table words and page attributes once per phase, and keep a full
  walk at most once per observation cycle; a full re-read detects in-place table
  corruption, which generation numbers cannot.
- PLL: run the non-waiting baseline, profile and program stages back to back in
  one poll while the mutex is held. Keep one sample per lock observation with
  its 5 µs spacing.

**Expected gain.** JPEG table programming falls from 0.66 s to within one tick;
H.264 programming by over 100 times; DSI preparation from an estimated 1.5 s of
CPU to milliseconds; the PLL mutex hold from about 150 ms to a few milliseconds.

**How to measure.** Drive the task loops in
[checks/a733-video-h264.c](../../checks/a733-video-h264.c) and
`checks/a733-video-jpeg-run.c` with a fake tick counter and require byte-identical
register traces. Count mapping and validation calls in `checks/display-native.py`.
Count polls between enter and leave in `checks/a733-display-pll.c`.

# Part 3. Boot time and build time

## Boot time

Persistent boot does not exist yet. Each supervised image currently pays these
costs inside its fixed 2, 20 or 60 s lifetime and within its first watchdog
renewal window.

| Finding | Current cost | Change | Estimated saving |
| --- | --- | --- | --- |
| `security-misc-11` | Portable SHA-256 over the 14.3 MB mimus bundle before the first renewal of a 2 s window | Skip the per-block copy for aligned input; add an Armv8 SHA-2 path | about 0.5 s if SHA-2 instructions exist |
| `supervisor-apps-11` | Same routine used for 14.2 MB of mimus-board inputs and two 1280×720 display frames | Same | as above |
| `cpu-startup-10` | Barrier and counter read after every 64-byte cache clean | Check the deadline once per 4 KiB | 15–25 ms plus the model range |
| `clock-power-pins-6` | One PMIC bus phase per tick, 92 polls per snapshot, four USB supplies in series | Run non-waiting phases in one poll; submit all four requests | about 350 ms |
| `clock-power-pins-11` | Linear token scans under a global lock, twice per TWI access | Cache the slot index in the TWI adapter | sub-microsecond per call |
| `buses-pcie-6` | One PCIe PHY register write per tick | Write the table in one mutex hold | about 100 ms |
| `display-8` | Seven polls per EDID byte | Merge phases; evaluate 8-byte reads | about 1 s |
| `camera-9` | About 310 single-register I2C polls | Block writes for contiguous runs | 0.2–0.3 s |
| `radio-net-2`, `radio-net-7`, `radio-net-8` | Radio bootstrap | See Part 2 | 20–30 s |

### Use SHA-2 instructions where present

**Findings:** `security-misc-11` (low), `supervisor-apps-11` (low).
**Files:** [drivers/storage/storage_digest.c](../../drivers/storage/storage_digest.c),
[apps/mimus/native.c](../../apps/mimus/native.c),
[platform/a733-smp/launch.c](../../platform/a733-smp/launch.c).

**Current behavior.** `a733_smp_diagnostic_start` hashes the whole input bundle
(14,346,688 bytes for the bench bundle, up to 32 MiB) before inference can earn
a watchdog reload. In supervised builds the window is 2 s. `storage_digest_add`
copies every 64-byte block into a tail buffer even when the input is aligned and
contiguous, and `compress()` is portable C built with `-mgeneral-regs-only`.

**Proposed change.** Compress aligned full blocks directly from the input. Read
`ID_AA64ISAR0_EL1` at EL1 or EL3, record it in the next supervised boot's
evidence, and publish the SHA-2 feature bit to EL0 code. If present, add a
`SHA256H`/`SHA256H2`/`SHA256SU0`/`SHA256SU1` compression in a translation unit
built with `+sha2` and without `-mgeneral-regs-only`, keeping the portable path
as reference and fallback. The A55 cryptographic extension is optional, so this
gain is conditional. Do not route bulk hashing through the crypto engine; its
2 KiB tasks are slower for this use.

**Expected gain.** Portable SHA-256 at an estimated 20–25 cycles per byte costs
about 0.6 s for the bench bundle at 494 MHz. SHA-2 instructions run at about
2–3 cycles per byte, about 0.07–0.15 s. The direct-block change gives a few
percent.

**How to measure.** NIST vectors and random-length differential tests against
`hashlib`; the instruction path under the Unicorn AArch64 harness or QEMU
`-cpu max`; unchanged mimus asset digests.

### Check the deadline per page during cache publication

**Finding:** `cpu-startup-10` (low).
**Files:** [platform/a733/cpu_coherence.S](../../platform/a733/cpu_coherence.S),
[platform/a733-smp-cached/continue.c](../../platform/a733-smp-cached/continue.c).

**Current behavior.** `a733_cpu_coherence_publish` runs a deadline check, an
`ISB` plus a counter read and two compares, after every `DC CVAC`. Cached boot
publishes the 2 MiB code block, the RAM image up to `__bss_end` and the tables.
The storage-maintenance patch also publishes the mimus model region at
`0x43000000`, whose length comes from the host and can approach
`0x48000000`.

**Proposed change.** Keep one check before the loop, one at least every 4 KiB
(64 lines) and one after the final `DSB SY`. Keep the start-address alignment
so refusal behavior is unchanged.

**Expected gain.** An estimated 0.7–1.3 ms per MiB, so 15–25 ms for the image
ranges and more for a large model range.

**How to measure.** Run [checks/device-memory-capacity.c](../../checks/device-memory-capacity.c)
and the coherence instruction model: identical clean addresses, refusal before
the first maintenance operation on an expired deadline, and refusal within one
page on a mid-range expiry.

### Batch PMIC phases and USB supply checks

**Findings:** `clock-power-pins-6` (low), `clock-power-pins-11` (low).
**Files:** [drivers/power/a733_pmic.c](../../drivers/power/a733_pmic.c),
[platform/a733/native_usb.c](../../platform/a733/native_usb.c),
[drivers/clock/a733_clock_leases.c](../../drivers/clock/a733_clock_leases.c),
[drivers/i2c/a733_twi_native.c](../../drivers/i2c/a733_twi_native.c).

**Current behavior.** The PMIC provider advances one bus phase per poll. One
snapshot is six register reads of 15 phases each, at least 92 polls, against
about 2.5 ms of wire time at 100 kHz. USB startup verifies four supplies one
after another, about 370 ms. The TWI native adapter scans the clock registry
and rail ledger twice per register access under a global try-lock.

**Proposed change.** In `transfer()`, keep advancing while the step succeeds
and the phase is not a wait whose flag is still clear, within the held I2C
section and a fixed step budget, keeping every deadline check. In
`native_usb.c`, submit all four supply requests at once; this needs four request
handles and per-rail state, with unchanged reap order and failure retention. The
camera path has the same serial pattern. In the TWI adapter, cache the slot
index privately as a hint re-verified with the full token comparison.

**Expected gain.** About 3–6 ms per snapshot and about 15–25 ms for USB supply
verification, about 350 ms saved before USB enumeration.

**How to measure.** In the PMIC runtime model, count polls per snapshot and run
the NACK, timeout and clock-change fault cases. In the native USB host model,
count steps to USB preparation. The clock lease checks must pass, including a
reused-slot case with a stale token.

### Batch PCIe, HDMI and camera configuration

**Findings:** `buses-pcie-6` (low), `display-8` (low), `camera-9` (low).
**Files:** [drivers/pci/a733_pcie_phy.c](../../drivers/pci/a733_pcie_phy.c),
[drivers/display/a733_hdmi.c](../../drivers/display/a733_hdmi.c),
[drivers/display/a733_display_engine.c](../../drivers/display/a733_display_engine.c),
[drivers/camera/ov5640.c](../../drivers/camera/ov5640.c).

**Current behavior and change.**

- PCIe: the PHY table is 91 writes plus four finish writes, one per poll, at
  least 97 ticks. Write the table in one poll under the register mutex, stopping
  at the PLL wait; confirm that holding the mutex for about 95 accesses is
  acceptable to other users.
- HDMI EDID: seven polls per byte, at least 1792 ticks for 256 bytes. Issue the
  setup writes in one poll and the data read plus clear in one poll, keeping the
  per-byte hot-plug check and the deadline behavior; this gives about three polls
  per byte. Evaluate the DesignWare eight-byte read against A733 sources before
  adopting it. Skip the display engine's read-before-write on full-mask words,
  saving 94 polls.
- OV5640: one register write per poll, about 310 polls plus 826 ms of required
  delays. The 219-entry init table collapses to 65 contiguous runs. Send
  unmasked, undelayed runs as block writes of at most 40 data bytes; keep masked
  and delayed entries as single operations; fail each block as a unit.

**Expected gain.** About 100 ms for PCIe, about 1 s for EDID, 0.2–0.3 s for
camera bring-up.

**How to measure.** Poll counts in the PCIe, HDMI and display-engine models with
unchanged write traces; an identical 65,536-byte modeled register image in
`checks/ov5640-reference.py`.

## Build and check time

### Compute image identity at build time

**Finding:** `build-system-7` (medium).
**Files:** [cmake/a733-board.cmake](../../cmake/a733-board.cmake),
[cmake/a733-mimus-native.cmake](../../cmake/a733-mimus-native.cmake),
[cmake/a733-smp-cached.cmake](../../cmake/a733-smp-cached.cmake).

**Current behavior.** Each image hashes its inputs during CMake configuration and
registers every source and header as a configure dependency. Editing any source
re-runs configuration, re-hashes thousands of files (including recursive globs
over TinyUSB, FreeRTOS+TCP and wpa_supplicant headers) and re-runs 13 Python
generation steps. Twenty-six CMake files then rewrite `build-id.c`
unconditionally, so images whose identity did not change still recompile and
relink. The cost applies only to profiles enabled in the build directory.

**Proposed change.** As a first step, switch the 26 unconditional writes to the
temporary-file plus `configure_file(COPYONLY)` idiom already used in
`a733-smp-cached.cmake`. Then add one function, `sunstone_build_id`, that
generates the identity at build time and depends on the full input list,
including globbed headers, and writes only on change. Keep globbed lists as
configure dependencies so new headers are picked up.

**Expected gain.** An edit-build cycle drops from a full reconfiguration plus
many relinks to one compile and the relinks whose identity changed.

**How to measure.** Touch one driver source and build the board image: CMake
must not re-run, and only that object and the affected identities rebuild. A
content edit must still change `build-id.txt`.

### Hook only the addresses that matter in emulated checks

**Finding:** `checks-harness-8` (medium).
**Files:** [checks/storage-read-single-core.py](../../checks/storage-read-single-core.py),
[checks/pwm-native.py](../../checks/pwm-native.py),
[checks/hdmi-framebuffer.py](../../checks/hdmi-framebuffer.py).

**Current behavior.** 120 of the 176 Unicorn code-hook registrations in
`checks/` have no address range, so Python runs on every emulated instruction.
Most callbacks act only at a few addresses. The storage-read hook also reads
memory and runs two regular expressions per instruction. A measured empty global
hook costs 0.31 s per two million instructions, against about 1 ms with a ranged
hook. `hdmi-framebuffer.py` gives all cases a shared 120-second wall-clock budget.

**Proposed change.** Add a shared helper that registers one ranged hook per
address, and a scanner for system-register access encodings, as
`hdmi-framebuffer.py` already does. Migrate hooks whose callbacks dispatch only
on address, after confirming each has no per-instruction side effects. Give
budgets as instruction counts only.

**Expected gain.** Ten to a hundred times less emulation time in the migrated
checks, and results that do not depend on host load.

**How to measure.** Run `storage-read-single-core.py` before and after against
the same image: identical `results.json` and about an order of magnitude less
wall time.

### Stop duplicating toolchains and raw RAM in evidence

**Finding:** `checks-harness-9` (low).
**Files:** [checks/hdmi-application.py](../../checks/hdmi-application.py),
[checks/displayport-native.py](../../checks/displayport-native.py),
[checks/hdmi-audio-native.py](../../checks/hdmi-audio-native.py),
[checks/hdmi-clock-native.py](../../checks/hdmi-clock-native.py),
[tools/evidence-snapshot.py](../../tools/evidence-snapshot.py).

**Current behavior.** The evidence cache is 33 GB. It holds, for example,
18 copies of `libLLVM.so.21.1` (2.49 GB) and 127 copies of the Python binary.
RAM checkpoints are written raw; a sampled 48 MiB dump has 165 nonzero pages
of 12,288 and compresses about 94 times.

**Proposed change.** Hardlink identical tool files within the evidence tree,
which keeps each evidence directory self-contained. Compress RAM checkpoints and
record the hash of the decompressed bytes, so recorded identities and restore
paths keep working, or re-baseline existing evidence.

**Expected gain.** About 6–7 GB of storage and less copy time per run.

**How to measure.** Run one tool-copying check and one checkpointing check twice;
manifests must match, the second run must add little disk usage, and a restore
from a compressed checkpoint must give identical results.

# Optimization roadmap

[docs/goal.md](../goal.md) orders optimization after the required hardware
functions work. The roadmap follows that order for anything that needs the
board, and starts offline work that carries no hardware risk now. Each stage
lists its findings.

## Stage 1: make the goal measurable

1. Add `load_us` and a PCM-to-formatted `total_us` to the recorded path, and
   reuse the paced pattern for repeated recognition (`bringup-gaps-7`,
   `bluetooth-2`).
2. Fix the 15 s FEL transfer timeout recorded by the host-tools audit area, so a
   physical mimus run can load its models.
3. In the next approved supervised boot, record `ID_AA64ISAR0_EL1` on every core
   (SHA-2, CRC32, dot product), decode the A76 and DSU clock rates, and time a
   fixed loop against the 24 MHz counter to confirm the A55 frequency.

## Stage 2: offline changes with no hardware risk

These are small, host-checkable and independent of each other.

- Recognition compute: separate compiler flags (`smp-6`, `runtime-memory-3`,
  `mimus-perf-10`, `build-system-1`); NEON quantization (`mimus-perf-8`);
  rate-limited yield (`mimus-perf-9`); fast CRC (`mimus-perf-4`); asynchronous
  jobs (`mimus-perf-7`); hash once and before capture (`bluetooth-2`, `audio-8`).
- Shared infrastructure: word-wise `memcpy` (`runtime-memory-2`,
  `build-system-12`); direct-block SHA-256 (`security-misc-11`,
  `supervisor-apps-11`); counter clock divide (`cpu-startup-11`); per-page
  deadline checks (`cpu-startup-10`).
- Evidence throughput: the bounded USB0 drain loop in one shared transport
  (`usb-1` and its group); per-buffer host journaling (`host-tools-2`); no
  per-poll health hashing (`npu-2`).
- Build and checks: build-time identity (`build-system-7`); ranged hooks
  (`checks-harness-8`).

## Stage 3: matrix kernels in the mimus repository

1. Multi-row int32 kernel with per-matrix flush bounds (`mimus-perf-2`).
2. SDOT kernel with the hi/lo activation split, after Stage 1 confirms dot-product
   support on every core (`mimus-perf-3`).

Each step must reproduce the recorded parity CRCs and update the exactness
section of [docs/mimus.md](../mimus.md).

## Stage 4: hardware-gated recognition levers

1. CPU operating points: eFuse bin reader, DCDC5 and DCDC3 write support, PLL
   sequence, host-modeled check, then one approved physical step to 1196 MHz
   (`cpu-startup-3`, `bringup-gaps-6`). Higher points wait for a thermal policy.
2. Qualify task execution on cores 6 and 7 and sustained SMP scheduling, then
   move the two-worker runtime to the A76 cores and link it into the integrated
   images (`smp-3`, `bringup-gaps-8`, `x-concurrency-2`).
3. Idle `WFI` and service-task placement (`smp-10`, `usb-4`,
   `supervisor-apps-12`).

## Stage 5: more workers and kernel-entry cost

1. Generalize the mimus engine beyond two workers with unequal shares
   (`smp-3`, `x-concurrency-2`).
2. After measuring them, reduce SVC, barrier and branch-history costs in the SMP
   port (`smp-4`, `smp-5`, `x-concurrency-7`).

After Stage 5, re-profile on the board. If warm recognition is still above
0.05 times the audio duration, the next candidates are higher operating points
under thermal control and the DSU clock.

## Stage 6: device data paths as consumers appear

Schedule these when the device they serve enters an integrated workload.

- Bluetooth microphone: UART trigger level and batched draining
  (`buses-pcie-2`, `buses-pcie-3`).
- Storage: FIFO bursts and the faster update profile (`storage-1`,
  `storage-2`, `storage-5`).
- Radio: progress loops and table coalescing (`radio-net-2`, `radio-net-7`,
  `radio-net-8`, `radio-net-14`).
- DMA, audio and USB host polling (`dma-iommu-1`, `dma-iommu-8`,
  `x-coherence-9`, `audio-10`, `usb-9`).
- Video, display and camera copy phases (`video-3`, `video-4`, `display-6`,
  `display-7`, `display-9`, `camera-5`).
- GPU and NPU stepping and verification (`gpu-1`, `gpu-10`, `x-coherence-8`,
  `npu-1`, `npu-3`, `x-coherence-5`).
- Boot-time batching (`clock-power-pins-6`, `clock-power-pins-11`,
  `buses-pcie-6`, `buses-pcie-7`, `buses-pcie-12`, `display-8`, `camera-9`).
- Evidence storage (`checks-harness-9`).
