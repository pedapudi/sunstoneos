# Hardware support inventory

This inventory covers usable onboard hardware independently of application
requirements. The [subsystem files](handoff.md) hold present status, evidence
and remaining work. Update those files when support changes.

The [A733 device tree and datasheet](reference-index.md) define the SoC
inventory. The [V1.2 schematic](board-wiring.md) defines routed functions.
Neither current board has populated eMMC or UFS. Both microSD cards must retain
their contents under [the storage rules](goal.md). Confirm board revision and
other optional population before assigning physical qualification.

**No row is fully qualified across its required functions.**
A board-qualified scope in a subsystem file applies only to the stated image,
inputs, operation and cleanup. It does not qualify the entire inventory row.
An implementation or a host model alone establishes no physical operation.

| Hardware | Required functions and application interface | Dependencies and physical checks |
| --- | --- | --- |
| Boot ROM and boot firmware | Recovery, image loading, repeatable boot | Confirm SoC identity, DDR firmware, entry contract, storage preservation; [State](status/storage.md) |
| CPU cores | AArch64 tasks on all six A55 and two A76 cores; floating-point/vector context | Secondary startup, coherent memory, interrupt routing, affinity; [State](status/cpu.md) |
| DDR and CPU memory management | Usable RAM, permissions, cache policy, allocation | Discover actual RAM; reserve boot firmware; test address boundaries; [State](status/memory-dma.md) |
| Interrupt controller and timers | Interrupt dispatch, tick, monotonic time, deadlines | GICv3 distributor/redistributors; per-core timers; [State](status/cpu.md) |
| Clocks, resets, regulators, power domains | Dependency-aware enable, disable, rate control, power transitions | Confirm power ICs and safe board operating points; [State](status/power-thermal.md) |
| Pin control and GPIO | Pin functions, levels, pulls, interrupts | Confirm header routing, voltage domains, shared-pin exclusions; [State](status/buses-rtc.md#pins) |
| DMA and device address translation | Transfers, mapped buffers, cache synchronization, completion, recovery | Address widths and transfers above 4 GiB, alignment, ownership, IOMMU faults; [State](status/memory-dma.md#dma) |
| UART | Console and exposed serial channels | Pin routing, baud rates, flow control, receive overrun; [State](status/buses-rtc.md#uart) |
| I2C | Transactions and recovery on exposed and internal buses | Clock dependencies, power devices, sensor control; [State](status/buses-rtc.md#i2c) |
| SPI | Transfers and exposed controller modes | Chip selects, DMA, shared pin functions; [State](status/buses-rtc.md#spi) |
| PWM and ADC | Applicable waveform and sampling interfaces | Routed channels, clocking, signal verification; [State](status/buses-rtc.md#pwm-and-adc) |
| SD, eMMC, UFS, SPI storage | Block access and recovery where populated or exposed; eMMC and UFS media absent on both current boards | Preserve the existing contents of both microSD cards and follow the write targets in goal.md section 4; qualify SD and applicable SPI storage, boot conflicts, voltage, capacity and power-loss behavior; [State](status/storage.md) |
| USB controllers and PHYs | Host/device transfers and supported speeds; console and HID classes | Confirm physical ports, role switching, disconnect recovery; [State](status/usb.md) |
| Type-C and display multiplexing | Role/orientation and applicable display alternate modes | Confirm controller, wiring, shared PHY modes; [State](status/usb.md#host-and-type-c) |
| Wi-Fi | Firmware transport, scan, association, supported connection modes, packet interface | Confirm radio variant; SDIO/USB wiring; coexistence and reconnect; [State](status/radio.md) |
| Bluetooth controller and profiles | Bonds, BLE HID, hands-free audio gateway and voice packets | Firmware, HCI transport, SCO/eSCO routing, codec support; [State](status/radio.md) |
| Audio controllers | I2S input/output and other routed audio functions; timestamped PCM | No microphone present; schematic routes I2S0 to the header and I2S1 PCM to Bluetooth; qualify both; [State](status/audio.md) |
| CSI receivers and PHYs | Camera capture, modes, synchronization, errors, frame buffers | Schematic routes four lanes each to CAM1/MCSIA and CAM2/MCSIB; qualify sensors, DMA, and clocks; [State](status/camera-isp.md) |
| OV5640 | Sensor control, supported MIPI modes, exposure and frame delivery | No module connected; confirm wiring when available; upstream sensor reference; [State](status/camera-isp.md) |
| Image processing | Pixel conversion, ISP controls, scaling, exposure/white-balance integration | Sensor tuning, buffer formats, documented dimensions; [State](status/camera-isp.md) |
| Display engine | Scanout, composition, synchronization and supported formats | Buffer sharing, display clocks, bandwidth; [State](status/display-g2d.md) |
| HDMI | Output modes, detection, applicable audio | Monitor and cable; controller and PHY; [State](status/display-g2d.md) |
| MIPI DSI | Host operation, panel commands, scanout, backlight integration | Schematic routes four DSI1 lanes to LCD; no display connected, panel undecided; timings, power sequence, touch separate; [State](status/display-g2d.md) |
| DisplayPort | DP output through documented V1.2 TYPEC24 lanes/AUX and Type-C alternate mode | Qualify actual revision, negotiation/orientation, HPD handling, COMB0 exclusion, link training and sink output; [State](status/display-g2d.md) |
| G2D | Native 2D fill/copy, scaling, blending, rotation and format conversion | Shared DE_SYS power/clocks, RCQ, IOMMU master 10, memory ownership and physical output correctness; [State](status/display-g2d.md) |
| GPU | Memory management, submissions, graphics/compute runtime, recovery | Exact BVNC, firmware, compiler/runtime, power and synchronization; [State](status/gpu.md) |
| NPU | Model loading, inference, compiler/runtime interface, recovery | Operator/shape support, numerical behavior, buffer ownership; [State](status/npu.md) |
| Video and image codecs | Supported encode/decode operations through application APIs | Firmware/runtime, formats, dimensions, shared memory bandwidth; [State](status/video.md) |
| DI301 deinterlacer | Offline weave, motion-adaptive processing, TNR, film detection and supported formats | Shared DE_SYS power/clocks, guarded master-9 mappings, DMA visibility and physical field processing; [State](status/video.md) |
| PCIe | Link, configuration, interrupts, DMA and endpoint interface | Connector and endpoint; link recovery and boot conflicts; [State](status/pcie.md) |
| RTC and watchdog | Time retention, alarms, independent reset/recovery | Backup power population, clock source, reset-cause reporting; [State](status/buses-rtc.md#rtc-and-watchdog) |
| Thermal and frequency control | Temperature, throttling, supported operating points | Calibration, power coordination, load measurements; [State](status/power-thermal.md) |
| Crypto, random, identity and secure storage | Applicable hardware operations and trusted firmware interfaces | Confirm silicon features, ownership, immutable provisioning state; [State](status/security.md) |
| Always-on processors and mailboxes | E902 management-core ownership, existing firmware/mailbox services and sleep/wake coordination | Resolve firmware/shared-resource ownership and cold-start remote-writer exclusion; no physical control or drain qualification; [State](status/power-thermal.md#management-processor) |
| Remaining SoC/board functions | Audit LEDs, buttons, infrared, Ethernet, debug and other blocks | Distinguish routed devices, internal functions, and unavailable pins; [State](status/buses-rtc.md#remaining-functions) |

Missing peripherals block their physical qualification. Shared-resource
constraints and mutually exclusive configurations belong with each subsystem.
All required rows need physical qualification before hardware support is complete.
The [hardware evidence worklog](hardware-worklog-20261031.md) retains dated
measurements and frozen-input details.
