# Build and exercise the kernel foundation

The original emulator target is a FreeRTOS AArch64 kernel check for QEMU's `virt`
machine. The [physical A733 check](a733-kernel.md) adds an EL3 monitor and board
interrupt/timer setup.
It runs at EL1 on one emulated Cortex-A55 with a GICv3 interrupt controller,
a physical generic timer, and 128 MiB RAM. It is not an Orange Pi boot image.

Clang 21, LLD 21, CMake, GNU patch, Python 3.12 or later, and
`qemu-system-aarch64` are required. Configuration stops when the C compiler is
not Clang 21 or later. Python verifies source archive hashes against
[the dependency manifest](../sources.lock.json). Downloaded sources and build
output stay in the user cache.

```sh
set -e
cache="$HOME/.cache/sunstoneos"
freertos=$(python3 tools/fetch-sources.py freertos)
tinyusb=$(python3 tools/fetch-sources.py tinyusb)
cmake -S . -B "$cache/build/qemu-virt" \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-clang.cmake \
  -DFREERTOS_DIR="$freertos" -DTINYUSB_DIR="$tinyusb"
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

The central configuration also prepares storage targets that require TinyUSB.
Configuration therefore stops when `TINYUSB_DIR` is missing, even when only a
kernel or PMIC target will be built; those targets do not link USB code.
Configuration also compares the FreeRTOS and TinyUSB files that the build
compiles with the digests in the dependency manifest, through
`tools/fetch-sources.py <name> --verify <directory>`. A tree that differs is
refused, and editing one of those files later triggers the comparison again.

The toolchain preserves explicitly supplied `CMAKE_C_COMPILER`,
`CMAKE_ASM_COMPILER` and `CMAKE_CXX_COMPILER` paths. Set `SUNSTONE_LLD` to the
`ld.lld` driver when selecting a linker outside `PATH`; CMake propagates that
selection into its compiler checks. Use a fresh build directory when changing
toolchains, because CMake caches compiler and linker flags.

## Reproducible images

The central build gives every target `-ffile-prefix-map` options through the
`sunstone-path-maps` interface library. Each option replaces one absolute root
with a fixed name in `__FILE__` strings and debug information:

| Root | Name |
| --- | --- |
| Repository checkout | `src` |
| Build directory | `build` |
| `FREERTOS_DIR`, `TINYUSB_DIR` | `freertos`, `tinyusb` |
| `SUNSTONE_TCP_DIR`, `SUNSTONE_RSN_DIR`, `SUNSTONE_NETWORK_C_HEADERS` | `tcp`, `rsn`, `c-headers` |
| `MIMUS_DIR`, `NEWLIB_ROOT`, `NEWLIB_SMP_ROOT` | `mimus`, `newlib`, `newlib-smp` |
| `SUNSTONE_SOURCE_CACHE` (default `~/.cache/sunstoneos/sources`) and its parent | `sources`, `cache` |

`configASSERT` passes `__FILE__` to `platform_panic`, so without these maps the
loaded image bytes would contain the checkout and cache paths. With them,
`kernel-check`, `sunstone-a733-board` and `sunstone-a733-storage-maintenance`
built from two checkouts at different paths into two build directories produce
identical ELF files, and their loadable segments contain no `/home` path. The
maps are kept out of target `COMPILE_OPTIONS` because several image identities
hash those options. Standalone projects under `apps/` and `platform/` do not
use the central `CMakeLists.txt` and therefore do not receive the maps.

The board and storage build identities hash their input files and their
effective flags: `CMAKE_C_FLAGS`, `CMAKE_ASM_FLAGS`, the flags of the selected
build type, and the compile options and definitions of the image targets. The
board inputs include the private headers in `network/include`, `drivers/usb`
and `drivers/usb/host`. Text recorded from flags uses the fixed names above, so
moving the checkout or the build directory leaves an identity unchanged.
`checks/build-identity.py` configures two copies of the repository and checks
these properties; pass the usual cache options after `--`:

```sh
python3 checks/build-identity.py --work "$cache/build/identity-check" -- \
  -DFREERTOS_DIR="$freertos" -DTINYUSB_DIR="$tinyusb" ...
```

The board resolves the pinned Cadence PHY source through its lock archive path
under `SUNSTONE_SOURCE_CACHE`, as `tools/fetch-sources.py pcie-sunxi-cadence-combophy-c`
stores it. Caches that only hold the extracted board-kernel tree at
`pcie/kernel/bsp/drivers/phy/` are still accepted; either copy must match the
pinned digest.

Builders that link new units with objects accepted from earlier evidence, such
as `apps/npu-returning/build.py`, compare each reused object's recorded header
dependencies with the current files through `tools/returning_build.py`. They
refuse to link when a header has changed, because the reused object still
holds the earlier structure layouts. The reused objects must then be rebuilt
from current source. Recorded paths under the original worktree are mapped to
the current tree. A recorded header that remains in another git checkout, such
as a different worktree, is also refused, because comparing it with that
checkout would not test the current tree.

The patched FEL recovery tool is built only on the host:

```sh
python3 tools/build-sunxi-fel.py --output "$cache/build/sunxi-fel-rebuild"
```

The script extracts the locked sunxi-tools archive, applies the four patches in
the order recorded in [the source record](sources.md#local-compatibility-patches)
without fuzz, runs `make sunxi-fel` and writes `build.json` with the tool's
SHA-256. It never runs the tool. It exits with an error when that SHA-256
differs from the recorded tool's, after writing `build.json`; pass
`--allow-different` to accept a different build. When the host lacks the libusb-1.0 and libfdt
development packages, pass make variables that point at extracted headers, for
example `LIBUSB_CFLAGS=-I<dir>/usr/include/libusb-1.0 LIBUSB_LIBS=-l:libusb-1.0.so.0`.
With GCC 15.2.0 and the headers from `libusb-1.0-0-dev` `2:1.0.29-2build1` and
`libfdt-dev` `1.7.2-2ubuntu1`, the result is the recorded four-patch tool,
`4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e`. The
model-readback evidence records this tool, and `tools/check-a733-smp.py` and
related checks require it through their `FEL_HASH` constants. The earliest DDR
and AArch64 probes used an earlier build with only the first three patches.

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
The root option `A733_NPU_SUPERVISED=ON` instead selects
`a733-npu-convolution-supervised`, with a separately authenticated session and
retained resources until full board power removal. It still requires the pinned
model, input and reference files and real platform admission.

The [native board application](board-application.md) is available as
`sunstone-a733-board` when `SUNSTONE_TCP_DIR`, `SUNSTONE_RSN_DIR` and
`SUNSTONE_NETWORK_C_HEADERS` identify the authenticated networking sources and
freestanding C headers. It links USB0 output, USB2 host reads and native radio
startup. The default mode requires qualified hardware-watchdog admission.
`SUNSTONE_BOARD_SUPERVISED=ON` selects the separate mode that leaves the watchdog
disabled and retains resources until manual power removal. Both modes require
real memory and power admission before physical execution; see the
[supervised board procedure](board-application.md).

The `sunstone-a733-camera` target connects native OV5640 capture to USB0 output.
The separate `a733-smp-supervised` target runs the all-core observer with the
watchdog disabled and manual recovery. Both are available through the central
build. Their component documents describe distinct physical admission requirements:
[camera](camera-application.md) and [supervised observer](a733-smp-supervised.md).

The `sunstone-a733-gpu-render` target is enabled when `SUNSTONE_GPU_FIRMWARE`
points to the pinned firmware described by the [GPU application](gpu-application.md).
It connects fixed rendering and copied USB output; physical admission remains
separate from compilation.
