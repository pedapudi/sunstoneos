# Reference sources

Pin source revisions before using them in a build. Source files document reference
implementations; physical support on this board requires qualification evidence.

| Source | Inspected revision or document | Purpose |
| --- | --- | --- |
| [Board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f) | `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` | Board wiring descriptions and A733 driver implementations |
| [Board bootloader](https://github.com/orangepi-xunlong/u-boot-orangepi/tree/b791be842935b27268ae3d00e943a9075495f30a) | `b791be842935b27268ae3d00e943a9075495f30a` | Boot package and application handoff |
| [FreeRTOS kernel](https://github.com/FreeRTOS/FreeRTOS-Kernel/tree/97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2) | `97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2` | AArch64 interrupt port and scheduler; Cortex-R82 non-MPU port adapted for the separate SMP emulator target |
| [Newlib source release](https://sourceware.org/pub/newlib/newlib-4.5.0.20241231.tar.gz) | `4.5.0.20241231`; archive hash in `sources.lock.json` | Separate SMP library build with dynamic reentrancy, explicit syscall state, and retargetable locks; see [runtime evidence](smp-runtime.md) |
| [FEL tools](https://github.com/linux-sunxi/sunxi-tools/tree/b92f4ed3115a520e3ec406697d268239e3f29488) | `b92f4ed3115a520e3ec406697d268239e3f29488` | Recovery protocol and tested local A733 patches |
| [TinyUSB](https://github.com/hathach/tinyusb/tree/d668b0abb7219b9ce324fa88d46604bb9e869973) | `d668b0abb7219b9ce324fa88d46604bb9e869973` | Device stack and sunxi MUSB driver adapted for A733 |
| [A733 datasheet](https://dl.radxa.com/cubie/a7a/docs/hw/datasheet/A733_Datasheet_V0.93.pdf) | Version 0.93 | Hardware inventory and documented limits |
| [PowerVR](https://docs.mesa3d.org/drivers/powervr.html) | Documentation inspected 2026-10-15 | Candidate GPU runtime; exact BVNC match pending |
| [OV5640 driver](https://github.com/torvalds/linux/blob/master/drivers/media/i2c/ov5640.c) | Revision pin pending | Sensor programming reference |

The inspected upstream Linux Allwinner device-tree directory at
`2f2f7eeef8b0a1a9b9ca998231419873ccae8d4e` contains no A733 entry.
Track upstream support per component instead of assuming the board is supported.

The mimus engine has a portable C++ backend and deterministic host comparisons.
Its board adapters require changes for thread identity, allocation, storage,
scheduling, and hardware access. Use its source and current project instructions
when implementing that port.

Download identities and archive hashes are in [sources.lock.json](../sources.lock.json).
The [DMA reference record](dma.md) pins the inspected board-kernel driver hash
and distinguishes data addressing from unresolved wider descriptor addressing.
The [native DMA memory record](dma-memory.md) identifies the cache-maintenance
and translation references used by the EL1 adapter.
The [NPU reference record](npu.md) pins the VIPLite driver, A733 SDK, and
TIM-VX parser. It records binary-analysis provenance and format-inspection evidence.
The [address relocation record](nbg-relocation.md) pins the SDK library and
identifies the isolated instructions used for encoding comparisons.
The [complete loader trace](nbg-loader-trace.md) identifies synthetic HAL inputs,
source resolution, allocation padding, and the intercepted submission boundary.
The [native container record](nbg-model.md) uses the same pinned format evidence
and reuses saved SDK output for a comparison driven by C-decoded records.
The [NPU memory record](npu-mmu.md) pins table geometry and directory setup
references for the native page-table builder. The [controller record](npu-controller.md)
identifies the adapted register sequences and their modeled ownership behavior.
The [native NPU CPU record](npu-native.md) identifies the generic-timer reference
and shared AArch64 translation/cache implementation.
The [NPU service](npu-service.md) applies a separate startup-privilege patch to
the pinned FreeRTOS SMP port; the downloaded source retains its license notices.
The board-specific boot0 binary comes from the board build repository at
`a95129bf30ea11886cc35f08ebb3e69695a26656`. Its header specifies a 245,760-byte
image loaded at `0x47000`. Selected power/clock and DDR functions have executed
through the [RAM wrapper](boot.md); the normal storage boot path has not.

The forced-FEL ROM entry was inspected in the [A733 ROM dump](https://github.com/smaeul/sunxi-blobs/blob/42aabf4f2c93158a0cd88a000f9af56ce024882a/sun60iw2p1/fixs-brom/blob.hex).
Five instruction words matched on the attached board before the warm-reset probe.
The [Cortex-A55 technical reference manual](https://documentation-service.arm.com/static/5e7e1405b471823cb9de57ae)
describes the reset vector and execution-state controls.

## Local compatibility patches

- The FreeRTOS patch adds the missing `memset` declaration and uses 64-bit
  operands for AArch64 system-register reads. It applies only to the generated
  build copy of `port.c`; the downloaded source retains its MIT license notice.
- The A733 SRAM configuration is the `soc_info.c` change from
  [proposal 223](https://github.com/linux-sunxi/sunxi-tools/pull/223), head
  `5d996736a41f975245bd6a22d8d4c3cf044fb38a`.
- FEL status validation comes from [proposal 259](https://github.com/linux-sunxi/sunxi-tools/pull/259),
  head `a90b1b712800068c55e0519368b6a405e0073e9e`.
- The execute-argument patch fixes a locally observed CLI parser error: `exe`
  consumes its command and address, leaving the following command intact.

The FEL patches retain the upstream tool's GPL licensing. The SRAM probe used
the first two FEL patches; the argument-consumption fix was recorded afterward.
DDR and AArch64 probes used all three patches. The DDR wrapper supplies its own
SRAM layout instead of loading the full boot0 through the original SPL path.

The monitor's interrupt grouping follows the [Trusted Firmware-A interrupt
controller API](https://trustedfirmware-a.readthedocs.io/en/latest/components/platform-interrupt-controller-API.html):
Group 0 serves EL3 and nonsecure Group 1 serves EL1. The [Arm GIC architecture
specification](https://documentation-service.arm.com/static/67eaa4d098aa3c3b6eea7351)
defines the nonsecure priority view and system-register interface. The monitor
implementation is local code; its physical evidence is in [a733-kernel.md](a733-kernel.md).

The TinyUSB patches retain the downloaded source and its MIT notices. They
apply A733 register/clock definitions, AArch64 register widths, task polling,
control-transfer completion handling, and zero-length IN submission. The
[USB implementation and evidence](usb.md) distinguish tested behavior from
remaining controller modes. Clock and PHY setup follows the pinned board
bootloader’s `clock_sun60iw2.c`, `board_sun60iw2.c`, and USB register definitions.

The optional [C++ runtime](runtime.md) uses the static libraries and headers from
the [AArch64 bare-metal distribution](https://developer.arm.com/-/media/Files/downloads/gnu/14.3.rel1/binrel/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf.tar.xz),
release `14.3.rel1`. Its downloaded SHA-256 matches the publisher’s companion
checksum and is pinned in the dependency manifest. Project sources still use
Clang 21. The distribution retains its library license notices and GCC runtime
library exception; downloaded files remain outside tracked source.

The FEL bounded-receive patch follows the status-validation patch. It caps each
USB receive at 512 KiB while preserving short-response boundaries. The
[application evidence](mimus.md) records the full-speed timeout diagnosis,
recovery without reset, and successful model readback with this patch.

The mimus application uses the local port revision in
[apps/mimus/source.json](../apps/mimus/source.json). Model provenance and body
preservation are recorded by `tools/prepare-mimus.py` in each input manifest.
