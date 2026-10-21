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

## Build patches apply exactly

Many images compile sources that the build generates by applying a reviewed
patch to a repository or upstream file, often to the output of an earlier
patch. `apply_build_patch` in [cmake/patch.cmake](../cmake/patch.cmake) runs
GNU patch with `--fuzz=0`, so every hunk must match its recorded context lines
exactly. GNU patch would otherwise accept up to two mismatched context lines,
and a change to a base file could move a hunk beside code that it was never
reviewed against. Configuration stops with the patch report when a hunk does
not match. A hunk whose exact context sits a few lines away from its recorded
line is still accepted. No reject file is written.

Four patches in the boot-CPU PWM and crypto applications were written against
an earlier `apps/board/main.c` and `apps/storage-read-single-core/runtime.c`
and still need fuzz. `cmake/patch.cmake` lists each with the SHA-256 of the
output that was checked to place every hunk at its intended location. Such a
patch is applied with fuzz only while its output keeps that digest. Any further
change to its base or to the patch stops configuration; regenerate the patch
against its current base and remove its entry.

The central `CMakeLists.txt` uses this function for every profile it includes.
The standalone projects under `apps/` and `platform/` still define their own
copy, which accepts fuzz. A host check exercises exact, offset and fuzzy
application, the pinned outputs and the board-mode rule described below:

```sh
PATH="$HOME/.cache/sunstoneos/host/usr/lib/llvm-21/bin:$PATH" \
  python3 checks/audit-build-safety-configure.py --output "$cache/build/patch-check"
```

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

Applications derived from the supervised board image, such as
`sunstone-a733-thermal`, `sunstone-a733-crypto` and `sunstone-a733-h264`, are
defined through `sunstone_board_application` in
[cmake/a733-board-application.cmake](../cmake/a733-board-application.cmake).
The function keeps the board's sources, compile options, include directories
and definitions, and replaces the board's USB0 owner, admission, boot and
contract magics with the application's. Each recipe lists only what differs:
its sources, its boot and platform patches, extra definitions, the linker
script and its identity inputs. The function's `sunstone_build_identity`, also
used by `sunstone-a733-pcie-test`, hashes the input files, the compiler and
`ld.lld` binaries, the configuration flags, and the compile definitions,
options, include directories and link options of the image and its USB2 host
objects. A change to `CMAKE_C_FLAGS` therefore changes the build ID. Beside
the image, the application's build directory holds `build-id.txt`,
`identity-inputs.txt`, `identity-options.txt` with the hashed option text, and,
after each link, `manifest.json` with the build ID and the SHA-256 of the final
ELF. `build-id.c` is rewritten only when the ID changes, so reconfiguring does
not relink an unchanged image.

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

The returning images link a finite client against one returning platform:
the monitor, boot, I2C and FreeRTOS objects of the `a733-power-probe` target
and the counter, lock, clock, rail, PMIC, pin, GPU-resource, task and queue
units of the PMIC provider probe, which use the probe's FreeRTOS configuration
in `checks/pmic-provider-probe`. `ReturningPlatform` in
`tools/returning_build.py` supplies that platform to these builders:

| Builder | Images |
| --- | --- |
| `apps/rtc-returning/build.py` | read-only RTC client and its check-only policy ELF |
| `apps/rtc-source-returning/build.py` | RTC source initialization, or alarm with `--operation alarm` |
| `apps/thermal-returning/build.py` | thermal client, or ADC client with `--device adc` |
| `apps/npu-returning/build.py` | NPU supply client |
| `apps/usb-returning/build.py` | finite USB client |

By default the platform is the accepted objects of the
`pmic-provider-probe-20261018` evidence, with the header check described
above. Recorded compile flags are mapped from the checkout named in that
evidence's source snapshot to the current repository. With
`--platform-build <dir>`, where `<dir>` is a central build directory configured
from this checkout with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, the builder builds `a733-power-probe` there, links its
platform objects and compiles the provider units from the current tree with
that target's flags, the provider probe's configuration and `-Werror`. Such an
image depends on no earlier evidence, and its `input-identities.json` records
the platform objects and the provider units' header closure. The USB builder
also links accepted TinyUSB objects that exist only in evidence, so it accepts
only the evidence platform.

`apps/npu-finite/build.py` rebuilds the single-core NPU application from its
recorded configuration and replaces its monitor and platform exit;
`--interrupt`, which `apps/npu-interrupt/build.py` passes, selects the
interrupt-driven variant.

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

The application profiles built from the supervised board, such as
`SUNSTONE_A733_PWM`, `SUNSTONE_A733_STORAGE_READ` and `SUNSTONE_A733_THERMAL`,
require `SUNSTONE_BOARD_SUPERVISED=ON` on the command line. Configuration stops
before the board target is created when one of them is enabled without it,
and the profile does not change the mode in the cache. Disabling the profile
therefore leaves the requested board mode unchanged. A build directory
configured before this rule may still hold the mode that an earlier profile
set; pass the mode explicitly or use a fresh directory.

The `sunstone-a733-camera` target connects native OV5640 capture to USB0 output.
The separate `a733-smp-supervised` target runs the all-core observer with the
watchdog disabled and manual recovery. Both are available through the central
build. Their component documents describe distinct physical admission requirements:
[camera](camera-application.md) and [supervised observer](a733-smp-supervised.md).

The `sunstone-a733-gpu-render` target is enabled when `SUNSTONE_GPU_FIRMWARE`
points to the pinned firmware described by the [GPU application](gpu-application.md).
It connects fixed rendering and copied USB output; physical admission remains
separate from compilation.
