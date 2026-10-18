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
| [Board schematic and position drawing](https://drive.google.com/drive/folders/199S7SkbqVDjMpPT7tkcbBX6kmbfnz94r) | V1.2; PDF hashes in `sources.lock.json` | Physical recovery pads, connector routing, and component references |
| [Board user manual](https://drive.google.com/file/d/1R1MC9erKTztd87GvJ_UFE132x6KgPATe/view) | V1.0; PDF hash in `sources.lock.json` | Recovery-pad photograph, power-on procedure, and advertised interfaces |
| [PowerVR](https://docs.mesa3d.org/drivers/powervr.html) | Documentation inspected 2026-10-16 | Candidate GPU runtime; board software selects BVNC 36.56.104.183, listed under active development; physical identity pending |
| [OV5640 driver](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/media/i2c/ov5640.c) | `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` | Sensor programming reference; native control and complete register-image comparison in [camera.md](camera.md) |

The inspected upstream Linux Allwinner device-tree directory at
`2f2f7eeef8b0a1a9b9ca998231419873ccae8d4e` contains no A733 entry.
Track upstream support per component instead of assuming the board is supported.

The mimus engine has a portable C++ backend and deterministic host comparisons.
Its board adapters require changes for thread identity, allocation, storage,
scheduling, and hardware access. Use its source and current project instructions
when implementing that port.

Download identities and archive hashes are in [sources.lock.json](../sources.lock.json).
The [board wiring record](board-wiring.md) identifies the relevant schematic
sheets and manual pages. Documented wiring remains separate from physical
population and functional qualification of the attached board.
The [DMA reference record](dma.md) pins the inspected board-kernel driver hash
and distinguishes data addressing from unresolved wider descriptor addressing.
The [native DMA memory record](dma-memory.md) identifies the cache-maintenance
and translation references used by the EL1 adapter.
The [crypto-engine record](crypto-engine.md) identifies the pinned A733 CE v5
kernel and U-Boot references. Native SHA-256 and AES execution uses repository
source; the [crypto application](crypto-application.md) loads no opaque CE
firmware or Linux crypto library. Its fixed AES reference comes from NIST
SP 800-38A, as recorded in [the cipher evidence](crypto-cipher.md).
The [H.264 application](h264-native.md) adapts open-source Linux Cedrus control
and uses pinned H6-CedarC source references. Its source manifest records licenses
and hashes. A733 CedarC libraries provide instruction references; the native
image does not load those Linux libraries. FFmpeg supplies the host pixel oracle.
The [native RTC application](rtc-application.md) adapts the pinned board kernel's
GPL-2.0-or-later SoC RTC calendar and alarm operations, with crystal-control
sequences from the pinned RTC clock driver. Its
[source manifest](../apps/rtc/sources.json) records the exact driver, clock and
board-description inputs. Repository source executes these operations under
FreeRTOS; no RTC firmware blob or Linux runtime is loaded. The component document
records conflicting manual descriptions and the unresolved external-RTC board variant.
The [PCIe implementation](pcie.md) uses the pinned board kernel and U-Boot as
register and sequence references. The [native application](pcie-application.md)
executes repository controller and PHY source under FreeRTOS; it loads neither
a PCIe firmware blob nor either reference operating environment.
The [SPI application](spi-application.md) uses repository controller and platform
source under FreeRTOS. Board-kernel and U-Boot sources provide register and routing
references; the AXP8191 table in the pinned U-Boot supplies the BLDO1 voltage
decoder fields. The native image loads no SPI firmware blob. Its component
record identifies the source revision, schematic route and electrical limits.
The [NPU reference record](npu.md) pins the VIPLite driver, A733 SDK, and
TIM-VX parser. It records binary-analysis provenance and format-inspection evidence.
The [address relocation record](nbg-relocation.md) pins the SDK library and
identifies the isolated instructions used for encoding comparisons.
The [complete loader trace](nbg-loader-trace.md) identifies synthetic HAL inputs,
source resolution, allocation padding, and the intercepted submission boundary.
The [native container record](nbg-model.md) uses the same pinned format evidence
and reuses saved SDK output for a comparison driven by C-decoded records.
The [native assembly record](nbg-assembly.md) reuses that capture for allocation
padding, address resolution, and command construction.
The [checksum record](nbg-checksum.md) pins two published models and compares
the native verifier with the isolated SDK CRC primitive. The
[descriptor investigation](npu.md#descriptor-field-compatibility-is-a-research-result)
pins Mesa's candidate neural-command fields and records compatibility limits.
The [ring validation record](nbg-rings.md) pins the SDK HAL library and restricts
its instruction comparison to the isolated transpose descriptor encoder.
The [compiler record](npu-compiler.md) pins the downloaded host image and records
the generated convolution, original license locations, and numerical comparisons.
The [directory investigation](nbg-directory.md) pins the inspector's debug
information and compiler CRC instructions used to establish the emitted layout.
The [convolution loader trace](nbg-conv-loader.md) reuses the pinned A733 loader
and synthetic HAL to capture preparation of that numerical fixture.
The [GPU firmware record](gpu.md) identifies the board driver's configured BVNC,
matching published MIPS firmware, source hashes, native staging, and remaining
runtime requirements. The firmware and its license are pinned in the source
manifest. Driver-source adaptations retain their MIT notice.
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
The [physical ROM capture](watchdog.md) now identifies a different revision with
matching forced-entry words and a relocated FEL dispatcher. Board-specific
recovery analysis uses that captured image; the published dump remains a
separate reference.
The [Cortex-A55 technical reference manual](https://documentation-service.arm.com/static/5e7e1405b471823cb9de57ae)
describes the reset vector and execution-state controls.

## Binary dependencies and replacement order

Document actual dependency use before considering replacement. Record origin,
version and hash, execution location, source availability, compatibility limits
and license status. The tracking issues assign boot dependencies to
[issue 1](https://github.com/pedapudi/sunstoneos/issues/1), platform sources and
reference binaries to [issue 2](https://github.com/pedapudi/sunstoneos/issues/2),
radio dependencies to [issue 5](https://github.com/pedapudi/sunstoneos/issues/5),
GPU dependencies to [issue 8](https://github.com/pedapudi/sunstoneos/issues/8),
and NPU tools to [issue 9](https://github.com/pedapudi/sunstoneos/issues/9).
[Issue 11](https://github.com/pedapudi/sunstoneos/issues/11) tracks the final-image
dependency inventory and unresolved documentation requirements.

The native OS uses source-built FreeRTOS, TinyUSB, Newlib, FreeRTOS+TCP and
selected wpa_supplicant security code where the application requires them.
Linux drivers provide implementation references; Linux is not a board runtime.
The native Bluetooth application reuses Zephyr's open-source BR/EDR secure
pairing and RAM link-key policy through the existing FreeRTOS HCI owner.
[The source manifest](../apps/bluetooth/security-source.json) pins commit
`64e98eb5278d790f3b3e5279ca8b39b45097bbc0`, individual file hashes and the
Apache-2.0 license. The build compiles the adapted policy into the native image;
it does not run a Zephyr kernel. Controller firmware remains a separate opaque
dependency. [Bluetooth integration](bluetooth-native.md) records the exercised
incoming-headset path and outstanding physical qualification.

The JPEG diagnostic uses FFmpeg 8.0.1 (`7:8.0.1-3ubuntu2`) as an open-source
host decoder reference. It is not linked into SunstoneOS. The
[numerical reference record](jpeg-application.md#exact-nonconstant-diagnostic)
identifies the selected decoder, input/output hashes, executable and shared
libraries. The native driver uses its own source implementation.

The following opaque dependencies have distinct execution roles:

| Dependency | Current use | Documentation and remaining gap |
| --- | --- | --- |
| CedarC runtime libraries | Executable instructions provide JPEG register, table and layout references; the libraries are not loaded by FreeRTOS | [Codec sources](video-codecs.md) pin the published package and library hashes. Source-availability and redistribution assessments remain outstanding; cached notices are preserved. |
| Board boot0 | Selected power, clock and DDR-training routines execute during RAM bring-up | [Boot record](boot.md) and source lock identify the exact binary, reviewed entry points and physical observations. A separate license assessment is not recorded. |
| PowerVR firmware | Native GPU loader stages code for the internal MIPS processor | [GPU record](gpu.md) pins hardware selection, firmware ABI, hash and accompanying license. Physical firmware execution remains unqualified. |
| AIC8800D80 firmware | Native SDIO loader uploads Wi-Fi code and Bluetooth data, patches and configuration | [Firmware record](radio-firmware.md) specifies revision selection, lengths, hashes and protocol. [Radio provenance](radio.md) records unresolved redistribution terms. Physical radio operation remains unqualified. |
| ACUITY/Vivante tools | Host compiler and simulator generate and inspect NPU model artifacts | [Compiler record](npu-compiler.md) and its lock file retain package identities, versions and license locations. These tools do not run inside FreeRTOS. |
| NPU SDK binaries | Host analysis and isolated instruction comparisons establish model-loading behavior | [NPU record](npu.md) identifies the inspected libraries and executable. Native decoding and assembly replace the inspected loading behavior for the supported profiles; general compatibility remains incomplete. |

GPU and radio firmware are supplied as immutable memory spans; these loaders
do not require a runtime filesystem. Published artifact availability does not
establish hardware compatibility or redistribution rights.

Reuse suitable open-source drivers and libraries, adapting their OS interfaces
to FreeRTOS as needed. Native execution does not require rewriting their device
logic. An open-source host driver and the firmware it loads are separate
dependencies; assess their source availability independently.

After complete hardware functionality and performance tuning, investigate source
replacements for remaining opaque dependencies. Retain the working binaries as
comparison references. Replace source-available components only for a demonstrated
incompatibility or unmet requirement. Verify each replacement against the
supported hardware operations.
The native NPU toolkit remains tracked in issue 12. Replacing embedded GPU or
radio firmware is separate from implementing the A733 host drivers; complete
replacement is not established by the current reverse-engineering evidence.

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
