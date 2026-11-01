# SunstoneOS current state

SunstoneOS runs FreeRTOS on the Allwinner A733 Orange Pi Zero 3W.
The [accepted objective](goal.md) requires complete usable board hardware,
local applications and performance qualification. No subsystem has complete
qualification. The [hardware matrix](hardware.md) holds the full inventory;
[October history](handoff-history-20261031.md) preserves the previous handoff.

## Board state and recovery

The latest measurements below are from 2026-10-31. Saved USB identities do
not establish present board access. Only the coordinator checks live identity,
permissions, locks and failure records before a device command, under
[AGENTS.md](../AGENTS.md#hardware-access). Both microSD cards must be preserved;
neither board has populated eMMC or UFS. Keep the failed UFS probe disabled.

Evidence paths below are relative to `~/.cache/sunstoneos/evidence/`.
The latest development records are in `dev-runs-1031/`:

- **CPU clocks — board-verified.** Cold DRAM training restores CPU-L and the
  DynamIQ shared unit (DSU) to the boot ROM clock words. The wrapper reports
  `clock_point_result: 0`; CPU-L and DSU PLL words are `0xf8802600`.
  Evidence: `3-3-ddr-clock-plain-3/session.json` under that directory.
- **CPU rails — board-verified retained state; recovery in progress.** DCDC3
  and DCDC5 can remain at 1050 and 1000 mV after failure. Watchdog reset and
  USB replug do not reset the PMIC. boot0 never programs these CPU rails.
  The CPU-return entry requires both at 800 mV. Evidence:
  `3-3-rail-helper-2/rail-helper/kernel.log`, the supplied 2026-10-31 replug
  measurement, and the [boot0 rail inventory](vendor-firmware/boot-rom-and-boot0.md#pmic-rails-over-twi).
  The pinned development helper refuses raised DCDC3. A recovery helper to
  lower both rails is in progress, according to the supplied 2026-10-31 status.
  A successful clock check does not establish rail readiness.
- **DRAM restore — implemented; memory validation failed.** The helper writes
  879 PHY registers with no final survey mismatch. boot0 reports 12,288 MiB,
  but the later PHY message-block readback returns 0 for expected nonzero
  values. The runner records error 7 and falls back to full training.
  Evidence: `3-1-ddr-v5-survey/survey.json` and `session.json` under the
  development directory. This does not establish usable restored memory.
- **Watchdog recovery — board-verified for the recorded stops.**
  `3-3-cpu-return-191044/run/watchdog-report.json` records an EL3 stop,
  a 0.5-second reset interval and retained RTC fault words. DRAM is lost;
  cold training is required before another image uses it.

## Subsystem status

**Implemented** means code exists, without a physical success claim.
**Host-verified only** means the cited software checks pass without board proof.
**Board-verified** applies only to the operation and frozen inputs stated here.
**Not started** means the named function has no implementation.
The component documents identify source files and narrower evidence boundaries.

| Subsystem | Status and demonstrated scope | Reference |
| --- | --- | --- |
| ROM/FEL, DDR, RAM-only loading | **Board-verified:** full training reports 12,288 MiB; clock restoration and two checked RAM pages pass in the development record above. Fast restore remains experimental. | [Boot](boot.md), [DRAM](dram.md), [restore design](designs/dram-training-restore.md) |
| CPU, GIC, timers and SMP | **Board-verified:** eight-core timer/interrupt workload at bin-4 top points; one eight-worker CPU recognition input passes. Sustained load, thermal transitions and fault recovery remain open. | [CPU startup](cpu-start.md), [coherence](cpu-coherence.md), [SMP runtime](smp-runtime.md), [CPU clocks](cpu-clock.md) |
| Memory bus | **Board-verified:** EL3 entry restores MBUS to 600 MHz on both boards, within the recorded memory-pattern checks. | [MBUS restore](designs/mbus-restore.md) |
| Clocks, resets, PMIC and power domains | **Implemented:** shared ownership and operating-point control; CPU rail recovery remains incomplete as stated above. | [Clocks](clock-leases.md), [PMIC](pmic-runtime.md), [power](power-domains.md), [performance modes](performance-modes.md) |
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
| Generic GPU compute queue | **Not started:** the design and prototype headers exist; application execution still uses the explicit legacy GPU integration. | [Queue design](designs/gpu-compute-queue.md), [layering audit](layering-audit.md) |

Earlier board results in this table retain their exact input scope. CPU, GPU
and NPU evidence is indexed in [the preserved board results](handoff-history-20261031.md#qualified-on-the-boards)
and [application evidence](mimus-evidence.md). The CPU/GPU and NPU checkpoint
returns are under `sol-resume-20261028/` and `sol-resume-20261031/`.
The 64 KiB SD result is
`sol-sd-warm-packet-1029/sentinel-physical-result.json`.
Host-only autonomous boot evidence is under `resume-card-os-1026/`.

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
The compatibility GPU path requires `SUNSTONE_A733_OS_GPU_LINEAR=ON` and
`SUNSTONE_A733_OS_LEGACY_MIMUS_GPU=ON`; it has the unresolved gaps below.

## Coordinator launch and capture

These are command forms for the coordinator. Select one port explicitly;
never operate the other board as a side effect. Each tool holds
`tools/usb-access.py:board_lock`, checks the target and records evidence.
A new USB instance needs reconnect permissions before intentional reset.
Stop at the first transport failure; do not retry the failed instance.
The [board skill](../.agents/skills/sunstone-a733-board/SKILL.md) and
[watchdog mechanism](watchdog.md) define the complete access rules.

For a plain full-training wrapper produced by `tools/prepare-ddr.py`:

```sh
py="$HOME/.cache/sunstoneos/venv/bin/python"
fel="$HOME/.cache/sunstoneos/build/sunxi-a733/sunxi-fel"
SUNSTONE_USB_PORT=3-3 "$py" -B tools/run-ddr.py --fel "$fel" \
  --build "$HOME/.cache/sunstoneos/build/docs-current-1031/ddr" \
  --evidence "$HOME/.cache/sunstoneos/evidence/docs-current-1031-ddr"
```

Expected success: `PASS: DDR reports 12288 MiB; both 4 KiB pages match`;
the session must also have `clock_point_result: 0`.
A restore wrapper additionally requires `--record` and `--fallback-build`.
That path uses WDT0 mode `0x41` (four seconds), then one full-training
fallback if restoration fails. Exit 3 identifies successful fallback after a
failed restore.
See the [DRAM runbook](board-runbooks/dram-restore-test-1.md).

Development returning images use [tools/dev-run.py](../tools/dev-run.py).
It still requires the older application host API. The generic CPU-return
module in this checkout lacks that API; select a compatible frozen
`--host-tree`. Adapting preparation to the external port remains open.

The command form is:

```sh
"$py" -B tools/dev-run.py cpu-return --port 3-3 \
  --image "$image" --template "$template" --host-tree "$host_tree" \
  --evidence "$HOME/.cache/sunstoneos/evidence/docs-current-1031-cpu"
```

The coordinator supplies a frozen watchdog-admitting ELF, its session/assets
template and the matching host-source tree. This command requires initialized
DRAM, boot clocks and 800 mV CPU rails. Success prints `cpu-return: returned`
and writes `dev-run.json`, the session and transcript. GPU ladder images use
`gpu-return --stage N --ladder DIR` with their matching host tree/template.
The `npu` kind currently refuses: watchdog-admitting preparation is incomplete.
`--check-only` still reads the board unless `--state FILE` supplies a capture.

The host arms WDT0 mode `0xb1` (16 seconds) before launch. The CPU0 supervisor
restarts it while healthy; an EL3 stop shortens it to 0.5 seconds.
[RTC breadcrumbs](../include/sunstone_crumb.h) retain progress and the first
failure. [tools/a733-crumbs.py](../tools/a733-crumbs.py) decodes them.
Normal return requires this session's `RETURN_ROM` and mode 0.
A watchdog return is a failed run: read breadcrumbs before DRAM, train DRAM
again, and establish the rails independently. PMIC voltage survives reset.
No additional board run is requested for this documentation change.

## Open problems and files

- **CPU restart readiness:** `apps/os/cpu-return-entry.c` requires 800 mV rails;
  `apps/board-bundles-power/cpu-opp-step.c` defines both rail sequences; the
  pinned development helper selects `OS_CPU_RETURN_DCDC3_800` and requires
  DCDC3 already at 800 mV.
  `tools/dev-run.py:rails_note` still claims USB power removal resets both rails;
  the measured retained-rail state above takes precedence. The supplied status
  reports that the recovery helper to lower both rails is in progress.
- **DRAM restore:** `boot/a733/ddr-restore.c`, `boot/a733/ddr-entry.S` and
  `tools/run-ddr.py` must reconcile the successful write survey with the later
  zero PHY message block. Keep full training as the working path.
- **GPU service startup and compute:** `drivers/gpu/pvr_gpu_service.c` does not
  bind the health executed-counter callback or issue the first-job MMUCACHE
  command. `platform/a733/gpu_backend.c` omits the vendor secure writes before
  startup. `platform/a733/gpu_mimus.c` still maps workloads with 2 MiB pages
  and lacks device-cache transfers. The [reference comparison](reference-backsolve-gpu.md)
  distinguishes this checkout from the analysis branch's 256 KiB fix.
- **NPU startup/restart:** `apps/npu-finite/` and `drivers/npu/npu_service.c`
  need current scheduled startup and interrupt evidence beyond constructor
  checkpoints; the earlier full restart did not return.
- **Application split and host fixtures:** the [layering audit](layering-audit.md)
  lists the legacy GPU boundary. `tools/dev-run.py` still imports application
  host modules at their former OS paths. Existing I2S adapter and Bluetooth
  admission fixture failures are recorded under
  `~/.cache/sunstoneos/build/os-refs-1031/` and in the preserved handoff.
- **Remaining qualification:** sustained CPU load, thermal protection, GPU/NPU
  concurrency, autonomous card boot and accessory-dependent operation remain
  required by [the hardware matrix](hardware.md). A passing host model or a
  normal return does not establish device fault recovery.

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
