# Dependencies for complete board support

This graph covers every area in the [hardware inventory](hardware.md). Arrows run
from a prerequisite to the function that needs it. They describe implementation
relationships, not a mandatory serial work schedule. Independent drivers can be
developed and checked concurrently.

The table records structure only. The dashboard obtains evidence and qualification
status from the hardware inventory. Evidence on an inventory area applies only to
its documented scope; it does not establish that every prerequisite or mode works.
Additional nodes identify shared functionality without assigning it a second status.

## Foundations and mode-specific requirements

Boot begins with inherited power and basic clock configuration. Basic clocks and
pin control enable I2C; I2C then enables PMIC regulator control. This distinction
avoids a circular dependency between complete power management and its own bus.
A working single-core runtime supports independent peripheral integration. Full
multicore scheduling remains a separate completion requirement.

DMA controllers move data between memory and peripherals. The system IOMMU maps
addresses for particular device masters. They are separate prerequisites. GPU and
NPU have private memory translation; neither requires the system IOMMU. Current
USB0, UART, SPI and storage paths can use programmed I/O.

Dependencies cover the complete listed function. Earlier partial modes can work
with fewer prerequisites: HDMI colorbars precede framebuffer scanout, DSI commands
precede scanout, and CSI receiver checks precede sensor capture. Bluetooth firmware
loads through SDIO before UART operation; Bluetooth does not require Wi-Fi association.
Bluetooth wired PCM uses I2S1, while voice transported through HCI can proceed independently.

Qualification conditions describe physical equipment, electrical evidence or recovery
that a mode needs. They are not claims that accessories are currently available.
No MIPI camera, DSI panel or microphone is connected. Intentional watchdog expiry
requires a qualified reset destination; it does not block unrelated supervised
RAM-only tests with a reviewed return and manual recovery plan.

## Machine-readable dependency table

IDs matching hardware inventory names inherit that area's evidence. Other IDs are
structural nodes. `Requires` contains comma-separated IDs, or `-`. `Record` names
the component document supporting the relationship. The dashboard checks coverage,
references and cycles before displaying the graph. The graph is an integration
plan based on those records; it is not a new claim of hardware qualification.

