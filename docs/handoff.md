# SunstoneOS current state

SunstoneOS runs FreeRTOS on the Allwinner A733 Orange Pi Zero 3W.
The [accepted objective](goal.md) requires complete usable board hardware,
local applications and performance qualification. No subsystem has complete
qualification. The [hardware matrix](hardware.md) holds the full inventory;
[October history](handoff-history-20261031.md) preserves the previous handoff.

## Host tool admission

Host tools use direct image, session, target and hardware-condition checks.
Queue claims, review records and archived sources do not authorize the changed
runners. DRAM readiness uses a reversible memory comparison; CPU launchers
inspect the live caller stack before uploading their reservations. Uncertain
restoration retains board ownership. Artifact output paths must be unused;
evidence directories can receive unique children. [The tool audit](tool-gates.md) lists each condition and the remaining
changes in files assigned to other agents.

All three baseline images build with Clang 21 and cached lld. Recorded mimus
recognition in cached QEMU matches its reference. The integration gate passes
with and without the external mimus checkout. Returning-watchdog, development
runner, DDR retention, capture conversion, CPU clock and capture-bound host
checks pass, along with the check index and pre-push fixture. Integration
evidence is under `~/.cache/sunstoneos/build/tool-gates-land-1031/`.
No board was accessed for this integration. The audit names protected-owner
changes and earlier unverified component checks.

## Board state and recovery

