# sunstoneos

sunstoneos is a FreeRTOS-based embedded operating system under development for
the Orange Pi Zero 3W. Its scope includes all onboard hardware, independently
of the first application, mimus speech transcription.

On the board, a FreeRTOS kernel check passes with timer preemption, queues and
floating-point context preservation. An EL3 monitor recovers from a forced EL1
stall and returns results through FEL. A native USB device channel passes
status, echo, log and recovery checks. All eight CPU cores have reached their
first FreeRTOS task on the board; sustained eight-core scheduling has passed
only in emulators.

An [integrated OS image](docs/os-image.md) runs the mimus board application
with a thermal service. It builds in six checked variants: supervised,
resident, resident with I2S audio, radio, resident with radio, and supervised
Bluetooth headset. The resident variants are entered by a stage-2 microSD
loader. Every variant passes host models; no board has run any of them. A
[source audit](docs/audit/README.md) and six rounds of fixes and features based
on it are merged. They were checked on the host only, and nothing in them ran
on the board. A CPU clock and voltage driver coupled to the thermal policy
exists, but no image grants a clock or voltage change. Persistent boot,
application qualification and complete hardware qualification remain
outstanding. Mimus matches the host on 32 development recordings. One board run of
recognition compute on one core at the boot loader's 494 MHz CPU clock took
about 2.2 times the audio duration, against a target of at most 0.05 times.

- [Interactive progress dashboard](docs/progress-ui.md)
- [Accepted goal](docs/goal.md)
- [Hardware inventory and qualification](docs/hardware.md)
- [Validation policy](docs/validation.md)
- [Current work and next actions](docs/handoff.md)
- [Integrated OS image](docs/os-image.md)
- [Implementation audit and fix rounds](docs/audit/README.md)
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
