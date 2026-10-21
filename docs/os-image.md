# Integrated OS image

`sunstone-a733-os` is one FreeRTOS image that runs the
[mimus board application](../apps/mimus-board/README.md) and a continuous
thermal service together. The mimus board application already combines the
USB0 device service, the USB2 host with keyboard and read-only USB storage,
the Type-C and PMIC owners and native mimus recognition at EL0. The OS image
adds three fixed parts to it:

- one interrupt table that routes and dispatches every shared peripheral
  interrupt the image owns;
- one start list that runs each application's pin, start and task-privilege
  hooks in order;
- a resident lifetime, in which the armed hardware watchdog is renewed only
  while every listed service makes progress.

Status: implemented, built and host-modeled in both lifetimes. No board has run
the image. The sources are in [apps/os](../apps/os) and the build is
[cmake/a733-os.cmake](../cmake/a733-os.cmake).

## Lifetimes

The build selects one of two lifetimes. Both use the 808-byte OS session
described below and the same services.

| | Supervised | Resident |
| --- | --- | --- |
| CMake option | `SUNSTONE_A733_OS=ON` | also `SUNSTONE_A733_OS_RESIDENT=ON` |
| Contract producer | FEL host launcher; none exists yet for this image | Stage-2 loader ([boot slots](boot-slots.md)) |
| Contract magic | `0x415337334f534331` | `0x415337334f524331` |
| Result magic | `0x415337334f535231` | `0x415337334f525231` |
| WDT0 | Disabled, never renewed | Armed in the four-second mode by the loader |
| ROM redirect and RTC FEL flag | Zero | Zero |
| Diagnostic deadline | 60 s after arming | `UINT64_MAX`; the interrupt path never stops on time |
| Expiry | Equal to the diagnostic deadline | Four seconds after arming |
| Startup deadline | One second after arming | One second after arming |
| End of life | Terminal halt, manual power removal | Watchdog reset into a cold boot |
| EL3 boot | Supervised board boot | Armed board boot, which reads WDT0, the redirect words and the FEL flag back against the contract |

The supervised lifetime keeps the existing bounded contract for the first
physical runs. The resident lifetime defines `SUNSTONE_BOARD_RESIDENT`. Its
board contract check (`apps/board/admission.c`) requires the armed watchdog
fields, zero FEL fields, `diagnostic_deadline == UINT64_MAX` and
`expires_at == armed_at + 4 s`. In both lifetimes the session keeps the
supervised layout, with every timer, RTC and deadline field zero.

At EL1 the board derives its lifetime from the cached diagnostic deadline.
In the resident lifetime that gives `deadline_us = UINT64_MAX / 24`, which
exceeds every counter value, so `board_live()` and every service deadline
derived from it never expire. The drivers the image links only subtract from
or compare with these deadlines; none multiplies or adds to them.

## Resident supervision

Each listed service records the counter time of its latest bounded loop
iteration. The record is one atomic store, which the EL0 recognizer can make.
A failed service parks or suspends without recording.

| Service | Reports from | Window |
| --- | --- | --- |
| PMIC owner | Each poll | 1 s |
| USB0 device service | Each exchange pass | 1 s |
| Type-C I2C service | Each poll | 1 s |
| Type-C power supervision | Each poll | 1 s |
| USB2 host owner | Each controller poll | 1 s |
| Mimus recognizer | Its wait loops, input hashing and the inference yield checkpoint | 3 s |
| Thermal service | Each completed step, at least every 25 ms | 1 s |

The windows are configuration. No board measurement of the longest pause of
each loop exists yet.

The board's supervising task runs every 20 ms. While the board is live and
every service has reported within its window, it renews WDT0 at most once per
second (`apps/os/services.c`). The supervising task starts with the scheduler,
before the services have finished their own startup; USB0, for example,
reports only after its supply rails are ready and TinyUSB has started. Until
every service has made its first report, the task neither renews nor closes,
and the loader's first expiry, four seconds after arming, bounds startup. The
first renewal follows the first complete set of reports. If a service is
still silent at that expiry and the watchdog has not reset the board, the
task closes as below. When a reported service's window expires, a renewal
is refused, or the board stops being live, the task closes admission
(`board_fail`), revokes the Type-C attachment and parks. The watchdog then
resets the board within four seconds of the last renewal.

The renewal (`apps/os/watchdog.c`) takes the register mutex, checks that WDT0
is in the four-second mode with reset output, and that the ROM redirect words
and the RTC FEL flag are zero. It writes the restart key once and reads the
mode back. A changed mode after the write retains the mutex. The shared reload
in `platform/a733/maintenance_watchdog.c` cannot serve this image, because it
requires the FEL redirect and flag that the resident contract forbids.

## Interrupt table

