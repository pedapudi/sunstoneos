# Hardware bring-up and performance sequencing

SunstoneOS must support every function in the [hardware matrix](hardware.md),
independently of mimus. The order below follows shared dependencies. Work on
sources, firmware, interfaces, and host checks can continue when physical work
is blocked. Missing accessories remain explicit qualification blockers.

## Recovery precedes further peripheral probes

The UFS probe stalled without returning through the CPU secure timer. Before
another new peripheral-access sequence, qualify an independent watchdog and
a reset destination that cannot execute unknown storage software.

The board has BOOT and GND test pads beside mini-HDMI. The documented recovery
procedure uses a stable connection between those pads, attached with power
disconnected. It does not require a boot pushbutton. The user's ordinary USB
power cycle reached FEL with a microSD card inserted, but watchdog recovery
remains unverified. Preserve the card and other storage contents.

The [physical recovery procedure](board-wiring.md) identifies the pads and the
prepared input checker. Confirm the connection, check the input, then conduct
one bounded SRAM watchdog-expiry trial with retained results and automatic
USB rediscovery. The passed configuration/restoration trial does not need repeating.
Integrate the qualified recovery path into subsequent hardware probes.

## Shared platform services enable device drivers

After recovery, establish clocks, resets, regulators, power domains, pin
ownership, and interrupt delivery. Extend RAM accounting, protection, cache
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

The GPU now has native [firmware decoding and staging](gpu.md) and
[MIPS memory/boot preparation](gpu-mips.md) with host evidence.
[Command transport](gpu-queues.md), [initialization records](gpu-fw-state.md),
[GPU workload tables](gpu-vm.md), and [startup sequencing](gpu-start.md)
also have host reference evidence. [Compatibility, health, and event service](gpu-control.md)
have host integration evidence. [Shutdown](gpu-stop.md) has host handshake and
idle/reset evidence; physical interrupt delivery and isolation remain outstanding.
The [FreeRTOS lifecycle service](gpu-service.md) integrates these components and
passes ownership, reopen, event, and failure checks in the two-core emulator.
The [native memory adapter](gpu-memory.md), [power controller](gpu-power.md), and
[interrupt delivery](gpu-interrupts.md) now have focused host/emulator evidence.
Physical bindings, shared-resource management, and device isolation remain required.
GPU firmware boot, graphics/compute runtime, and physical identity remain
unqualified. The NPU has implemented components with host/emulator evidence,
but physical numerical execution remains outstanding. Neither accelerator is
fully functional in SunstoneOS.

Once each hardware function works, qualify operational modes, concurrent
workloads, cancellation, errors, recovery, and applicable power transitions.
Use the [validation policy](validation.md) to select checks for changed behavior
and reuse evidence whose inputs remain valid.

CPU, GPU, and NPU optimization follows functional coverage. The established
mimus compute target is at most 0.25 times audio duration: a ten-second
recording permits at most 2.5 seconds of recognition computation. Preserve
the model and recorded-reference behavior. Measure capture-to-result latency
separately from compute time.

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