This page describes the merged startup, host-tool and application interfaces
and recorded board results from 2026-10-31. Evidence paths are relative to
`~/.cache/sunstoneos/evidence/`. Saved USB identities do not establish live
access. Only the coordinator checks identity, permissions, locks and failure
records under [AGENTS.md](../AGENTS.md#hardware-access). Preserve both microSD
cards. Neither board has populated eMMC or UFS; keep the failed UFS probe disabled.

- **DRAM restore — board-verified.** The default SID-selected restore passed
  twice on 3-1, with quick checks of 34.116 and 34.117 ms, 879 PHY writes,
  12,288 MiB reported and `clock_point_result: 0`. Evidence:
  `dram-default-board-1031/3-1-203259/session.json` and
  `3-1-204839/session.json` under that same evidence directory.
  Explicit-record restores also passed on both boards. Temperature coverage,
  full-map memory testing and lower DRAM frequencies remain unqualified.
- **CPU clocks and rails — board-verified for the recorded restoration.**
  DRAM startup restores CPU-L and the DynamIQ shared unit (DSU) to the boot
  clock words. PMIC rails survive watchdog reset and can retain raised values.
  `tools/dev-run.py` now normalizes both CPU rails to 800 mV by default
  before a guarded CPU-return launch and after a validated normal return.
  The helper checks boot clocks before lowering voltage. Evidence:
  `dev-runs-1031/3-3-rail-helper-201322/`; code:
  `apps/board-bundles-power/cpu-opp-step.c` and `tools/dev-run.py`.
- **Hello-compute CPU-return control — failed.** On 3-1 the application
  finished while the CPU clock owner was in REQUEST. The return path requires
  FOLLOW and refused the return. Evidence:
  `dev-runs-1031/3-1-cpu-return-204119/run/watchdog-report.json`; code:
  `apps/os/cpu.c` and `apps/os/cpu-return/cpu.patch`. The merged return path
  handles this owner phase. The corrected hello control returned normally on
  3-1 with seven secondaries off; evidence:
  `dev-runs-1031/3-1-cpu-return-211702/run/session.json`.
- **CPU reliability — failed.** Image `02-reliability` ran on 3-3 and failed
  with the CPU clock driver in RETAINED at `return-cpu.c:159`. Evidence:
  `cpu-reliability-1031/start-1/`. Another worker owns this defect. Three-start
  repeatability and full restoration remain unqualified.
- **GPU firmware diagnostic — failed.** Image `2-firmware` from `ladder-wdt3`
  stopped in the compatibility callback at
  `platform/a733/gpu_backend.c:375` in its frozen build. Evidence:
  `dev-runs-1031/3-1-gpu-return-204909/run/watchdog-report.json`.
  That image uses the unmerged GPU-reference branch. The cause remains under
  investigation; the record does not prove a firmware ABI mismatch.
- **Earlier 3-3 host cleanup — failed.** A host-side missing
  `bundle/return-review.json` error followed a normal native return with
  `RETURN_ROM` and mode 0. Evidence:
  `dev-runs-1031/3-3-cpu-return-201331/run/session.json`.
  A normal return does not establish completed host RAM restoration.
  The merged transport keeps a confirmed normally returned instance usable
  after a host-side failure. That behavior is covered without a board.
  Observed subsequent NPU evidence is under
  `~/.cache/sunstoneos/build/npu-wdt-1031/runs/01-startup-only-28c8e15a2b62-834973ec/`:
  `NPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0` and
  `npu: returned; matched`. Single inference and the two-network runtime also returned with matching
  output. Their evidence is in that same build directory under
  `runs/02-single-inference-0dd327616ce6-834973ec/` and
  `runs/03-runtime-8f51a95699b7-834973ec/`. These three bounded operations
  are board-qualified on 3-3 at 852 MHz and 800 mV.
  Both images include the post-bootstrap settling delay and startup register
  breadcrumbs; the finite runtime requires ISR notification for completion.
  Their build-directory READMEs give hashes, commands and failure stages.
  Independent correctness, hardware-safety and recovery reviews cover the
  NPU branch against `a0b092b5`. Capture failures now quarantine the saved USB
  instance, and unresolved NPU returns retain the board lock. No PMIC rail
  write, voltage target or clock point changed. Review and passing host/build
  evidence are under `~/.cache/sunstoneos/build/npu-wdt-1031/land-review-*/`,
  `land-checks/` and `land-baselines/`.

Both recent 3-1 failures returned through WDT0. Treat DRAM as uninitialized
after that reset and run the DDR tool before the next image uses it.

## Subsystem status

**Implemented** means code exists, without a physical success claim.
**Host-verified only** means the cited software checks pass without board proof.
**Board-verified** applies only to the operation and frozen inputs stated here.
**Not started** means the named function has no implementation.
The component documents identify source files and narrower evidence boundaries.

| Subsystem | Status and demonstrated scope | Reference |
| --- | --- | --- |
| ROM/FEL, DDR, RAM-only loading | **Board-verified:** full training and the saved-record restore both report 12,288 MiB; clock restoration and two checked RAM pages pass in the development records above. | [Boot](boot.md), [DRAM](dram.md), [restore design](designs/dram-training-restore.md) |
| CPU, GIC, timers and SMP | **Board-verified:** eight-core timer/interrupt workload at bin-4 top points; one eight-worker CPU recognition input passes. Sustained load, thermal transitions and fault recovery remain open. | [CPU startup](cpu-start.md), [coherence](cpu-coherence.md), [SMP runtime](smp-runtime.md), [CPU clocks](cpu-clock.md) |
| Memory bus | **Board-verified:** EL3 entry restores MBUS to 600 MHz on both boards, within the recorded memory-pattern checks. | [MBUS restore](designs/mbus-restore.md) |
| Clocks, resets, PMIC and power domains | **Implemented:** shared ownership and operating-point control; default development runs normalize CPU rails at boot clocks. | [Clocks](clock-leases.md), [PMIC](pmic-runtime.md), [power](power-domains.md), [performance modes](performance-modes.md) |
| GPU | **Board-verified:** standalone firmware start/health/stop/restart, eight jobs and two exact-sum shapes at 1008 MHz/960 mV. Full application recognition and failure recovery remain unverified. | [GPU power](gpu-power.md), [backend](gpu-backend.md), [reference comparison](reference-backsolve-gpu.md) |
| NPU | **Board-verified:** earlier interrupt-driven ShuffleNetV2 at 852 MHz. The watchdog-admitting startup, one-inference and two-job/two-model runtime images are board-qualified on 3-3 at 852 MHz and 800 mV, including matching copied outputs, CLOSE, local stop and normal ROM return. Repeated start/stop, domain OFF/restart, SDK access and fault recovery remain unqualified. | [NPU](npu.md), [startup](npu-startup.md), [service](npu-service.md), [finite inference comparison](../apps/npu-finite/INFERENCE-SEQUENCE.md) |
| microSD | **Board-verified:** 50 MHz/four-bit/3.3 V PIO, one 4 KiB DMA read and one 64 KiB DMA read match PIO references and return normally. SDR104, writes, sustained throughput and recovery remain unverified. | [Storage](storage.md), [MMC](mmc.md), [SD DMA](designs/sd-dma.md), [updates](storage-update.md) |
| CPU memory protection and DMA | **Implemented:** mappings, reservations, cache maintenance and device address translation. Per-device DMA visibility still needs physical evidence. | [Memory](memory-protection.md), [DMA](dma.md), [device memory](device-memory-capacity.md) |
| USB0, USB host and Type-C | **Implemented:** native device transport, bounded host class profiles and Type-C ownership. Returning FEL transport is demonstrated by the development records; it does not qualify host USB. | [USB platform](usb-platform.md), [host USB](usb2-host.md), [Type-C](typec.md) |
| Pins, GPIO, UART and I2C | **Partly implemented:** native pin/UART ownership and internal I2C clients. Header TWI0–3 and S_TWI2 lack native resource bindings; exposed-pin and serial qualification remains open. | [Pins](pins.md), [UART](uart.md), [I2C gaps](../drivers/i2c/README.md) |
| SPI, PWM and ADC | **Implemented:** native paths, including SPI3 DMA; physical transfer, waveform and sampling qualification remains open. | [SPI](spi.md), [PWM](pwm.md), [ADC](adc-application.md) |
| RTC and thermal | **Implemented:** time/alarm and thermal services. RTC notifications after completion or failure are refused before callbacks; focused sanitizer and linked checks pass. Retained watchdog breadcrumbs are board-verified; alarm, throttling and critical-temperature qualification remain open. | [RTC](rtc-application.md), [alarm ownership](../apps/rtc/qualification.md), [thermal](thermal.md), [watchdog](watchdog.md) |
| Wi-Fi and Bluetooth | **Implemented:** transport, networking and selected profiles; host-tested deadline checks now cover public SDIO record consumption. Fitted rails/PM2, UHS timing/tuning, returning shutdown and physical interoperability remain open. | [Radio](radio-platform.md), [SDIO reference and guarded reads](radio-sdio-reference.md), [Wi-Fi](radio-wifi.md), [Bluetooth](bluetooth-native.md) |
| I2S and headset audio | **Implemented:** capture and bounded audio paths; no microphone is attached. Physical cyclic-DMA ordering and headset qualification remain open. | [I2S](i2s-stream.md), [headset](bluetooth-hfp.md) |
| Camera, CSI and ISP | **Implemented:** OV5640 modes, receiver and recorded ISP paths. No MIPI camera is attached; native continuous-frame delivery remains incomplete. | [Camera](camera.md), [CSI](csi-receiver.md), [ISP](camera-isp0.md) |
| Display and 2D processing | **Implemented:** HDMI, DSI, DisplayPort, scanout and G2D paths. CPU command-span checks and unchanged command pages pass host checks. DisplayPort uses four lanes at 5.4 Gb/s and requires matching analog/SERDES authority. Scanout slots remain immutable. Native images need a reviewed watchdog return path; no MIPI panel is attached. Sink output, streaming and recovery remain unverified. | [HDMI](hdmi-native.md), [DSI](dsi-video.md), [DisplayPort](displayport-native.md), [G2D](g2d.md) |
| Video and image processing | **Implemented:** H.264/HEVC decode, H.264/JPEG encode, JPEG decode and deinterlacing paths. HEVC initialization preserves retained ownership; its offline CPU pixel comparator passes host checks with CPU-generated copies. Physical numerical output and drain remain unverified. | [H.264](h264-native.md), [HEVC](hevc-native.md), [encoding](h264-encode-native.md), [JPEG](jpeg-application.md), [deinterlacing](deinterlace-native.md) |
| PCIe and NVMe | **Implemented:** root-port, endpoint-test and bounded NVMe paths. No endpoint is available for qualification. | [PCIe](pcie.md), [NVMe](pcie-nvme.md) |
| Crypto, entropy and identity | **Implemented:** native crypto modes, MD5/SM3 and CPU-staging erasure; host checks pass. Prior CE exclusion, secure access, IRQ/drain, raw entropy and physical qualification remain open. The existing crypto image parks and is ineligible for returning runs. | [Security inventory and evidence](security-engines.md), [entropy](entropy.md), [identity](board-application.md) |
| Always-on management processor | **Implemented:** remote-writer ownership accounting; native management-core control and sleep/wake qualification remain open. | [Remote writers](remote-writers.md), [SCP reference](vendor-firmware/scp-firmware.md) |
| Autonomous microSD OS boot | **Host-verified only:** packaging, cold-entry and slot/update checks; no physical installation or autonomous OS boot is established. | [Cold entry](cold-entry.md), [resident OS design](designs/autonomous-card-os.md) |
| Generic GPU compute queue | **Not started in the merged tree:** design and prototype headers only. The implementation remains on an unmerged branch. | [Queue design](designs/gpu-compute-queue.md), [layering audit](layering-audit.md) |

Earlier board results in this table retain their exact input scope. CPU, GPU
and NPU evidence is indexed in [the preserved board results](handoff-history-20261031.md#qualified-on-the-boards)
and [application evidence](mimus-evidence.md). The CPU/GPU and NPU checkpoint
returns are under `sol-resume-20261028/` and `sol-resume-20261031/`.
The 64 KiB SD result is
`sol-sd-warm-packet-1029/sentinel-physical-result.json`.
Host-only autonomous boot evidence is under `resume-card-os-1026/`.

## Host validation and pending device runs

The combined host checks pass under
`~/.cache/sunstoneos/build/integrate-streams-1031/`. OS-only and hello-compute
build with empty `MIMUS_DIR`; the third baseline uses the external mimus
checkout at `~/git/mimus-worktrees/small-fixes-1031`. Selected integration
builds cover memory, DMA, cyclic I2S, radio dispatch and radio/headset service
composition. Source-ownership, shared-helper and progress checks pass.

Focused checks pass for complete memory-map admission, DMA/IOMMU ownership,
raw PMIC observation, cold-session refusal, SDIO deadlines, HEVC state and
pixels, RTC alarm lifetime and MD5/SM3 staging/upload ownership. Device effects
remain modeled. The check index verifies 750 checks; pre-push fixtures reject
25 blocked cases and accept 12 permitted cases. Test cleanup moves artifacts
into the trash directory.

Independent review approved the integration and two fixture/parser corrections.
The dashboard reads the current handoff's bulleted actions. Memory preparation
checks refusal of an existing artifact directory before using a fresh one.
The failed setup logs and successful resumed cases remain in the evidence
directory. No board was accessed; these results add no physical qualification.

The display and CPU integration checks pass under
`~/.cache/sunstoneos/build/integrate-streams-1031/followup/`. All three baseline
images and the native G2D, HDMI framebuffer, DisplayPort, DSI and CPU reliability
images build. Command-span sanitizer checks preserve 132 command pages;
pinned G2D/DE comparisons and 18 DisplayPort rate-admission cases pass.
The seven CPU scheduler-model cases and console-present/absent sanitizer cases
pass. CPU-return preparation uses saved state, passes its offline checks and
records no launch. Independent review approved the shared changes.

Memory admission checks every tested block before writes. Cyclic DMA receive
staging excludes every retained allocation and metadata range. Mapping,
sanitizer and FreeRTOS DMA service/cyclic checks pass, as do the three baseline
builds. The [memory admission record](designs/memtest-map-admission.md) and
[DMA ownership record](designs/dma-rx-copy-ownership.md) define this evidence.
The frozen memory artifact is under
`~/.cache/sunstoneos/build/memory-dma-1031/images/memtest/`. Its legacy monitor
needs WDT0, breadcrumbs and a matching runner before launch. DMA0 needs physical
access authority; DMA1 remains source-blocked. Existing DRAM restore evidence
remains valid. Full-map testing after training and restore, temperature coverage
and per-master DMA/IOMMU behavior remain unqualified.

The PMIC decoder preserves DCDC8/DCDC9 codes 88–127 as raw, undecoded
values because pinned U-Boot and Linux references disagree. The six-rail
observer and source/provider checks pass on the host. Its physical launch
requires watchdog and breadcrumb integration. The
[PMIC comparison](designs/pmic-reference-reconciliation.md) identifies artifacts
and missing fitted-PMIC, wiring and voltage evidence. The
[thermal and management proposal](designs/thermal-management-qualification.md)
records processor lifecycle dependencies and E902 resource conflicts.

Stage 2 rejects stale board/CPU-grant epochs and malformed typed records
before watchdog arming. OS admission stamps its private grant copy. Under
`~/.cache/sunstoneos/build/sd-boot-1031/`, 36 sanitizer/linked session cases,
40 generic host-record cases, 19 slot-tool cases and connected backup, write,
readback and restoration checks pass. The three baseline builds pass. The
monitor occupies 32,368 bytes and leaves 400 bytes before its fixed report.
The [stage-2 record](../boot/a733/stage2/README.md) describes the checks.
SDR104 and sustained reads need physical tests. Physical writes, installation,
autonomous recognition and reset recovery remain unqualified. Installation
requires real card extents, a verified full backup, write approval and a
qualified runner.

The [bounded CPU workload](../apps/cpu-runtime-qualification/RELIABILITY.md)
passes native builds, seven FreeRTOS scheduler-model cases and offline
return-image preparation. Frozen image hashes and coordinator command forms
are in `~/.cache/sunstoneos/build/cpu-reliability-1031/README.md`. The first
reliability launch failed as recorded above. The earlier longer-recognition
nonreturn has no recovered fault location; the corrected hello control has
only its own recorded scope. Further trials remain held after the failure.

## Merged application and failure-record interfaces

Mimus lives in its own repository under `sunstoneos/`.
[apps/mimus/source.json](../apps/mimus/source.json) pins
`port/sunstoneos-split` at `c0872ac1`. The application owns recognition,
models, manifests, capture wrappers and recognition checks. The merged
reference cleanup supplies explicit OS admission inputs and the shared USB
emulator; OS-only and hello-compute builds require no mimus sources.
[The application kit](designs/app-abi.md) defines the interface.

First-failure records are implemented in `apps/board/main.c:board_fail_at`.
`apps/os/application.c:ss_fail` records the application failure group before
closing the session. [RTC breadcrumbs](watchdog.md#rtc-words-retain-progress-and-the-first-failure)
preserve the site, caller and detail through watchdog reset.
`checks/harness.py:skip_crumbs` steps over both progress SVC `0x5c0d` and
first-failure SVC `0x5c0e` in function-level instruction models.

## Build OS and application images

Run from the repository root. The cached configuration wrapper selects
Clang 21, cached `ld.lld`, FreeRTOS, TinyUSB, networking and newlib paths.
Use a fresh directory per configuration. It supplies a default external
application path, so explicitly clear `MIMUS_DIR` for independent OS builds.
The selections and target names are defined in
[the CMake composition](../cmake/a733-os.cmake).

```sh
os="$PWD"
build="$HOME/.cache/sunstoneos/build/docs-current-1031"
cfg="$HOME/.cache/sunstoneos/bin/cfg.sh"

"$cfg" "$os" "$build/os" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=
cmake --build "$build/os" --target sunstone-a733-os -j12

"$cfg" "$os" "$build/hello" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=hello-compute
cmake --build "$build/hello" --target sunstone-a733-os-hello -j12

mimus="$HOME/git/mimus-worktrees/small-fixes-1031"
"$cfg" "$os" "$build/mimus" -DMIMUS_DIR="$mimus" -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=mimus
cmake --build "$build/mimus" --target sunstone-a733-os -j12
```

`MIMUS_DIR` names the external application checkout with its `sunstoneos/`
port. The [source record](../apps/mimus/source.json) pins the example above.
SunstoneOS owns device services and the [application kit](designs/app-abi.md);
the external application owns recognition, models, manifests and capture tools.
[The image guide](os-image.md) describes output layout and optional profiles.
The compatibility GPU path requires `SUNSTONE_A733_OS_GPU_LINEAR=ON` and
`SUNSTONE_A733_OS_LEGACY_MIMUS_GPU=ON`; it has the unresolved gaps below.

## Coordinator launch and capture

The coordinator selects one port and holds `tools/usb-access.py:board_lock`
through the existing tools. Intentional reset needs reconnect permissions.
Stop requests at the first transport failure and do not retry that instance.
The [board skill](../.agents/skills/sunstone-a733-board/SKILL.md) and
[watchdog record](watchdog.md) define the access and reset rules.

The DDR runner defaults to restoring the record named by the board's SID:

```sh
py="$HOME/.cache/sunstoneos/venv/bin/python3"
fel="$HOME/.cache/sunstoneos/build/sunxi-a733/sunxi-fel"
SUNSTONE_USB_PORT=3-1 "$py" -B tools/run-ddr.py --fel "$fel" \
  --evidence "$HOME/.cache/sunstoneos/evidence/docs-current-1031-ddr"
```

Success prints `PASS: DRAM restored; quick check … ms; both 4 KiB pages match`
and exits 0. A missing or unusable record selects full training. A failed
restore resets through WDT0 and trains fully once; successful fallback exits
3 and prints that DRAM is trained. Use the printed DDR evidence directory
for later runners. `--full-training` bypasses restore; `--build DIR` selects
an explicit build. [The record runbook](board-runbooks/dram-restore-records.md)
covers setup, SID names, fallback and limitations.

[tools/dev-run.py](../tools/dev-run.py) now prepares manifest-based CPU-return
applications directly. Build the hello-compute control as described in
[the image guide](os-image.md#build-an-os-image), then use this command form:

```sh
"$py" -B tools/dev-run.py cpu-return --port 3-1 \
  --image "$build/control-hello/sunstone-a733-os-hello.elf" \
  --evidence "$HOME/.cache/sunstoneos/evidence/docs-current-1031-cpu"
```

The default hello decoder supplies its input. Other applications provide
`--input FILE` in manifest order and, when needed, `--decoder FILE`.
`--records FILE` supplies reviewed board/thermal/CPU-clock session records.
The default rail helper runs before the state capture and after a validated
normal return with boot clocks. After watchdog reset it waits until DRAM is
initialized and the next launch normalizes rails. `--no-normalize-rails` and
`--no-restore-rails` explicitly disable the respective default.

The corrected hello-compute control returned normally in the recorded 3-1
run above. Repeated startup and broader reliability remain unqualified.
Current `gpu-return` is refused because the existing ladder uses the pre-split application layout;
its runner is `tools/dev-run.py` at `6a714bcf`. NPU development supports startup,
single inference, runtime, local restart, model switching, SDK access and
domain OFF/restart through its native session template and exact image marker.
`--check-only` reads the board unless `--state FILE`
supplies a capture; neither mode writes rails.

The next NPU diagnostics are host-verified and await coordinator runs. Their
source and tools are frozen at `593ce329` beneath
`~/.cache/sunstoneos/build/npu-wdt-1031/next-host-593ce329/`. Image directories
are under `images/` in that build directory:

| Run order | Image | Expected operation |
| --- | --- | --- |
| 1 | `local-restart` | Two matching jobs across two local start/stop lifetimes; four IRQ completions. |
| 2 | `model-switch` | Match conv12, conv8, then conv12 again; three jobs and four IRQ completions. |
| 3 | `sdk-access` | Match both models through the public application API; three IRQ completions and owner-authorized quiescence. |
| 4 | `power-cycle` | Prove domain policy/status OFF 0/0 then ON 8/8 between matching jobs; four IRQ completions. |

Each directory's README gives full hashes, exact commands, breadcrumb order
and stage failures. `images/next-images.json` binds the artifacts and frozen
host archive. Continue only after the preceding run returns normally with
matching output; power-cycle runs last. The SDK diagnostic uses the finite
task privilege. Its host evidence covers the memory-only public API and
owner checks, and does not qualify full OS EL0 mappings.

Independent correctness, hardware-safety and recovery panels approve the
branch through `593ce329` against `a0b092b5`. Review found and corrected
capture quarantine, unresolved ownership release, SDK failure-state races,
SDK close ownership, report capacity and transient-provider handling.
The NPU checks, SDK ABI and interleaving cases, power transition fault models,
all four artifact checks and the three baseline builds pass. Evidence is in
`next-checks/`, `next-baselines/`, `next-sdk-api/`, `next-abi/`,
`next-frozen-preparation/` and `power/root/` in the same build directory.

The host arms WDT0 mode `0xb1` (16 seconds). The CPU0 supervisor restarts it
while healthy; an EL3 stop shortens it to 0.5 seconds.
[tools/a733-crumbs.py](../tools/a733-crumbs.py) decodes progress and the first
failure. Normal return requires this session's `RETURN_ROM` and mode 0.
A watchdog return is a failed run; DRAM initialization and rail checks precede
reuse.

## Open problems and files

- **CPU reliability:** the first reliability launch failed with the CPU clock
  driver in RETAINED at `return-cpu.c:159`. Another worker owns the defect;
  repeated startup, sustained work and full restoration remain unqualified.
- **GPU service:** the latest compatibility stop belongs to the frozen
  unmerged diagnostic. The merged service still has the health-binding,
  vendor-setup, MMUCACHE, page-size and SLC gaps in
  [the reference comparison](reference-backsolve-gpu.md).
- **NPU and hardware qualification:** repeated NPU start/stop, SDK access and domain OFF/restart,
  sustained CPU load, thermal protection, accelerator concurrency, autonomous
  card boot and accessories remain outstanding in [the hardware matrix](hardware.md).
- **Known host-check failure:** mimus
  `sunstoneos/checks/os-cpu-return-followup-integration.py` fails with
  `Accepted survey build/source differs`. Its accepted survey manifest pins
  an older `tools/run-launch-survey.py` hash. The source/evidence mismatch
  remains; it is separate from native execution results.

## Open branches

- `work/board-runner-1031`: returning-image and runner changes; unmerged.
- `work/gpu-integrate-1031`: GPU integration; unmerged.
- `work/gpu-ref-fixes-1031`: align the GPU service with the reference sequence; diagnosis and fixes in progress, unmerged.
- `work/gpu-queue-1031`, with mimus `port/sunstoneos-gpu-queue`: supply a generic GPU compute queue and application port; unmerged.

## Review and validation

Follow [AGENTS.md's review gate](../AGENTS.md#review-gate-and-validation):
a diagnostic needs its author's check and run; a feature needs one reviewer
and a host test. Voltage, power-domain and storage-write changes need a short
design with measurable budgets before coding and the same three reviewers
for design and code. Unexplained measured regressions block integration.

Use [the validation policy](validation.md) with flags selecting the changed
branches. Reuse valid evidence after documentation edits and commits.
A board result consists of its log, image hash and Git revision.
Find source pins and cached manuals through [the reference index](reference-index.md).
