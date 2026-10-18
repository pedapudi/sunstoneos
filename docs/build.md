# Build and exercise the kernel foundation

The original emulator target is a FreeRTOS AArch64 kernel check for QEMU's `virt`
machine. The [physical A733 check](a733-kernel.md) adds an EL3 monitor and board
interrupt/timer setup.
It runs at EL1 on one emulated Cortex-A55 with a GICv3 interrupt controller,
a physical generic timer, and 128 MiB RAM. It is not an Orange Pi boot image.

Clang 21, LLD 21, CMake, GNU patch, Python 3.12 or later, and
`qemu-system-aarch64` are required. Python verifies source archive hashes against
[the dependency manifest](../sources.lock.json). Downloaded sources and build
output stay in the user cache.

```sh
set -e
cache="$HOME/.cache/sunstoneos"
freertos=$(python3 tools/fetch-sources.py freertos)
cmake -S . -B "$cache/build/qemu-virt" \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-clang.cmake \
  -DFREERTOS_DIR="$freertos"
cmake --build "$cache/build/qemu-virt" -j 8
python3 tools/check-kernel.py "$cache/build/qemu-virt/kernel-check.elf"
```

On the development host, LLD and QEMU were extracted from Ubuntu packages into
`~/.cache/sunstoneos/host`. Before configuring, prepend
`$HOME/.cache/sunstoneos/host/usr/lib/llvm-21/bin` to `PATH`. Pass
`--qemu "$HOME/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64"` to the checker.
The installed host supplies their shared libraries; no system package installation
was required. Package versions were LLD `1:21.1.8-6ubuntu1` and QEMU
`1:10.2.1+ds-1ubuntu3.2`.

The toolchain preserves explicitly supplied `CMAKE_C_COMPILER`,
`CMAKE_ASM_COMPILER` and `CMAKE_CXX_COMPILER` paths. Set `SUNSTONE_LLD` to the
`ld.lld` driver when selecting a linker outside `PATH`; CMake propagates that
selection into its compiler checks. Use a fresh build directory when changing
toolchains, because CMake caches compiler and linker flags.

## What the check establishes

Three statically allocated tasks exercise the timer and scheduler. A producer
sends sequential values through a queue. A higher-priority consumer checks their
order. A lower-priority task runs continuously without yielding, requiring timer
preemption for the other tasks to resume.

Both blocking tasks retain distinct values in a callee-saved floating-point
register across context switches. The check stops after 100 successful exchanges
and requires at least 100 ticks and progress by the non-yielding task. A missing
interrupt, corrupted queue value, failed register comparison, assertion, or
ten-second timeout fails the check.

The ELF uses identity mappings for peripheral and RAM addresses. RAM uses normal
cacheable attributes; peripherals use device attributes. Code and data currently
share writable/executable mappings. Per-region permissions, multicore operation,
dynamic allocation, and physical board services remain unfinished.

The successful image recorded on 2026-10-15 has SHA-256
`d0fcaa3f5985c4ad726000399e5eaeaed53064e27ed3789b7aa1ef7c81ce7787`.
It completed 100 exchanges in 100 ticks. ELF section totals are 18,168 bytes of
text/read-only data, 8 bytes of initialized data, and 71,024 bytes of reserved
RAM, including stacks and translation tables.

The checker writes raw output and input identities to
`~/.cache/sunstoneos/evidence/kernel-check.json`. It reuses a successful result
when the image, emulator, linked host libraries, command, and checker match.
Documentation edits and commits do not require another emulator run.

The separate [SMP target](smp.md) tests two-core scheduling with shared memory,
queues, and mutexes. Select `smp-kernel-model` explicitly to build it; the original
single-core check and physical board images retain their own ports.

The [storage maintenance image](storage-maintenance-launch.md) connects the native
PMIC, storage and USB tasks with watchdog supervision. Configure with both
`FREERTOS_DIR` and `TINYUSB_DIR` pointing to the pinned sources, then select its
target explicitly:

```sh
cmake --build "$HOME/.cache/sunstoneos/build/qemu-virt" \
  --target sunstone-a733-storage-maintenance -j 8
```

The resulting ELF requires a separate immutable launch session and physical
recovery evidence before board execution. Building it does not authorize storage writes.


The [native mimus image](mimus-smp.md#native-eight-core-application-image) uses the
same central build with the SMP C++ runtime and recorded or paced recognition.
Its target is `sunstone-a733-mimus-native`.

The [fixed NPU application](npu-application.md) is a standalone CMake project at
`apps/npu-convolution`. Its target is `a733-npu-convolution`; it requires an
explicit board handoff source and the pinned model, input and reference files.
The preserved refusal fixture is for software checks and cannot admit a board.

The [native board application](board-application.md) is available as
`sunstone-a733-board` when `SUNSTONE_TCP_DIR`, `SUNSTONE_RSN_DIR` and
`SUNSTONE_NETWORK_C_HEADERS` identify the authenticated networking sources and
freestanding C headers. It links USB0 output, USB2 host reads and native radio
startup. Its immutable session still requires qualified hardware-watchdog,
memory and power admission before physical execution.

The `sunstone-a733-camera` target connects native OV5640 capture to USB0 output.
The separate `a733-smp-supervised` target runs the all-core observer with the
watchdog disabled and manual recovery. Both are available through the central
build. Their component documents describe distinct physical admission requirements:
[camera](camera-application.md) and [supervised observer](a733-smp-supervised.md).
