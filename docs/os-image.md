# Integrated OS image

`sunstone-a733-os` is one FreeRTOS image that runs the
[mimus board application](../apps/mimus-board/README.md) and a continuous
thermal service together. An optional microphone build also captures each
utterance from an I2S microphone and recognizes it in place of the recorded
WAV. The mimus board application already combines the
USB0 device service, the USB2 host with keyboard and read-only USB storage,
the Type-C and PMIC owners and native mimus recognition at EL0. The OS image
adds three fixed parts to it:

- one interrupt table that routes and dispatches every shared peripheral
  interrupt the image owns;
- one start list that runs each application's pin, start and task-privilege
  hooks in order;
- a resident lifetime, in which the armed hardware watchdog is renewed only
  while every listed service makes progress.

Status: implemented, built and host-modeled in both lifetimes, each with and
without microphone recognition. No board has run the image. The sources are in
[apps/os](../apps/os) and the build is [cmake/a733-os.cmake](../cmake/a733-os.cmake).

## Lifetimes

The build selects one of two lifetimes. Both use the 808-byte OS session
described below and the same services. The microphone build extends the
session to 992 bytes and adds its capture client to the services, in either
lifetime; the stage-2 loader admits both session sizes.

| | Supervised | Resident |
| --- | --- | --- |
| CMake option | `SUNSTONE_A733_OS=ON` | also `SUNSTONE_A733_OS_RESIDENT=ON` |
| Contract producer | FEL host launcher (`tools/check-a733-os-supervised.py`) | Stage-2 loader ([boot slots](boot-slots.md)) |
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
| Microphone capture client (microphone build) | Each task pass that ends with no unanswered DMA service request, one pass per tick | 1 s |

The windows are configuration. No board measurement of the longest pause of
each loop exists yet. The DMA worker answers a request in its next
iteration, so a capture client waiting longer on an unanswered request means
the worker has stopped, and the client's record then expires.

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
priority `0xd8`. The microphone build adds the DMA0 interrupt, INTID 143, on
CPU0 with the same priority; CPU0 runs the DMA service worker. The OS boot patch ([boot.patch](../apps/os/boot.patch)) calls
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
| `audio` (microphone build) | The capture client, the DMA bootstrap task and the DMA worker | Validates the sealed OS session and adds PB5, PB6 and PB8 to the board pin authority | Starts the capture client, the audio clock and pin bindings, the DMA service and its interrupt |

The mimus board image binds the same three named mimus functions directly to
the board hooks, so its behavior is unchanged.

## Session and contract

The session at `0x41f00000` is the 592-byte supervised board session followed
by the thermal owner's facts ([os.h](../apps/os/os.h)):

| Bytes | Field |
| --- | --- |
| 0..591 | Board session, workload 0 or 4 |
| 592..615 | Magic `0x415337334f534131`, version 1, size of the whole session: 808, or 992 in the microphone build |
| 616..631 | Thermal owner and clock authority |
| 632..759 | Initial, electrical, clock and supervision evidence hashes |
| 760..767 | APB1 clock word and its frequency |
| 768..771 | ADC clock flag, alarm mask 31, critical mask 31, zero |
| 772..801 | Hot, hysteresis and critical codes for five channels |
| 802..807 | Zero |
| 808..991 | Microphone build only: the I2S receive facts below |

The OS admission ([admission.c](../apps/os/admission.c)) requires the OS
contract magic, `reserved[0] = 0x41f00000`, `reserved[1]` equal to the session
size and the SHA-256 of the whole session in `reserved[2..5]`. The thermal owner must differ from the
PMIC, USB0, Type-C and USB2 owners, and the thermal fields follow the thermal
application's rules. The unchanged board check then validates every other
field against the board prefix. EL3 seals the whole session; each start
entry reads it through `board_session_sealed`.

## Thermal service

[thermal.c](../apps/os/thermal.c) uses the thermal driver and the fixed board
policy for the session lifetime. It reads all five channels every 25 ms, or
sooner after the thermal interrupt, and passes each result to the policy. A
CRITICAL level, a driver fault, a lost owner or an unexpected result ends the
session through `board_fail`. The service then masks its interrupt and keeps
its clock token and register mutex. THROTTLE and NORMAL are load hints; the
image has no reduced-load mode, so it ignores them. USB0 output does not carry
thermal readings; the mimus result frame has no free field for them.

## Microphone recognition

