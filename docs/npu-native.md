# Native NPU CPU operations

The native NPU adapter is **implemented** and passes a FreeRTOS emulator check.
It supplies CPU translation, cache maintenance, ordered register access,
microsecond timing, and memory-publication callbacks for the
[controller core](npu-controller.md). Physical NPU access remains untested.
The default build does not link it. It is linked into
the NPU board images: `a733-npu-convolution` and `a733-npu-convolution-supervised`
from [apps/npu-convolution](../apps/npu-convolution/CMakeLists.txt), the
`a733-npu-convolution-single-core` profiles, and the combined
`sunstone-a733-accelerators` image from [apps/accelerators](../apps/accelerators/CMakeLists.txt).

The [interface](../include/a733_npu_native.h) contains the private
[memory policy](npu-memory.md), timer-conversion state, register mapping, and
failure state. The [implementation](../platform/a733/npu_native.c) uses shared
[AArch64 memory operations](../arch/aarch64/device_memory.c), also used by the
[DMA adapter](dma-memory.md).

## Initialization checks resources without peripheral access

The caller supplies a dedicated table arena, pinned buffers, one 4 KiB register
mapping, and a fatal handler. Initialization rejects overlaps between registers,
RAM allocations, the arena, and driver state. It checks CPU read/write translation
of the register page against the supplied physical address and Device-nGnRnE or
Device-nGnRE attributes.

The memory policy checks the RAM mappings and constructs private NPU page tables.
Initialization also samples the physical counter and its frequency. It performs
no peripheral register access. Refusal leaves the adapter and table arena
unchanged. These checks do not establish that platform RAM is available or
exclusively reserved; the platform allocator must supply that guarantee.

All calls require EL1, enabled CPU translation and data caches, stable stage-1
mappings, and exclusive ownership of the resources. Stage-2 translation and
physical-to-device bus remapping are unsupported. The production service must
serialize every call and preserve session resources until controller close or
independently established fault recovery.

## Shared CPU operations preserve cache ownership

The shared translation routine executes `AT S1E1R` or `AT S1E1W`. It masks
asynchronous exceptions around the operation and restores both `PAR_EL1` and
the caller's interrupt masks. Translation results contain the physical address,
memory attributes, and shareability. The routine requires enabled MMU/data cache
and supported cache geometry before issuing the translation instruction.

Cache geometry must justify exclusive ownership in 64-byte units. Unknown
writeback granules, granules larger than 64 bytes, and a maintenance stride
larger than the granule are rejected. Maintenance uses `DC CVAC`, `DC CIVAC`, or
`DC IVAC`, with `DSB SY` before and after the range. The
[DMA record](dma-memory.md) contains the architectural references and ownership
requirements for these shared operations.

Publication delegates reservation, command-token, table-content, and live-mapping
checks to the memory policy. It then performs native cache maintenance.
Completion invalidates device-written buffers after the controller confirms full
idle. The adapter does not validate command semantics or independently prove
device completion.

Each register operation checks alignment, page bounds, and the live CPU
translation before a volatile 32-bit access. Barriers order register writes and
complete accesses. A validation or memory-operation failure latches failure,
marks the memory policy faulted, and prevents further register accesses.
Time and barrier callbacks cannot return an error value, so invalid use invokes
the required fatal handler. Returning from that handler traps.

## Counter conversion provides bounded software deadlines

The adapter reads `CNTPCT_EL0` and verifies that `CNTFRQ_EL0` remains equal to
the initialization frequency. It converts counter increments into microseconds
using the [counter converter](../runtime/counter_clock.c). Quotient/remainder
arithmetic preserves fractional microseconds and avoids overflow when scaling
counter increments by one million.

Arm specifies a system counter between 56 and 64 bits wide. The converter uses
the low 56 bits so its wrap behavior does not depend on a wider implementation.
See the [Arm Generic Timer guide, section 2](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Generic%20Timer.pdf?revision=c710e7a7-9f52-4901-8c9d-91b19f44f9c7).

