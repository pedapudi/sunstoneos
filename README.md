# sunstoneos

sunstoneos is a FreeRTOS-based embedded operating system under development for
the Orange Pi Zero 3W. Its scope includes all onboard hardware, independently
of the first application, mimus speech transcription.

The attached board runs a FreeRTOS kernel check with timer preemption, queues,
and floating-point context preservation. An EL3 monitor recovers from a forced
EL1 stall and returns results through FEL. A native USB device channel passes
status, echo, log, and recovery checks. Persistent boot, application qualification, and
complete hardware qualification remain outstanding. Mimus
now matches the host on 32 development recordings; performance remains below target.

- [Interactive progress dashboard](docs/progress-ui.md)
- [Accepted goal](docs/goal.md)
- [Hardware inventory and qualification](docs/hardware.md)
- [Validation policy](docs/validation.md)
- [Current work and next actions](docs/handoff.md)
- [Reference sources](docs/sources.md)
- [Build and kernel check](docs/build.md)
- [Boot investigation and physical evidence](docs/boot.md)
- [Physical FreeRTOS and recovery check](docs/a733-kernel.md)
- [Native USB diagnostics](docs/usb.md)
- [C and C++ runtime](docs/runtime.md)
- [Local mimus transcription](docs/mimus.md)
- [Project issues](https://github.com/pedapudi/sunstoneos/issues)

Run the local progress dashboard with Python 3:

```sh
python3 tools/progress.py
```

Open `http://127.0.0.1:8765`. It reads the existing project records and board-test
queue automatically. Performance optimization follows complete hardware functionality.