| ID | Function | Requires | Scope | Qualification conditions | Record |
| --- | --- | --- | --- | --- | --- |
| boot-rom-and-boot-firmware | Boot ROM and boot firmware | - | Loading, entry state, DDR firmware provenance, repeatable boot and recovery. | Preserve installed storage; establish the return and recovery path for each launch. | hardware.md |
| ddr-and-cpu-memory-management | DDR and CPU memory management | boot-rom-and-boot-firmware | RAM capacity, reserved regions, mappings, permissions, cache coherence and allocation. | Verify actual RAM geometry and owned ranges before native or DMA access. | hardware.md |
| interrupt-controller-and-timers | Interrupt controller and timers | boot-rom-and-boot-firmware | GICv3 interrupt dispatch, per-core timers, ticks and bounded deadlines. | Verify interrupt ownership, masking, completion and restoration. | hardware.md |
| basic-clock-reset | Basic clocks and resets | boot-rom-and-boot-firmware | Bootstrap clock/reset access using inherited supply settings; complete power policy comes later. | Confirm parent rates and safe register access before enabling a device. | clock-leases.md |
| pin-control-and-gpio | Pin control and GPIO | basic-clock-reset | Pin multiplexing, voltage domains, pulls, levels, interrupts and shared-pin exclusions. | Confirm board routing and electrical compatibility of attached signals. | pins.md |
| single-core-runtime | Single-core FreeRTOS runtime | ddr-and-cpu-memory-management, interrupt-controller-and-timers | Task entry, exceptions, scheduling, synchronization and bounded monitor return on one CPU. | Establish native entry and cleanup for each image; existing evidence is scope-specific. | hardware.md |
| cpu-cores | CPU cores | single-core-runtime, basic-clock-reset | All six A55 and two A76 cores, coherent shared memory, per-core interrupts, task scheduling and FP/vector state. | Demonstrate concurrent native tasks and safe secondary-core shutdown; diagnostic barriers alone are partial evidence. | hardware.md |
| i2c | I2C | basic-clock-reset, pin-control-and-gpio, single-core-runtime | Internal and exposed buses, transactions, bounded errors and recovery. | Confirm controller ownership, bus voltage, pull-ups and attached peers. | pmic-runtime.md |
| clocks-resets-regulators-power-domains | Clocks, resets, regulators, power domains | basic-clock-reset, i2c | PMIC rail control, shared leases, rate selection, resets and domain transitions. | Register selectors do not establish physical voltage, settling or load margin. | pmic-runtime.md |
| dma-controllers | DMA controllers | clocks-resets-regulators-power-domains, single-core-runtime | DMA0/1 channels, descriptors, transfer ownership, completion and drain. | Confirm channel inactivity and safe buffer reclamation for the selected controller. | dma-platform.md |
| system-iommu | System IOMMU | clocks-resets-regulators-power-domains, single-core-runtime | Device address translation, stream ownership, invalidation and fault handling. | Exclude other masters when installing mappings; establish recovery before fault tests. | iommu.md |
| dma-and-device-address-translation | DMA and device address translation | dma-controllers, system-iommu | Complete both shared transfer and translation paths; consumers use only the path they need. | Check address widths, alignment, cache maintenance and device drain per master. | dma-startup.md |
| uart | UART | basic-clock-reset, pin-control-and-gpio, single-core-runtime | Console and exposed serial ports, baud rates, flow control and overrun handling. | Attach a compatible peer for end-to-end exchanges. | uart-platform.md |
| spi | SPI | basic-clock-reset, pin-control-and-gpio, single-core-runtime | Exposed transfer modes, chip selects and bounded completion; DMA modes also require DMA controllers. | Attach an electrically compatible peer and establish pin ownership. | spi-platform.md |
| pwm-and-adc | PWM and ADC | clocks-resets-regulators-power-domains, pin-control-and-gpio, single-core-runtime | Routed PWM outputs and ADC sampling, acquisition timing and interrupts. | Confirm ADC resistor population, reference voltage and allowable source impedance; measure PWM with a suitable peer. | adc-application.md |
| sd-emmc-ufs-spi-storage | SD, eMMC, UFS, SPI storage | clocks-resets-regulators-power-domains, pin-control-and-gpio, single-core-runtime, spi | Populated or exposed block devices; SPI applies only to SPI storage. Include durable update and recovery behavior. eMMC and UFS media are absent on both current boards. | Preserve both microSD cards; establish backups before writes. Do not probe absent eMMC or UFS devices. Their media qualification is not required on these boards. | storage.md |
| type-c-and-display-multiplexing | Type-C and display multiplexing | clocks-resets-regulators-power-domains, pin-control-and-gpio, single-core-runtime | Role, orientation, power switching and routed alternate modes. | Confirm actual wiring, shared PHY ownership and VBUS cutoff before powered host tests. | typec.md |
| usb-controllers-and-phys | USB controllers and PHYs | clocks-resets-regulators-power-domains, single-core-runtime, type-c-and-display-multiplexing | Device and host controllers, PHYs, supported speeds, console and HID. Type-C is required for powered TYPEC24 host mode; USB0 device PIO is independent. | Host accessories and verified VBUS control are needed for host qualification; preserve reconnect access and return ownership. | usb-platform.md |
| radio-sdio | Radio SDIO transport | clocks-resets-regulators-power-domains, pin-control-and-gpio, single-core-runtime | SDC1 command and data transport for the radio; separate from block storage controllers. | Confirm the populated radio variant and supply/clock/reset wiring. | radio-platform.md |
| radio-firmware | Radio firmware bootstrap | radio-sdio | Pinned firmware loading and controller initialization shared by Wi-Fi and Bluetooth. | Authenticate artifact identity and establish successful firmware startup on the installed module. | radio-firmware.md |
| identity-calibration | Identity and calibration access | single-core-runtime, basic-clock-reset | Read-only identity and calibration access with secure ownership respected. | Confirm revision-specific access and calibration interpretation. | thermal.md |
| security-entropy | Security entropy | single-core-runtime, clocks-resets-regulators-power-domains | Randomness generation, health checks and provisioning for network and pairing security. | An assessed entropy profile is required before production security use. | entropy.md |
| wi-fi | Wi-Fi | radio-firmware, security-entropy | Scan, association, secure packet transport, supported modes, coexistence and reconnect. | Use an access point to qualify throughput-independent functional behavior and reconnect. | radio-security.md |
| bluetooth-controller-and-profiles | Bluetooth controller and profiles | radio-firmware, uart, security-entropy | HCI transport, bonding, BLE HID, HFP audio gateway and SCO/eSCO voice. Wired PCM also requires audio controllers. | Use a headset and BLE peers for end-to-end profiles and coexistence; avoid desktop keyboard injection. | bluetooth-native.md |
| audio-controllers | Audio controllers | clocks-resets-regulators-power-domains, pin-control-and-gpio, dma-controllers | Routed I2S capture/output, buffer ownership and timestamped PCM; current streaming uses DMA0. | No microphone is connected. Attach an I2S microphone or compatible audio peer; Bluetooth voice also needs a headset. | i2s-application.md |
| csi-receivers-and-phys | CSI receivers and PHYs | clocks-resets-regulators-power-domains, pin-control-and-gpio, system-iommu | MIPI camera receiver lanes, synchronization, errors and capture buffers. | Qualify each routed camera connector with an admitted sensor and cable. | csi-receiver.md |
| ov5640 | OV5640 sensor control | i2c, clocks-resets-regulators-power-domains, pin-control-and-gpio | Sensor control, power/reset, clocks, modes and exposure. Frame capture also needs CSI. | No module is attached; confirm its voltage, cable orientation and clock requirements. | camera.md |
| image-processing | Image processing and complete camera capture | csi-receivers-and-phys, ov5640, system-iommu | Reference sensor-to-memory capture, conversion, ISP, scaling and exposure/white-balance controls. Additional sensors require their own control and tuning. | Attach the reference sensor; verify frames and supported formats/dimensions. | camera-capture.md |
| display-engine | Display engine | clocks-resets-regulators-power-domains, system-iommu | Framebuffer scanout, composition, synchronization and formats. | Verify buffers, bandwidth and visible output through an admitted display interface. | display-engine.md |
| hdmi | HDMI | display-engine, clocks-resets-regulators-power-domains | Detection, modes, PHY, framebuffer output and HDMI audio. Colorbars can precede framebuffer integration. | Attach a monitor and cable; qualify video modes and audio with a compatible sink. | hdmi-framebuffer.md |
| mipi-dsi | MIPI DSI | display-engine, clocks-resets-regulators-power-domains, pin-control-and-gpio | DSI1 host, PHY, panel command sequences, scanout and backlight. Command transport can precede scanout; touch is separate. | No panel is selected or attached. Admit panel timings, power sequencing and cable before physical output. | dsi.md |
| displayport | DisplayPort | display-engine, type-c-and-display-multiplexing | Complete output path if board routing supports it; Type-C dependency is conditional on that route. | Resolve routing and shared PHY availability; attach a compatible sink if usable. | hardware.md |
| gpu-software | GPU firmware and compiler/runtime | - | Pin compatible firmware, command ABI, shader compiler and runtime dependencies with provenance and licenses. | Confirm BVNC hardware revision compatibility and artifact identities. | gpu-application.md |
| gpu | GPU | clocks-resets-regulators-power-domains, single-core-runtime, gpu-software | Private MMU, cache ownership, firmware startup, command submission, graphics, compute, synchronization and recovery. | Verify power and memory geometry; demonstrate useful outputs and safe shutdown. A compute fixture alone does not qualify graphics. | gpu-platform.md |
| npu-software | NPU model compiler and runtime | - | Pin external compiler/runtime and compiled model artifacts; document operators, shapes and numerical contracts. | Verify supported operators and expected numerical outputs. Native toolkit replacement is deferred. | npu-application.md |
| npu | NPU | clocks-resets-regulators-power-domains, single-core-runtime, npu-software | Private address translation, model loading, execution, output validation, completion and recovery. | Verify power, buffers and output correctness on actual hardware before performance tuning. | npu-platform.md |
| video-and-image-codecs | Video and image codecs | clocks-resets-regulators-power-domains, system-iommu | Supported encode/decode formats, dimensions, buffer lifetimes and application APIs; current JPEG/H.264 paths use the system IOMMU. | Compare actual encoded/decoded outputs and qualify supported limits and recovery. | video-platform.md |
| pcie | PCIe | clocks-resets-regulators-power-domains, pin-control-and-gpio, single-core-runtime | Link training, configuration, interrupts, DMA and endpoint operation. | Confirm connector and power, attach an endpoint and establish safe mapping/recovery before DMA. | pcie.md |
| rtc-and-watchdog | RTC and watchdog | basic-clock-reset, single-core-runtime | RTC source, calendar, retention and alarms; watchdog configuration, expiry and reset cause. | RTC needs clock/backup evidence and interrupt restoration. Intentional watchdog expiry waits for a qualified reset destination. | rtc-application.md |
| thermal-and-frequency-control | Thermal and frequency control | clocks-resets-regulators-power-domains, identity-calibration | Calibrated temperature measurement, throttling and coordinated operating points. | Establish calibration, voltage/frequency limits and temperature response before automatic policy. | thermal-application.md |
| crypto-random-identity-and-secure-storage | Crypto, random, identity and secure storage | clocks-resets-regulators-power-domains, single-core-runtime, identity-calibration, security-entropy | Hardware crypto, random, identity and available secure-storage services. SHA/AES bring-up is independent of qualified entropy. | Respect secure-world ownership and irreversible provisioning; validate each exposed service. | crypto-application.md |
| always-on-processors-and-mailboxes | Always-on processors and mailboxes | clocks-resets-regulators-power-domains, single-core-runtime | Existing firmware services, notifications and usable sleep/wake paths. | Identify processor, memory and firmware ownership before repurposing any resource. | hardware.md |
| remaining-soc-board-functions | Remaining SoC/board functions | pin-control-and-gpio, clocks-resets-regulators-power-domains | Audit LEDs, buttons, IR, Ethernet and debug functions; implement every usable routed function. | Resolve board revision and population; document evidence for inaccessible or unrouted interfaces. | board-wiring.md |
| all-board-support | Complete board integration | @hardware | Integrate every inventory area into a usable OS with concurrent resource ownership, public APIs, repeatable boot, error handling and documented recovery. | Qualify all usable functions and required modes. Record unavailable connectors/accessories explicitly; passing one diagnostic per device is insufficient. | goal.md |
| performance-tuning | CPU, GPU and NPU performance tuning | all-board-support | Optimize and measure actual workloads after complete hardware functionality; reuse valid measurements. | Set workload-specific targets and compare equivalent outputs without redundant benchmark cycles. | validation.md |
