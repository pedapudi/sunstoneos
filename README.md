# sunstoneos

sunstoneos is a FreeRTOS-based embedded operating system under development for
the Orange Pi Zero 3W. Its scope includes all onboard hardware, independently
of the first application, mimus speech transcription.

The attached board runs a FreeRTOS kernel check with timer preemption, queues,
and floating-point context preservation. An EL3 monitor recovers from a forced
EL1 stall and returns results through FEL. A native USB device channel passes
status, echo, log, and recovery checks. Persistent boot, mimus execution, and
complete hardware qualification remain outstanding.

- [Accepted goal](docs/goal.md)
- [Hardware inventory and qualification](docs/hardware.md)
- [Validation policy](docs/validation.md)
- [Current work and next actions](docs/handoff.md)
- [Reference sources](docs/sources.md)
- [Build and kernel check](docs/build.md)
- [Boot investigation and physical evidence](docs/boot.md)
- [Physical FreeRTOS and recovery check](docs/a733-kernel.md)
- [Native USB diagnostics](docs/usb.md)
- [Project issues](https://github.com/pedapudi/sunstoneos/issues)

The next application milestone is reproducing host transcription of recorded
audio. Complete hardware support remains required after that milestone.
