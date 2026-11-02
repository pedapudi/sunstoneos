# OS image on a virtual board in QEMU

**Status: implemented.**

The SunstoneOS image starts on a virtual eight-core board in QEMU and runs
through startup to the first application task. `checks/qemu-os-boot.py` builds
and boots it in under ten seconds without a board. The resident lifetime, the
OS CPU clock owner, the NPU and GPU services, the USB host and the radio are
not modeled.

**Board-qualified scope:** None. No board was used. The evidence is host-only:
the virtual board shares the OS, board application and driver sources with the
A733 images and replaces the hardware, so it finds software startup faults
and cannot qualify the hardware behind a provider.

## What runs

The image links `apps/os` (application, services, session, start, thermal and
admission), `apps/board/main.c`, the thermal, clock-lease, rail-lease, PMIC,
power-domain, pin and GPU-resource driver cores and their adapters, and a real
application, all unmodified. The scheduler is the FreeRTOS SMP port with the repository's
SMP patches in `patches/`. The board description
[platform/qemu-os](../../platform/qemu-os) supplies the rest; see
[the virtual board](../qemu-os-board.md).

| Case | Condition | Result |
| --- | --- | --- |
| Normal start | Scripted sensors, USB transport ready 30 ms after its task starts | `ss_live()` is 1 at the application's main entry on core 2; hello-compute finishes and returns the Adler-32 of its input |
| Zero sealed deadline | The earlier `apps/os/services.c`, which compares every application wait with the sealed session's deadline field | The first application wait is refused and the application stops with failure `0x757` |
| Sensors not yet read | Two sensors answer at 900 ms | The application starts with `ss_throttled()` 1 and finishes; the throttle clears after the last answer |
| Sensor lost | One sensor never answers | After five seconds the thermal owner ends the session with a thermal failure record |
| Service start fault | Erased calibration, closed sensor clock, or a held PMIC bus line | A failure record names the thermal or PMIC owner within one second |
| Latency CPU control | A76 composition with the latency instrumentation | The application reaches its main entry on core 6 with the OS live |

The negative control runs the whole check against the file from the commit
before the fix (`git show 15914f7c^:apps/os/services.c`, passed as
`--services-source`): the normal start then fails with failure `0x757`.

## Applications

hello-compute runs to its normal finish. The latency CPU control reaches its
main entry and then ends with failure `0xb03`, because it requires the CPU
clock owner's record to show the top operating points. These applications do
not run yet:

| Application | Reason |
| --- | --- |
| latency with DMA | Needs the OS DMA service, which requires the returning lifetime and the DMA controller |
| queue-add, queue-gemm, gpu-fast, gpu-lifecycle | Need the GPU or NPU service |
| usb-input, usb-serial, usb-camera | Need the USB host, Type-C and class services |
| cpu-npu-load, cpu-reliability | Need the returning lifetime and the CPU clock owner |
| mimus | An external application that needs the C++ runtime, the speech services and the CPU clock owner |
