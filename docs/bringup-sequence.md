# Hardware bring-up and performance sequencing

SunstoneOS must support every function in the [hardware matrix](hardware.md),
independently of mimus. The order below follows shared dependencies. Work on
sources, firmware, interfaces, and host checks can continue when physical work
is blocked. Missing accessories remain explicit qualification blockers.

## Recovery gates persistent writes and the UFS probe

Physical work starts with supervised RAM-only execution and reviewed return
procedures. Stop device communication after the first transport failure,
preserve its evidence, and require explicit recovery before the board is used
again. Intentional watchdog expiry and USB power automation are deferred; they
are not prerequisites for unrelated device work.

The UFS probe stalled without returning through the CPU secure timer. It stays
disabled (`SUNSTONE_EXPERIMENTAL_UFS=OFF`) until that failure and its recovery
are resolved.

The board has BOOT and GND test pads beside mini-HDMI. The documented manual
recovery procedure uses a stable connection between those pads, attached with
power disconnected. It does not require a boot pushbutton. The procedure has
not been qualified on the attached boards. The user's ordinary USB power cycle reached FEL with
a microSD card inserted. The [physical recovery procedure](board-wiring.md)
identifies the pads and the prepared input checker.

Persistent writes require established recovery and a preserved copy of the
existing contents. Preserve both microSD cards and other storage contents.

## Shared platform services enable device drivers

Establish clocks, resets, regulators, power domains, pin ownership, and
interrupt delivery. Extend RAM accounting, protection, cache
coherence, and startup to all CPU cores. Qualify DMA and device address
translation before peripheral drivers depend on them.

Complete the USB development channel's cold initialization and interrupts.
Identify populated storage, obtain read-only access, and back up existing
contents before persistent writes. Then establish reproducible storage boot,
image loading, recovery, and updates.

## Device work follows its dependencies

These groups can progress independently once their shared services work.
The [handoff](handoff.md) identifies the next concrete implementation tasks;
the [hardware matrix](hardware.md) remains the authoritative coverage record.

| Group | Implementation and first physical result |
| --- | --- |
| Basic interfaces | GPIO, UART, I2C, SPI, PWM, ADC, RTC, timers, and routed board controls; exercise transfers, interrupts, and error recovery |
| Storage and USB | SD, populated eMMC/UFS, routed SPI storage, USB host/device roles, PHYs, and Type-C control; verify transfers and reconnects |
| Radio and audio | Wi-Fi firmware and SDIO transport, association and packets; Bluetooth firmware, HCI, profiles and reconnects; I2S PCM and headset voice transport |
| Camera and display | CSI, OV5640 control and frame capture, ISP, display engine, HDMI, DSI and applicable DisplayPort routes; verify actual frames and scanout |
| Accelerators | GPU firmware, memory management, submissions, graphics/compute runtime and recovery; NPU admission, resources, numerical inference and recovery; video/image codecs |
| Remaining platform functions | PCIe, thermal management, frequency/power transitions, crypto, random generation, identity, mailboxes and remaining usable SoC functions |

Investigate difficult GPU, radio, codec, and NPU dependencies before their
physical bring-up slots. GPU implementation must not wait for an optimized
NPU or a native NPU compiler. External NPU tools remain acceptable until
complete hardware support; [issue 12](https://github.com/pedapudi/sunstoneos/issues/12)
tracks their lower-priority native replacement.

Recorded audio supports mimus development now. I2S microphone and Bluetooth
headset input remain required. An OV5640 module, selected DSI panel, and
applicable PCIe/display accessories are needed for their physical checks.
Controller implementation can proceed before those accessories are available.

## Functional coverage precedes performance optimization

The [integrated component status](handoff.md#integrated-component-status)
lists the implemented GPU, NPU and other device paths with their host, emulator
or model evidence. GPU firmware boot, graphics and compute runtime, physical
identity, cold initialization and device isolation remain unqualified. NPU
physical numerical execution remains outstanding. Neither accelerator is fully
functional in SunstoneOS. Reproducible boot images, recovery and update
procedures, and physical qualification also remain goal requirements.

Once each hardware function works, qualify operational modes, concurrent
workloads, cancellation, errors, recovery, and applicable power transitions.
Use the [validation policy](validation.md) to select checks for changed behavior
and reuse evidence whose inputs remain valid.

CPU, GPU, and NPU optimization follows functional coverage. The accepted
mimus target is complete warm recognition within 0.05 times audio duration:
a ten-second recording permits 500 ms from available PCM to formatted text.
This includes feature extraction, inference, decoding, punctuation, transfers
and synchronization. The 0.01-times target is a stretch objective for bounded
investigation. Preserve the model and established accuracy requirements.
Report startup time and live end-of-speech latency separately.

The [physical USB application result](mimus-usb.md#physical-integration-evidence)
records 19.588 seconds of compute for approximately 8.87 seconds of audio,
with 20.259 seconds from request through result retrieval. That boot-CPU result
does not meet the target. The configured clock was 494 MHz; independent
frequency measurement and physical multicore execution remain outstanding.
Sustained-performance qualification remains outstanding.

Independent GPU and NPU throughput targets have not been set. Establish
workload-specific acceptance criteria after their capabilities and runtime
paths are functional. Compare equivalent work at recorded clocks, precision,
and correctness requirements, including transfer costs and thermal behavior.
Every measurement batch needs a concrete question and sample count or stopping
rule. No recurring benchmarks are required during hardware bring-up.