[irq.c](../apps/os/irq.c) defines the table used by the shared
[interrupt table](a733-smp-cached.md#a-fixed-interrupt-table-serves-images-with-several-device-owners).
It has one entry: the thermal sensor interrupt, INTID 71, on CPU0 with GIC
priority `0xd8`. The OS boot patch ([boot.patch](../apps/os/boot.patch)) calls
`a733_irq_table_route` after common boot has disabled every SPI and both
groups, and stops with status 3, detail `0x310`, if routing is refused. The
thermal service enables and masks its own source.

The image applies no per-device `platform.patch`. Every table interrupt is
dispatched after the shared handler's diagnostic-deadline check, and only on
its routed core. The single-application thermal image dispatches INTID 71
before that check and on any core; the OS image does neither. An interrupt
outside the table, or on another core, takes the existing status-4 stop.

## Start list

[start.c](../apps/os/start.c) defines `os_applications`, a fixed list of named
entries, and implements the three board application hooks over it. `main.c`
still calls the hooks at their existing points: pins before the shared
providers are constructed, start after the shared tasks are bound, and both
before the scheduler starts. Each hook runs the entries in list order and
returns failure at the first refusal, so `main.c` keeps its panic codes 10 and
11.

| Entry | Task privilege | Pins | Start |
| --- | --- | --- | --- |
| `mimus` | None; the recognizer runs at EL0 | Validates the model, WAV and arena mappings for workload 0 or 4 | Starts the recognizer for workload 0 or 4 |
| `thermal` | The thermal task | Validates the sealed OS session | Validates the sealed OS session, acquires its clocks and starts its task |

The mimus board image binds the same three named mimus functions directly to
the board hooks, so its behavior is unchanged.

## Session and contract

The session at `0x41f00000` is the 592-byte supervised board session followed
by the thermal owner's facts ([os.h](../apps/os/os.h)):

| Bytes | Field |
| --- | --- |
| 0..591 | Board session, workload 0 or 4 |
| 592..615 | Magic `0x415337334f534131`, version 1, size 808 |
| 616..631 | Thermal owner and clock authority |
| 632..759 | Initial, electrical, clock and supervision evidence hashes |
| 760..767 | APB1 clock word and its frequency |
| 768..771 | ADC clock flag, alarm mask 31, critical mask 31, zero |
| 772..801 | Hot, hysteresis and critical codes for five channels |
| 802..807 | Zero |

The OS admission ([admission.c](../apps/os/admission.c)) requires the OS
contract magic, `reserved[0] = 0x41f00000`, `reserved[1] = 808` and the SHA-256
of all 808 bytes in `reserved[2..5]`. The thermal owner must differ from the
PMIC, USB0, Type-C and USB2 owners, and the thermal fields follow the thermal
application's rules. The unchanged board check then validates every other
field against the board prefix. EL3 seals all 808 bytes; the thermal entry
reads them through `board_session_sealed`.

## Thermal service

[thermal.c](../apps/os/thermal.c) uses the thermal driver and the fixed board
policy for the session lifetime. It reads all five channels every 25 ms, or
sooner after the thermal interrupt, and passes each result to the policy. A
CRITICAL level, a driver fault, a lost owner or an unexpected result ends the
session through `board_fail`. The service then masks its interrupt and keeps
its clock token and register mutex. THROTTLE and NORMAL are load hints; the
image has no reduced-load mode, so it ignores them. USB0 output does not carry
thermal readings; the mimus result frame has no free field for them.

## Build and checks

```sh
cmake -S . -B BUILD ... -DSUNSTONE_MIMUS_BOARD_APPLICATION=ON -DSUNSTONE_A733_OS=ON \
  [-DSUNSTONE_A733_OS_RESIDENT=ON]
cmake --build BUILD --target sunstone-a733-os
```

The configuration needs the same inputs as the mimus board image. Each link
runs [os-image-link.py](../checks/os-image-link.py), which reads the final ELF
and fails the build unless:

- each board application hook, `a733_smp_diagnostic_start`,
  `a733_smp_contract_valid` and `vApplicationIRQHandler` has exactly one
  global definition;
- no per-device interrupt hook from a single-application image is linked;
- every table INTID is a distinct SPI with a valid core and priority, and its
  handler is a linked function;
- every start entry has a distinct name and distinct linked functions;
- the base and overlay models, the recorded WAV and the recognizer arena do
  not overlap one another or any allocated image section, and lie inside the
  stage-2 loader's load window `0x41000000..0x47000000`, below the monitor
  reservation. The check reads the input extents from the generated
  `assets.h`, the arena from `apps/mimus/recorded.h` and the window from
  `boot/a733/stage2/loader.c`.

The result is written to `os-image/link-check.json` in the build directory.

[os-image-model.c](../checks/os-image-model.c) compiles the image's table,
supervision and admission sources on the host, once for each lifetime:

- every INTID from 32 to 1019 is delivered; only INTID 71 on CPU0 reaches the
  thermal handler, and route programming and an inherited-pending refusal are
  checked against a modeled distributor;
- the supervising task runs every 20 ms from time zero and the services first
  report at 0.3 s; with every service reporting, the watchdog is renewed about
  once per second for 120 modeled seconds and never expires;
- when USB0 first reports at 1.5 s, nothing closes, the first renewal follows
  within 20 ms of that report, and renewal continues;
- each of the seven services stalling at 70 s stops renewal within its window,
  and the watchdog expires four seconds after the last renewal;
- for each of the seven services, never reporting leads to expiry at four
  seconds with no earlier close, and the task closes at that expiry if the
  watchdog has not reset the board;
- a service that reports and then stalls before another service's first
  report closes the task when its window expires;
- a refused renewal leads to expiry at four seconds;
- the nominal session and contract are admitted, with 26 resident and 24
  supervised refusals of single changed fields.

The command is in the file's header comment.
[os-image-stage2.py](../checks/os-image-stage2.py) runs the stage-2 entry path
and passes its contract to this model (see [boot slots](boot-slots.md#checks)).

## Not yet integrated

- **I2S receive and the DMA0 interrupt service.** I2S capture is a separate,
  non-board image with its own session, contract, DMA and interrupt patches.
  Joining it needs a combined session extension, audio and DMA ownership in
  the board providers, and a table entry for its interrupts.
- **Radio, CE, HDMI, camera, GPU and NPU.** The board image owns radio and CE
  for odd workloads, which the mimus board composition does not admit. The
  others are separate images.
- **Physical use.** The supervised image needs a launcher and a reviewed
  preparation record for the 808-byte session. The resident image also needs every stage-2 entry
  prerequisite, a qualified watchdog-expiry recovery, and measured service
  windows.
