# SunstoneOS current state

SunstoneOS runs FreeRTOS on the Allwinner A733 Orange Pi Zero 3W.
The [accepted objective](goal.md) requires complete usable board hardware,
local applications and performance qualification. No subsystem has complete
qualification. The [hardware matrix](hardware.md) holds the full inventory;
[October history](handoff-history-20261031.md) preserves the previous handoff.

## Board state and recovery

This page describes the code merged through `dc3d1ccd` and recorded board
results from 2026-10-31. Evidence paths are relative to
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
  `apps/os/cpu.c` and `apps/os/cpu-return/cpu.patch`. The fix remains unmerged.
- **GPU firmware diagnostic — native return verified.** The reference startup
  fixes at `fed5813f` started and stopped firmware with zero jobs and the
  reference transcript on 3-1. Evidence:
  `dev-runs-1031/3-1-gpu-return-210941/`. The native return reached `RETURN_ROM`
  with watchdog mode 0. The host then failed on a missing
  `bundle/return-review.json`; completed saved-RAM restoration is not established.
  The earlier compatibility-callback failure remains recorded under
  `dev-runs-1031/3-1-gpu-return-204909/`.
- **Port 3-3 — quarantined by the runner.** A host-side missing
  `bundle/return-review.json` error followed a normal native return with
  `RETURN_ROM` and mode 0. Evidence:
  `dev-runs-1031/3-3-cpu-return-201331/run/session.json`.
  A normal return does not establish completed host RAM restoration.
  The quarantine-scope fix remains unmerged.

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
| NPU | **Board-verified:** earlier interrupt-driven ShuffleNetV2 at 852 MHz. Current constructor/disabled-IRQ checkpoints pass; current scheduled startup and same-instance restart remain unverified. | [NPU](npu.md), [startup](npu-startup.md), [service](npu-service.md) |
| microSD | **Board-verified:** 50 MHz/four-bit/3.3 V PIO, one 4 KiB DMA read and one 64 KiB DMA read match PIO references and return normally. SDR104, writes, sustained throughput and recovery remain unverified. | [Storage](storage.md), [MMC](mmc.md), [SD DMA](designs/sd-dma.md), [updates](storage-update.md) |
| CPU memory protection and DMA | **Implemented:** mappings, reservations, cache maintenance and device address translation. Per-device DMA visibility still needs physical evidence. | [Memory](memory-protection.md), [DMA](dma.md), [device memory](device-memory-capacity.md) |
| USB0, USB host and Type-C | **Implemented:** native device transport, bounded host class profiles and Type-C ownership. Returning FEL transport is demonstrated by the development records; it does not qualify host USB. | [USB platform](usb-platform.md), [host USB](usb2-host.md), [Type-C](typec.md) |
| Pins, GPIO, UART and I2C | **Implemented:** native providers and ownership. Broader exposed-pin and serial qualification remains open. | [Pins](pins.md), [UART](uart.md), [I2C](../drivers/i2c/a733_i2c.c) |
| SPI, PWM and ADC | **Implemented:** native paths, including SPI3 DMA; physical transfer, waveform and sampling qualification remains open. | [SPI](spi.md), [PWM](pwm.md), [ADC](adc-application.md) |
| RTC and thermal | **Implemented:** time/alarm and thermal services. Retained watchdog breadcrumbs are board-verified; alarm, throttling and critical-temperature qualification remain open. | [RTC](rtc-application.md), [thermal](thermal.md), [watchdog](watchdog.md) |
| Wi-Fi and Bluetooth | **Implemented:** radio transport, networking and selected Bluetooth profiles. Fitted wiring and physical interoperability remain blockers. | [Radio](radio-platform.md), [Wi-Fi](radio-wifi.md), [Bluetooth](bluetooth-native.md) |
| I2S and headset audio | **Implemented:** capture and bounded audio paths; no microphone is attached. Physical cyclic-DMA ordering and headset qualification remain open. | [I2S](i2s-stream.md), [headset](bluetooth-hfp.md) |
| Camera, CSI and ISP | **Implemented:** OV5640 modes, receiver and recorded ISP paths. No MIPI camera is attached; native continuous-frame delivery remains incomplete. | [Camera](camera.md), [CSI](csi-receiver.md), [ISP](camera-isp0.md) |
| Display and 2D processing | **Implemented:** HDMI, DSI, DisplayPort, scanout and G2D paths. No MIPI panel is attached; sink output, streaming and recovery remain unverified. | [HDMI](hdmi-native.md), [DSI](dsi-video.md), [DisplayPort](displayport-native.md), [G2D](g2d.md) |
| Video and image processing | **Implemented:** H.264/HEVC decode, H.264/JPEG encode, JPEG decode and deinterlacing paths. Physical numerical output and drain remain unverified. | [H.264](h264-native.md), [HEVC](hevc-native.md), [encoding](h264-encode-native.md), [JPEG](jpeg-application.md), [deinterlacing](deinterlace-native.md) |
| PCIe and NVMe | **Implemented:** root-port, endpoint-test and bounded NVMe paths. No endpoint is available for qualification. | [PCIe](pcie.md), [NVMe](pcie-nvme.md) |
| Crypto, entropy and identity | **Implemented:** native crypto modes, random-source and identity access. Full physical engine and recovery qualification remains open. | [Crypto](crypto-engine.md), [entropy](entropy.md), [identity](board-application.md) |
| Always-on management processor | **Implemented:** remote-writer ownership accounting; native management-core control and sleep/wake qualification remain open. | [Remote writers](remote-writers.md), [SCP reference](vendor-firmware/scp-firmware.md) |
| Autonomous microSD OS boot | **Host-verified only:** packaging, cold-entry and slot/update checks; no physical installation or autonomous OS boot is established. | [Cold entry](cold-entry.md), [resident OS design](designs/autonomous-card-os.md) |
| Generic GPU compute queue | **Host-verified only:** one context, one program, four leases, one job slot, interrupt completion and retained reopen. Physical queue execution and recovery remain unverified. | [Queue design](designs/gpu-compute-queue.md), [layering audit](layering-audit.md) |