The `SUNSTONE_A733_OS_AUDIO` build adds receive from an I2S0 microphone on
PB5 (BCLK), PB6 (LRCK) and PB8 (DIN) at 16 kHz, mono, 16-bit samples in 32-bit
wire slots. [audio.c](../apps/os/audio.c) composes it as the
[I2S image](i2s-application.md) does: the board's clock registry, pin manager,
register mutex and counter serve a separate audio lease owner, an audio
parent, the I2S0 receive binding and one DMA0 channel through the
[DMA service](dma-service.md). The DMA0 interrupt wakes the DMA worker at each
period's queue end, with a one-tick fallback; the
[I2S stream](i2s-stream.md#fifo-occupancy-bounds-the-handoff) describes why
that matters for the period bridge.

The 184 bytes after the thermal facts carry the receive facts:

| Bytes | Field |
| --- | --- |
| 808..831 | Magic `0x415337334f415531`, version 1, size 184 |
| 832..863 | SHA-256 of the reviewed audio entry record |
| 864..967 | DMA0 handoff: allowed clock configuration, controller, evidence, established facts, owned and service channels |
| 968..975 | Audio parent cold flag; capture length, 16,000 to 80,000 samples |
| 976..984 | Inherited function, pull and drive of PB5, PB6 and PB8 |
| 985..991 | Zero |

The established facts follow the lifetime. The supervised lifetime requires
`0x5f`, whose recovery is manual power removal, and starts the DMA service
with its supervised entry. The resident lifetime requires `0x3f`, whose
recovery is the watchdog reset, and starts the DMA service with its default
entry ([DMA startup](../include/a733_dma_startup.h)).

The admission applies the I2S image's DMA rules to these fields: one service
channel on the controller at `0x04601000`, every channel owned, the board's
fabric clocks, the board lease as handoff and evidence identity, and no
deadline in the session. The DMA owner differs from every board owner and
from the thermal owner.

Each recognition generation captures one utterance after its trigger: at
startup for workload 0, after each keyboard Enter for workload 4. The
recognizer requests the capture of its generation, waits for it, and passes
the captured WAV to the unchanged `mimus_transcribe_recorded` in place of the
recorded WAV. The OS build applies [recognition.patch](../apps/os/recognition.patch)
to the mimus board recognizer for this; the models, the arena and the result
frame are unchanged, and the captured PCM is not exported over USB0.

The capture client follows the I2S transcription client. It opens the stream,
submits it, copies each completed period into the WAV buffer and returns the
period, and cancels once the capture length is complete, still holding the
final period. It publishes the WAV only after the service reports CANCELED
with its reservations held. The next generation's request releases that WAV:
the client then returns the final period, reaps, closes and opens the stream
again. A discontinuous or wrong period, a stream that stops by itself, a
refused request or a failed board ends the session through `board_fail`. The
stream, its periods, the leases and the masked interrupt then stay held. In
the supervised lifetime they stay held until whole-board power removal. In
the resident lifetime the client stops reporting progress, renewal stops and
the watchdog resets the board.

## Build and checks

```sh
cmake -S . -B BUILD ... -DSUNSTONE_MIMUS_BOARD_APPLICATION=ON -DSUNSTONE_A733_OS=ON \
  [-DSUNSTONE_A733_OS_RESIDENT=ON] [-DSUNSTONE_A733_OS_AUDIO=ON]
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
- the start list is `mimus`, `thermal` and, in the microphone build, `audio`,
  and the table holds INTID 71 and, in the microphone build, INTID 143;
- the base and overlay models, the recorded WAV and the recognizer arena do
  not overlap one another or any allocated image section, and lie inside the
  stage-2 loader's load window `0x41000000..0x47000000`, below the monitor
  reservation. The check reads the input extents from the generated
  `assets.h`, the arena from `apps/mimus/recorded.h` and the window from
  `boot/a733/stage2/loader.c`.

The result is written to `os-image/link-check.json` in the build directory,
with the image hash, build identity, lifetime and microphone selection. The
supervised preparation uses it as the image manifest.

[os-image-model.c](../checks/os-image-model.c) compiles the image's table,
supervision and admission sources on the host, once for each lifetime with
and without the microphone:

- every INTID from 32 to 1019 is delivered; only INTID 71 on CPU0 reaches the
  thermal handler and, in the microphone build, only INTID 143 on CPU0 reaches
  the DMA0 handler; route programming and an inherited-pending refusal of each
  entry are checked against a modeled distributor;
- the supervising task runs every 20 ms from time zero and the services first
  report at 0.3 s; with every service reporting, the watchdog is renewed about
  once per second for 120 modeled seconds and never expires;
- when USB0 first reports at 1.5 s, nothing closes, the first renewal follows
  within 20 ms of that report, and renewal continues;
- each of the seven services, eight in the microphone build, stalling at 70 s
  stops renewal within its window,
  and the watchdog expires four seconds after the last renewal;
- for each of those services, never reporting leads to expiry at four
  seconds with no earlier close, and the task closes at that expiry if the
  watchdog has not reset the board;
- a service that reports and then stalls before another service's first
  report closes the task when its window expires;
- a refused renewal leads to expiry at four seconds;
- the nominal session and contract are admitted, with refusals of single
  changed fields: 24 supervised, 26 resident, 45 supervised microphone and 47
  resident microphone. The microphone refusals include the other lifetime's
  DMA established facts.

The command is in the file's header comment.
[os-image-stage2.py](../checks/os-image-stage2.py) runs the stage-2 entry path
and passes its contract to this model (see [boot slots](boot-slots.md#checks)).

[os-image-audio.py](../checks/os-image-audio.py) compiles `apps/os/audio.c`
with the microphone build's own flags, against modeled DMA service replies,
and runs it under unicorn. Two generations each capture 32,000 bytes over
eight periods and publish the exact WAV header and copied bytes; the second
returns the held final period, reaps, closes and reopens the stream first. A
discontinuous period and a stream that stops by itself are refused with the
stream held, and the recognizer's request rules are checked. A fifth case
runs the client's task passes: in a resident build each pass reports progress
except while a DMA request is unanswered or after a failure; a supervised
build reports none. The build directory must export its compile commands
(`CMAKE_EXPORT_COMPILE_COMMANDS=ON`).

## Supervised preparation and launch

[prepare.py](../apps/os/prepare.py) freezes one supervised session offline.
It requires the passed link check of the exact image, a reviewed session
template of 808 or 992 bytes and a reviewed entry record
(`a733-os-supervised-entry-v1`), and refuses to prepare without them. The
record names the session identities and carries:

- the board's supervised entry facts: target USB identity and port, DDR
  reservation, platform conditions and one-attempt manual recovery;
- the four thermal fact records, whose hashes the session holds;
- the DDR reservation of the models, the recorded WAV and the arena;
- in the microphone build, the audio entry record, whose hash the session
  holds and which names the DMA handoff, the inherited pins, the capture
  profile and the microphone, clock, DMA prior-drain and retention
  conditions, and the DMA0 nonsecure-access authority for the session's
  identities.

The bundle holds the image, the session with the linked build identity, the
board's SRAM launcher rebuilt for the OS request magic `0x415337334f534c31`,
the OS contract magic and the session size, every evidence byte the record
cites, and the three recognition inputs.

[check-a733-os-supervised.py](../tools/check-a733-os-supervised.py) launches
one bundle under an existing coordinator claim. It revalidates the frozen
bundle and record, requires the `supervised-ram-recovery` and
`os-supervised-software` prerequisites, and uses the shared supervised
transport, which holds the `tools/usb-access.py` board lock and makes one
attempt. Capture is the mimus board protocol over USB0; the microphone build
allows at most two recognitions in the 59-second capture window.

[os-image-launch.py](../checks/os-image-launch.py) checks both tools on a
linked image with labeled synthetic records. The image's admission, compiled
from `os-image-model.c` with the image's build identity, accepts the prepared
session with the contract the launcher would complete, and refuses it with
another session size or after the startup deadline. Preparation refuses a
wrong or incomplete record, changed thermal or audio evidence, a link check of
another configuration and out-of-rule session fields, each before writing any
output. The launcher freezes the bundle without the original evidence files,
refuses another USB instance and a changed session, and its read-only claim
check accepts only the matching claim through a modeled database connection.
No device, queue database or FEL program is opened.

## Not yet integrated

- **Continuous recognition.** Each generation captures one finite utterance.
  Streaming capture into the recognizer needs a pinned mimus change.
- **Radio, CE, HDMI, camera, GPU and NPU.** The board image owns radio and CE
  for odd workloads, which the mimus board composition does not admit. The
  others are separate images.
- **Physical use.** The supervised image needs a reviewed entry record
  from physical evidence and a queued board test; the microphone build also
  needs a connected, reviewed microphone. The resident image needs every
  stage-2 entry prerequisite, a qualified watchdog-expiry recovery, and
  measured service windows; the resident microphone build also needs a
  reviewed audio entry record with the resident DMA facts.