Samples must be fewer than 2^55 ticks apart. A backward or ambiguous sample
fails without changing conversion state; an entire unobserved wrap cannot be
detected. Accumulated microseconds wrap modulo 2^64, matching controller deadline
arithmetic. Calling CPUs must share one fixed-frequency counter domain.

Delay requests are limited to 1,000 microseconds. Each delay returns when its
elapsed time is sufficient or invokes the fatal handler after 1,048,576 samples.
That sample bound prevents a stopped counter from creating an infinite loop.
Frequency changes and invalid counter progression also invoke the fatal handler.
Interrupt preemption can extend a delay beyond its requested duration.

A synchronous interconnect stall can prevent instructions from completing.
Software deadlines therefore do not replace independent watchdog recovery.
Power, clocks, reset ownership, masked NPU interrupts, and watchdog recovery must
be established before physical register access. Their board integration remains
outstanding. The [FreeRTOS service](npu-service.md) now serializes requests from
EL0 tasks through a preemptible EL1 worker and passes an SMP emulator check.

## Focused evidence and physical limits

One [host counter check](../checks/counter-clock.c) passed with AddressSanitizer
and UndefinedBehaviorSanitizer. It compares the converter against 128-bit
arithmetic across six frequencies, fractional accumulation, large increments,
counter wrap, and microsecond wrap. It also checks atomic refusal of backward
or ambiguous samples and supported/unsupported cache geometry.

One [FreeRTOS emulator check](../checks/npu-native.c) passed within a ten-second
limit. Two EL1 tasks perform four serialized rounds each. They exercise native
publication, completion maintenance, counter-based delays, translation permission
faults, neighboring guards, and preservation of interrupt masks and `PAR_EL1`.
Initialization refuses ordinary RAM as registers, substituted physical backing,
and a read-only destination without changing the adapter or arena.

The register check uses the emulated PL011 UART's Device mapping. It reads UART
status and writes one character. It never sends the NPU initialization sequence
to that UART. An invalid register offset latches failure, preserves the output
argument, and causes later register and publication calls to refuse. A subsequent
time callback reaches the expected fatal handler.

The existing DMA emulator check also passed once because its native operations
now use the shared implementation. The host counter check covers the extracted
cache-geometry calculation. Other DMA admission logic and the standalone NPU
controller/memory implementations are unchanged; their broader checks were not
repeated. No mimus test or physical device request occurred.

QEMU has no A733 NPU and does not establish physical noncoherent visibility,
counter accuracy, multicore cache coherence, cancellation, or bus recovery.
The stopped-counter and changed-frequency fatal branches are implemented but
remain unexercised by the native emulator fixture. The separate host memory
model remains the evidence for CPU/device visibility ordering.

Local evidence is under `~/.cache/sunstoneos/evidence/npu-native-20261016/`.
The NPU emulator image has SHA-256
`a0c997421c46f1d6e9198797d85db23230e1b956bf50dda04a050a6eb86eaec9`.
It contains 30,800 bytes of code/constants, 8 bytes of initialized data, and
1,173,072 bytes of BSS and reserved stacks. Most reserved memory belongs to the
fixture's command buffers and tables. These are fixture-image measurements;
the service's separate measurements are in its implementation record. Physical
stack usage and latency remain unmeasured.

Reproduce after relevant inputs change:

```sh
clang-21 -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  checks/counter-clock.c runtime/counter_clock.c -o /tmp/counter-clock-check
/tmp/counter-clock-check
cmake -S . -B ~/.cache/sunstoneos/build/qemu-virt
cmake --build ~/.cache/sunstoneos/build/qemu-virt --target npu-native-model
timeout 10s ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64 \
  -machine virt,gic-version=3,secure=off,virtualization=off \
  -cpu cortex-a55 -smp 1 -m 128M -nographic -monitor none \
  -semihosting-config enable=on,target=native \
  -kernel ~/.cache/sunstoneos/build/qemu-virt/npu-native-model.elf
```

The emulator must exit with status zero and print `PASS NPU native:`.
Fresh toolchain configuration follows [build.md](build.md).
