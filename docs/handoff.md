# SunstoneOS subsystem index

SunstoneOS runs FreeRTOS on the Orange Pi Zero 3W/A733. The
[accepted objective](goal.md) requires complete usable hardware and native
applications. No subsystem has complete physical qualification.
[The hardware inventory](hardware.md) defines required functions independently
of application use. Update current state in the subsystem files below.

<a id="board-state-and-recovery"></a>
<a id="coordinator-launch-and-capture"></a>

The coordinator establishes live identity, permissions, per-port lock and failed
instance state before queue operations. Saved USB descriptors do not prove ROM
execution. Preserve both microSD cards; neither board has eMMC or UFS populated.
[The runner](board-runner.md) owns launch admission and bounded recovery.

## Subsystem status

Implemented means code exists. Board-qualified scope applies only to its named
inputs, operation and cleanup. Blocked functions state the missing prerequisite
in their subsystem file; host evidence establishes no physical qualification.

- [CPU](status/cpu.md): implemented; bounded eight-core workloads pass, while repeatability, sustained load and thermal/fault recovery remain open.
- [Memory/DMA](status/memory-dma.md): implemented; DRAM restoration passes on both boards, while full-map memory and device DMA qualification remain open.
- [Power/thermal](status/power-thermal.md): implemented; finite observations and rail normalization pass, while transitions and protection remain unqualified.
- [GPU](status/gpu.md): implemented; standalone exact sums pass, while the current queue, recognition and recovery remain unqualified.
- [NPU](status/npu.md): implemented; finite startup, inference and two-model runtime qualify, while repeated lifecycle, SDK access and domain restart remain open.
- [USB](status/usb.md): implemented; returning USB0 exchange passes, while host source authority and peripheral qualification remain blocked.
- [Storage](status/storage.md): implemented; scoped PIO/DMA reads pass, while SDR104, writes and autonomous card boot remain unqualified.
- [Radio](status/radio.md): implemented; fitted wiring, top-rate SDIO, returning shutdown and physical interoperability remain blocked.
- [Display/G2D](status/display-g2d.md): implemented; physical output, watchdog return and recovery remain unqualified.
- [Video](status/video.md): implemented; codec/deinterlace output, drain and numerical device behavior remain unqualified.
- [Audio](status/audio.md): implemented; cyclic ordering, serializer drain and physical microphone/headset operation remain unqualified.
- [Camera/ISP](status/camera-isp.md): implemented; no camera is attached, and ongoing native delivery and physical pixels remain open.
- [Buses/RTC](status/buses-rtc.md): partly implemented; PMIC reads, RTC progression and watchdog records pass, while exposed I2C bindings and peripheral qualification remain open.
- [Security](status/security.md): implemented; CE exclusion, private memory, entropy evidence and returning execution remain blocked.
- [PCIe](status/pcie.md): implemented; no endpoint is available for physical qualification.
- [Application](status/application.md): implemented; one recorded CPU recognition input passes, while broader inputs and accelerator/autonomous execution remain open.

<a id="open-branches"></a>

GPU integration boundaries and unmerged functionality are recorded in
[GPU status](status/gpu.md). Build commands belong in [the image guide](os-image.md).
Use [the validation policy](validation.md) and [reference index](reference-index.md).
[The hardware worklog](hardware-worklog-20261031.md) preserves frozen-input and
fault records; [the earlier handoff worklog](handoff-history-20261031.md) retains
older board results. Historical claims do not authorize a new launch.
