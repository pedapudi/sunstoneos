# Boot-CPU-only board images

The supervised board image and the applications derived from it (thermal, RTC,
PWM and crypto) each have a second image that runs on the boot CPU alone. The
EL3 monitor in that image confirms that the seven secondary CPUs are off, seals
the uploaded sessions, and starts FreeRTOS on CPU 0 with the same providers and
application tasks as the SMP image. The function
`sunstone_single_core_application()` in
`cmake/a733-application-single-core.cmake` builds all five images; each
`cmake/a733-<name>-single-core.cmake` recipe is one call to it.

## Image composition

The image `sunstone-a733-<name>-single-core` starts from the source list of the
SMP image `sunstone-a733-<name>`. It drops the cached SMP startup, the
secondary-CPU start code and the SMP image's generated boot, platform and build
ID. In their place it links these boot-CPU sources from
`apps/storage-read-single-core`:

- `start.S`, the EL3 entry;
- `monitor.c`, which admits the contract, checks that the secondary CPUs are
  off, prepares the sessions and cached tables, and configures the interrupt
  controller;
- `runtime.c`, which checks the admitted state at EL1 and starts the scheduler;
- `platform.patch`, applied to `platform/a733/platform.c`, which adopts the
  cached page tables and dispatches application interrupts.

The image compiles every source with the SMP image's options, include
directories and definitions, plus these definitions:

- `SUNSTONE_CACHED_SINGLE_CORE` selects the cached-table startup in the shared
  sources. Every boot-CPU image defines it except the storage-read and eMMC
  images, which keep the storage platform's own tables. The GPU and NPU
  single-core recipes also define it.
- `SUNSTONE_<NAME>_SINGLE_CORE`, for example `SUNSTONE_PWM_SINGLE_CORE`,
  selects the application's branches in the shared monitor, runtime and
  platform: where its runtime contract is retained, which session it prepares,
  which interrupt it routes and dispatches, and its geometry record.
- `SUNSTONE_SD_SINGLE_CORE` makes the board session validator also accept USB0
  takeover mode 2, the ROM USB handoff, for workload 2. The thermal, RTC, PWM
  and crypto images define it. The board image patches its validator instead.
- `SUNSTONE_SINGLE_CORE_PLATFORM_LOG` names the linker symbol of the platform
  log, `__<name>_platform_log`.

`apps/board-single-core` holds the `boot.h`, `cached.h` and `FreeRTOSConfig.h`
of all five images. Its `FreeRTOSConfig.h` keeps every task on the boot CPU.

## Per-application differences

Each image still differs from its SMP image in a few board sources. The build
patches these into its build directory with the exact patch helper in
`cmake/patch.cmake`, which refuses a hunk whose context lines do not match:

- `session.patch` moves the sealed board session into the image's retained
  section, so that it survives the EL1 clearing of `.bss`.
- `main.patch` masks interrupts after task creation and again before the
  scheduler starts. The board and PWM images also mask them after USB2 host
  construction.
- The board image patches `identity.c` and `admission.c`, the thermal and RTC
  images patch their `snapshot.c`, and the RTC image also patches
  `initialize.c`.
- The PWM image patches the board validator into `board-admission.c`, which
  `apps/pwm/admission.c` includes. It admits only USB0 takeover mode 2.

The PWM and crypto applications select their single-core state in their own
sources: `apps/pwm/snapshot.c`, `apps/crypto/snapshot.c` and
`apps/pwm/admission.c` test `SUNSTONE_PWM_SINGLE_CORE` or
`SUNSTONE_CRYPTO_SINGLE_CORE`.

## Linker script and retained state

Every image links with `apps/board-single-core/link.ld`, written into the
build directory with `board` and `BOARD` replaced by the application name. The
monitor clears the section `.<name>_retained` between `__<name>_retained_start`
and `__<name>_retained_end` before it seals the sessions. The host checks read
those symbols. The board, PWM and crypto images also link the 2 MiB uncached
and device pools.

`apps/dsi-scanout` patches `monitor.c`, `runtime.c` and the board linker
script. A change to the lines that its patches use as context stops the DSI
configuration, so its patches must be regenerated with such a change.

## Build identity

`sunstone_build_identity()` names each image. The identity hashes the
sources, headers, patches, the board linker script, the FreeRTOS port, both
recipe files, the compiler and linker binaries and the target options. The
manifest records the target, the execution profile
`<name>-single-core-supervised-v1`, the workload, the build ID and the SHA-256
of the final ELF. Host preparation in `apps/<name>/prepare.py` accepts the
image only with that profile and its `<name>_single_core_geometry` record.

## Building

The standalone projects `apps/<name>-single-core` build each image. Configure
them like the central build, with an absolute `CMAKE_TOOLCHAIN_FILE`, and build
target `sunstone-a733-<name>-single-core`. The central build also builds the
PWM image with `SUNSTONE_A733_PWM=ON` and the crypto image with
`SUNSTONE_A733_CRYPTO=ON`.

## Checks

The startup checks `checks/<name>-single-core.py` run the linked image in a
modeled machine and exercise admission, retention, interrupt routing and the
refusal cases. `checks/single-core-variants-equivalence.py` compares an image
built from two source trees. It requires identical object code for every
unit, and identical linked sections when the link order is unchanged. Use it
when a recipe or shared-source change should not change generated code.
