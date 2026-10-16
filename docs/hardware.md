# Hardware support and qualification

This inventory covers all onboard hardware and usable SoC blocks. It does not
depend on mimus using a device. The initial list comes from the A733 device tree
and datasheet; physical routing and component population remain unverified.
Expand the list when schematic or device inspection identifies additional blocks.

Status meanings: **planned** has no implementation; **implemented** has code;
**tested** has a functional result under stated conditions; **qualified** meets
its recorded operational and recovery criteria on physical hardware.

Every row below has **no qualification evidence**. Unless stated below, its
implementation is **planned**. Partial results apply only to the stated scope:

- Boot ROM: **tested** for identity, SRAM transfer, a returning AArch32 probe,
  restoration of the modified SRAM bytes, and forced-FEL return from AArch64.
  See [boot evidence](boot.md).
- CPU: **tested** for instruction execution on the boot A55 in AArch32 and
  AArch64 EL3 and nonsecure EL1, including counter reads and SMC return.
  Physical FreeRTOS tasks now pass the [kernel check](a733-kernel.md) on that CPU.
  Other cores remain planned.
- DDR: **tested** for initialization and distinct transfers to two 4 KiB pages.
  Firmware reports 12 GiB LPDDR5; full capacity and stability remain untested.
- Memory mappings, interrupt dispatch, and task scheduling: **tested** on the
  boot CPU for the physical kernel check. Cacheable RAM and non-cacheable result
  storage work for this workload; full memory protection/coherence remains untested.
- Timers and recovery: **tested** for the nonsecure tick and an EL3 secure-timer
  timeout that returns a stalled EL1 to FEL. This does not qualify the independent
  hardware watchdog, every interrupt source, or arbitrary failure recovery.
- CPU clock diagnostics: **tested** for read-only reporting of the boot A55
  clock configuration during recognition. [Application profiling](mimus.md)
  records the register decode and its limits. Rate changes, regulator control,
  and thermal management remain planned.

- USB0 device channel: **tested** at high speed for status, packet-boundary
  echoes, logs, and commanded ROM return. [USB evidence](usb.md) records the
  programmed-I/O implementation, inherited clock state, and remaining modes.

| Hardware | Required functions and application interface | Dependencies and physical checks |
| --- | --- | --- |
| Boot ROM and boot firmware | Recovery, image loading, repeatable boot | Confirm SoC identity, DDR firmware, entry contract, storage preservation |
| CPU cores | AArch64 tasks on all six A55 and two A76 cores; floating-point/vector context | Secondary startup, coherent memory, interrupt routing, affinity |
| DDR and CPU memory management | Usable RAM, permissions, cache policy, allocation | Discover actual RAM; reserve boot firmware; test address boundaries |
| Interrupt controller and timers | Interrupt dispatch, tick, monotonic time, deadlines | GICv3 distributor/redistributors; per-core timers |
| Clocks, resets, regulators, power domains | Dependency-aware enable, disable, rate control, power transitions | Confirm power ICs and safe board operating points |
| Pin control and GPIO | Pin functions, levels, pulls, interrupts | Confirm header routing, voltage domains, shared-pin exclusions |
| DMA and device address translation | Transfers, mapped buffers, cache synchronization, completion, recovery | Address widths, alignment, ownership, IOMMU faults |
| UART | Console and exposed serial channels | Pin routing, baud rates, flow control, receive overrun |
| I2C | Transactions and recovery on exposed and internal buses | Clock dependencies, power devices, sensor control |
| SPI | Transfers and exposed controller modes | Chip selects, DMA, shared pin functions |
| PWM and ADC | Applicable waveform and sampling interfaces | Routed channels, clocking, signal verification |
| SD, eMMC, UFS, SPI storage | Block access and recovery where populated or exposed | Population, boot conflicts, voltage, capacity, power-loss behavior |
| USB controllers and PHYs | Host/device transfers and supported speeds; console and HID classes | Confirm physical ports, role switching, disconnect recovery |
| Type-C and display multiplexing | Role/orientation and applicable display alternate modes | Confirm controller, wiring, shared PHY modes |
| Wi-Fi | Firmware transport, scan, association, supported connection modes, packet interface | Confirm radio variant; SDIO/USB wiring; coexistence and reconnect |
| Bluetooth controller and profiles | Bonds, BLE HID, hands-free audio gateway and voice packets | Firmware, HCI transport, SCO/eSCO routing, codec support |
| Audio controllers | I2S input/output and other routed audio functions; timestamped PCM | No microphone present; confirm header routes and codec requirements |
| CSI receivers and PHYs | Camera capture, modes, synchronization, errors, frame buffers | Connector wiring, lane limits, DMA, sensor clocks |
| OV5640 | Sensor control, supported MIPI modes, exposure and frame delivery | Confirm module wiring and availability; upstream sensor reference |
| Image processing | Pixel conversion, ISP controls, scaling, exposure/white-balance integration | Sensor tuning, buffer formats, documented dimensions |
| Display engine | Scanout, composition, synchronization and supported formats | Buffer sharing, display clocks, bandwidth |
| HDMI | Output modes, detection, applicable audio | Monitor and cable; controller and PHY |
| MIPI DSI | Host operation, panel commands, scanout, backlight integration | Panel undecided; timings, lanes, power sequence, touch separate |
| DisplayPort | Output and applicable Type-C integration | Verify board routing, shared PHY and monitor |
| GPU | Memory management, submissions, graphics/compute runtime, recovery | Exact BVNC, firmware, compiler/runtime, power and synchronization |
| NPU | Model loading, inference, compiler/runtime interface, recovery | Operator/shape support, numerical behavior, buffer ownership |
| Video and image codecs | Supported encode/decode operations through application APIs | Firmware/runtime, formats, dimensions, shared memory bandwidth |
| PCIe | Link, configuration, interrupts, DMA and endpoint interface | Connector and endpoint; link recovery and boot conflicts |
| RTC and watchdog | Time retention, alarms, independent reset/recovery | Backup power population, clock source, reset-cause reporting |
| Thermal and frequency control | Temperature, throttling, supported operating points | Calibration, power coordination, load measurements |
| Crypto, random, identity and secure storage | Applicable hardware operations and trusted firmware interfaces | Confirm silicon features, ownership, immutable provisioning state |
| Always-on processors and mailboxes | Existing firmware services, notifications, sleep/wake coordination | Identify firmware ownership before repurposing any processor |
| Remaining SoC/board functions | Audit LEDs, buttons, infrared, Ethernet, debug and other blocks | Distinguish routed devices, internal functions, and unavailable pins |

Missing peripherals block their physical qualification. Continue implementation
of unrelated devices. Record shared-resource constraints and mutually exclusive configurations
with each functional result. All required rows must be qualified before declaring
complete hardware support.

Hawkeye 64 MP is a separate feasibility investigation. The published A733 ISP
dimensions are below a full 64 MP frame. Lower-resolution modes and any viable
bypass path need independent evidence; camera branding does not establish
compatibility.