Earlier board results in this table retain their exact input scope. CPU, GPU
and NPU evidence is indexed in [the preserved board results](handoff-history-20261031.md#qualified-on-the-boards)
and [application evidence](mimus-evidence.md). The CPU/GPU and NPU checkpoint
returns are under `sol-resume-20261028/` and `sol-resume-20261031/`.
The 64 KiB SD result is
`sol-sd-warm-packet-1029/sentinel-physical-result.json`.
Host-only autonomous boot evidence is under `resume-card-os-1026/`.

## Merged application and failure-record interfaces

Mimus lives in its own repository under `sunstoneos/`.
[apps/mimus/source.json](../apps/mimus/source.json) pins
the integration branch `port/sunstoneos-gpu-integrate`. The application owns recognition,
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

mimus="$HOME/git/mimus-worktrees/os-refs-1031"
"$cfg" "$os" "$build/mimus" -DMIMUS_DIR="$mimus" -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=mimus
cmake --build "$build/mimus" --target sunstone-a733-os -j12
```

`MIMUS_DIR` names the external application checkout with its `sunstoneos/`
port. The [source record](../apps/mimus/source.json) pins the example above.
SunstoneOS owns device services and the [application kit](designs/app-abi.md);
the external application owns recognition, models, manifests and capture tools.
[The image guide](os-image.md) describes output layout and optional profiles.
`SUNSTONE_A733_OS_GPU=ON` selects the generic compute queue. Applications own
their programs and tensor placement; `apps/queue-add` supplies an independent
vector-add consumer. Data pages stop at 256 KiB. The GPU map is built once,
and completion uses interrupts with the retained 4 ms backup poll.

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

This command does not yet demonstrate a passing hello-compute control: the
REQUEST/FOLLOW refusal above remains in the merged code. Current `gpu-return`
is refused because the existing ladder uses the pre-split application layout;
its runner is `tools/dev-run.py` at `6a714bcf`. NPU development preparation
also remains unimplemented. `--check-only` reads the board unless `--state FILE`
supplies a capture; neither mode writes rails.

The host arms WDT0 mode `0xb1` (16 seconds). The CPU0 supervisor restarts it
while healthy; an EL3 stop shortens it to 0.5 seconds.
[tools/a733-crumbs.py](../tools/a733-crumbs.py) decodes progress and the first
failure. Normal return requires this session's `RETURN_ROM` and mode 0.
A watchdog return is a failed run; DRAM initialization and rail checks precede
reuse. No board run is requested for this documentation change.

## Open problems and files

- **Early CPU return:** `apps/os/cpu-return/cpu.patch` refuses completion
  before the clock owner reaches FOLLOW. The hello-compute failure above
  exercises this condition; `apps/os/cpu.c` defines the owner phases.
- **GPU queue qualification:** startup uses the executed-command health count,
  vendor secure writes, initial timer correlation and MMU-cache completion before
  READY. Shared mutable mappings bypass the GPU system cache. Physical queue
  jobs, complete application GPU recognition and recovery remain unverified.
- **NPU and hardware qualification:** current scheduled NPU startup/restart,
  sustained CPU load, thermal protection, accelerator concurrency, autonomous
  card boot and accessories remain outstanding in [the hardware matrix](hardware.md).
- **Known host-check failure:** mimus
  `sunstoneos/checks/os-cpu-return-followup-integration.py` fails with
  `Accepted survey build/source differs`. Its accepted survey manifest pins
  an older `tools/run-launch-survey.py` hash. The source/evidence mismatch
  remains; it is separate from native execution results.

## Open branches

- `work/tool-gates-1031`: simplify host tools and gate device access; under review, unmerged.
- `work/quarantine-scope-1031`: preserve a normally returned instance after a host-side error; fix prepared, unmerged.
- `work/early-return-1031`: permit application completion before CPU clock setup finishes; fix prepared, unmerged.
- `work/gpu-integrate-1031`: combines the reference startup fixes and generic
  compute queue on the current application kit; prepared for host validation
  and coordinator-run physical qualification.
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
