# sunstoneos

sunstoneos is a FreeRTOS-based embedded operating system under development for
the Orange Pi Zero 3W. Its scope includes all onboard hardware, independently
of the first application, mimus speech transcription.

The attached board has executed a small SRAM probe through its FEL recovery
interface. The FreeRTOS AArch64 kernel foundation passes a focused emulator
check. DDR initialization, physical FreeRTOS boot, and mimus execution remain
unimplemented; no complete board subsystem is qualified yet.

- [Accepted goal](docs/goal.md)
- [Hardware inventory and qualification](docs/hardware.md)
- [Validation policy](docs/validation.md)
- [Current work and next actions](docs/handoff.md)
- [Reference sources](docs/sources.md)
- [Build and kernel check](docs/build.md)
- [Boot investigation and physical evidence](docs/boot.md)
- [Project issues](https://github.com/pedapudi/sunstoneos/issues)

The first executable milestone is booting FreeRTOS and reproducing host
transcription of recorded audio. Complete hardware support remains required
after that milestone.
