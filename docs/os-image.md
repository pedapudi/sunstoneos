# Integrated OS image

`sunstone-a733-os` is one FreeRTOS image that runs the
[mimus board application](../apps/mimus-board/README.md) and a continuous
thermal service together. Four optional builds add more services:

- the microphone build captures each utterance from an I2S microphone and
  recognizes it in place of the recorded WAV;
- the radio build adds the board's Wi-Fi station: firmware loading,
  association, WPA2 with crypto engine entropy, DHCP or fixed addressing and
  one TCP exchange;
- the headset build recognizes speech from a Bluetooth headset in place of
  the recorded WAV. An mSBC option adds wideband headset speech;
- the GPU linear build places mimus linear layers on the CPU or the GPU
  ([recognition dispatch](recognition-dispatch.md)). It needs the resident
  lifetime, and no call reaches the GPU until the mimus engine has its
  exact-sum hook.

The radio build combines with either capture build. With the headset, one
owner of the AIC8800 module loads the firmware once and serves the Wi-Fi
station and the Bluetooth headset. The microphone and the headset exclude each
other, because the recognizer takes one capture source.

The mimus board application already combines the
USB0 device service, the USB2 host with keyboard and read-only USB storage,
the Type-C and PMIC owners and native mimus recognition at EL0. The OS image
adds three fixed parts to it:

- one interrupt table that routes and dispatches every shared peripheral
  interrupt the image owns;
- one start list that runs each application's pin, start and task-privilege
  hooks in order;
- a resident lifetime, in which the armed hardware watchdog is renewed only
  while every listed service makes progress.

Status: implemented. Every build listed under [Build and checks](#build-and-checks)
configures and links in both lifetimes, except the GPU linear build, which is
resident only; the host checks listed there model them. No board has run any of these builds. The sources are in
[apps/os](../apps/os) and the build is [cmake/a733-os.cmake](../cmake/a733-os.cmake).

## Lifetimes

The build selects one of two lifetimes. Both use the 808-byte OS session
described below and the same services. The microphone build extends the
session to 992 bytes and adds its capture client to the services, in either
lifetime; the stage-2 loader admits both session sizes. The radio build keeps
the session size and adds the radio owner and the network client to the
services, in either lifetime. The headset build keeps the session size and
adds the Bluetooth owner and the headset capture client to the services, in
either lifetime.

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

Recorded workload 2 selects PMIC, USB0, mimus and thermal services. It creates
no Type-C, host-power or USB2 tasks, so those three reports are not required.
The supervisor derives that task set once from the sealed session. Service
numbers and the windows below remain fixed across workloads.

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
| Radio owner (radio build) | Each pass while the radio has not failed, one pass per tick | 1 s |
| Network client (radio build) | Each pass while the board is live and the radio has not failed, one pass per tick | 1 s |
| Bluetooth owner (headset build) | Each Bluetooth poll while the application has not failed, one per tick; with the radio, the radio owner makes it after each radio poll | 1 s |
| Headset capture client (headset build) | Each task pass while capture has not failed; a pass follows each wake-up, at most 250 ms apart | 1 s |

The windows are configuration. No board measurement of the longest pause of
each loop exists yet. One radio owner pass repeats session polls with a
500-microsecond elapsed-time budget checked between bounded steps, then sleeps one tick
([board radio](board-radio.md)); the network client never waits on its
socket queues. The radio owner and network client are the board's own tasks
in `apps/board/main.c`; the radio build compiles that file with a build patch
([main.patch](../apps/os/main.patch)) that adds the two reports. A radio
failure, including an absent access point, a refused association or a failed
TCP exchange, stops both reports, and the watchdog resets the board. A
headset failure, including a lost link or a capture that stops, stops the
Bluetooth owner's or the capture client's report in the same way. The DMA worker answers a request in its next
iteration, so a capture client waiting longer on an unanswered request means
the worker has stopped, and the client's record then expires.

The board's supervising task runs every 20 ms. While the board is live and
every selected service has reported within its window, it renews WDT0 at most once per
second (`apps/os/services.c`). The supervising task starts with the scheduler,
before the services have finished their own startup; USB0, for example,
reports only after its supply rails are ready and TinyUSB has started. Until
every selected service has made its first report, the task neither renews nor closes,
and the loader's first expiry, four seconds after arming, bounds startup. The
first renewal follows the first complete set of reports. If a service is
still silent at that expiry and the watchdog has not reset the board, the
task closes as below. When a reported service's window expires, a renewal
is refused, a core has entered the EL3 fault handler, or the board stops being
live, the task closes admission (`board_fail`), revokes the Type-C attachment
and exits with status 5 through `a733_smp_exit_privileged`. The watchdog then
resets the board within four seconds of the last renewal. The exit records
the reason in the supervising core's retained fault slot
([fault capture](fault-capture.md), [reset](reset.md)).

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
groups, and stops with status 3, detail `0x310`, if routing is refused, or
detail `0x311` if the distributor's write-pending bit stays set until the
startup deadline. The
thermal service enables and masks its own source.

The GPU linear build adds the GPU interrupt, INTID 95, with the same
priority on the recognition core: core 2 until the A76 cores are admitted,
then core 6. The recognizer and the GPU worker run on that core, and the link
check requires the route's core to equal `os_recognition_core` in the image.

The radio and the headset add no table entry. The radio owner polls SDC1
with its interrupt, INTID 194, masked ([radio platform](radio-platform.md)),
the crypto engine is polled, and the Bluetooth owner polls UART1 with its
interrupt masked ([Bluetooth H4/HCI](bluetooth-hci.md)). Common boot leaves
these sources disabled.

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
| `mimus` | None; the recognizer runs at EL0 | Validates the model, WAV and arena mappings for workload 0 or 4; in the GPU linear build, the arena's last 4 MiB read-only at EL0 | Starts the recognizer for workload 0 or 4 |
| `thermal` | The thermal task | Validates the sealed OS session | Validates the sealed OS session, acquires its clocks and starts its task |
| `audio` (microphone build) | The capture client, the DMA bootstrap task and the DMA worker | Validates the sealed OS session and adds PB5, PB6 and PB8 to the board pin authority | Starts the capture client, the audio clock and pin bindings, the DMA service and its interrupt |
| `radio` (radio build) | None; `main.c` grants the radio owner and network client | Authenticates the radio profile and refuses radio or crypto engine owners equal to the thermal or DMA owner | Requires the radio owner and network client tasks that `main.c` created |
| `headset` (headset build) | The Bluetooth owner; the capture client runs unprivileged. With the radio, none: the radio owner is the Bluetooth owner | Applies the headset profile rules, refuses owners equal to a board or the thermal owner, and adds PG0..PG9, PM0, PM1, PM3, PM4 and the profile's PM latch. With the radio, requires the board's radio pins and PM latch and the radio's module owners, and adds PG6..PG9, PM3 and PM4 | Starts the capture client and either the Bluetooth owner or, with the radio, the attachment to the radio's module, and binds the client as segment consumer |
| `gpu-linear` (GPU linear build) | The GPU runtime's worker | Accepts the pin handoff | Creates the GPU worker on the recognition core |

The mimus board image binds the same three named mimus functions directly to
the board hooks, so its behavior is unchanged. The OS build compiles the
mimus board application with a build patch
([application.patch](../apps/os/application.patch)) so that its pin and
start entries accept the image's workloads, and replaces the mimus board's
EL3 publication hook with [identity.c](../apps/os/identity.c).

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
| 802..803 | Zero |
| 804..807 | CPU CRITICAL trip in millicelsius; zero keeps 110000, otherwise 1..110000 |
| 808..991 | Microphone build only: the I2S receive facts below |

The board session's workload is 0 or 4 in the base, microphone and headset
builds and 1 in the radio build, with or without the headset. Workload 1 is workload 0 with the board's
radio: the board session then carries the SID authority, the radio profile
address `0x43000000`, its size and SHA-256, and the PM latch, under the
board's own radio rules. The headset build uses the board session's SID
authority and SID evidence as the Bluetooth image does: the evidence is the
SHA-256 of the Bluetooth profile at `0x43000000`. With the radio, the radio
profile occupies `0x43000000` and the Bluetooth profile is at `0x43008000`.

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
session through `board_fail`. The service then masks its interrupt and retains
its clock token and any mutex held across an uncertain operation. A sensor
without a valid reading for one second triggers THROTTLE; five seconds without
a reading triggers terminal CRITICAL. These intervals are inclusive: a
reading remains fresh at an age of 1000000 µs. The session can lower the
CRITICAL trip of CPUB and CPUL; qualification at 70 °C sets the field to
70000. Host and native admission reject values above the vendor's 110000.
The policy uses the lower of the session and vendor trips. Existing zeroed
session fields retain vendor behavior; older validators refuse nonzero values
because those bytes were reserved. Preparation also refuses a nonzero trip
when the linked image manifest lacks the CPU-trip capability.

Before every sensor has supplied a valid reading and during THROTTLE, both
recognition tasks sleep for one scheduler tick at each existing checkpoint.
Driver readiness alone does not permit full load. The thermal owner releases
this startup delay only when all five channels have been seen and the policy
is NORMAL; a stale, hot or critical earlier reading prevents that release.
It publishes the effective level and oldest sensor's freshness deadline in
one atomic word after each successful policy update. Checkpoints enforce
that deadline even when the thermal task is delayed. The owner also expires
clock admission between scheduled reads without an extra THS read.
The thermal task continues sampling independently while recognition backs off. Each checkpoint checks
board liveness before and after that sleep and reports progress only after
it completes. NORMAL removes the delay. There is no waiting loop that could
keep watchdog progress current while inference is stuck. The normal path adds
one thermal getter call: one acquire load and, when NORMAL, a counter read.
The NPU receives the same freshness interval through its exclusive deadline,
one microsecond after the policy's inclusive endpoint.

The GPU dispatcher selects the CPU for new layers during THROTTLE. An already
admitted layer can finish its remaining GPU batches under the existing job
deadlines. NORMAL restores placement only for a still-open session; GPU
failure or incomplete output remains disabled. CPU fallback can shift heat
to the CPU, so the checkpoint delay and critical stop remain necessary.
This policy changes no clock, voltage or device power setting.

The CPU-clock build retains 602 immutable 56-byte snapshots, or 33,712 bytes,
covering the 60 s supervised lifetime at ten snapshots per second plus an
initial and terminal row. Each row contains a timestamp, five temperatures
and ages, a sample sequence and the seen, stale, level and sensor-fault flags.
The thermal owner performs no allocation, output or additional sensor read.
It publishes each row with a release store; readers acquire the count.
A critical reading is recorded before `board_fail` closes the session.

The first recognition's begin/end hooks select its baseline row and freeze
the final count. They surround the recorded recognition call, including model
loading, so the window contains both measured intervals. The end hook runs
after `compute_us` and `total_us` are captured, including on early errors.
Later recognitions cannot overwrite this first window. Other OS profiles
allocate no snapshot buffer and expose no thermal report operation.

With the same CPU-clock compiler flags, the thermal translation unit grows
by 1440 bytes of code, 33,816 bytes of zero-initialized storage and four bytes
of initialized data. The snapshot array accounts for 33,712 bytes of that
storage. The comparison uses the preceding implementation's source and
headers with the same cached compiler; it is a code-size measurement, not a
board timing measurement. The normal checkpoint adds one counter read; the
sampling and interrupt rates do not change.

USB operation 7 returns this immutable window through the existing 64-byte
response header, with up to 1024 payload bytes per request. Its 72-byte report
header binds epoch, session generation, recognition generation, row count,
overflow, CPU critical trip, begin/end times and recognition outcome. The
header and rows total at most 33,784 bytes. The host first fetches the header,
then at most 33 row pages, binding every page to the recognition generation.
The linked manifest records report capability from the native symbols; older
CPU-clock images without it receive no operation 7 request. The host saves
raw frames, `thermal-report.bin` and decoded `thermal-report.json`
separately from the existing recognition output. No transcript buffer grows.
A transport failure ends capture without retry. Earlier recognition artifacts
remain saved when thermal evidence is unavailable.

The decoder reports incomplete evidence when any channel lacks a fresh
reading, a snapshot gap exceeds 125 ms, or the final snapshot precedes the
window end by more than 125 ms. Overflow and recognition failure are separate
outcomes. A complete sampling record establishes neither continuous
measurement nor physical operating-point qualification. USB delivery after a
critical board failure is not guaranteed; the terminal row remains in RAM
until power removal.

The focused checks use modeled devices and task scheduling:

- [Thermal integration](../checks/os-thermal-throttle.py) covers startup,
  throttle/recovery, exact freshness expiry, the 70 °C stop, terminal-row
  publication and fixed snapshot capacity across CPU, GPU and CPU-clock builds.
- [Report capture](../checks/os-thermal-report.py) executes the linked native
  admission, thermal task, window hooks, report copy and USB exchange. It checks
  page bounds, immutable generations, malformed requests, the excluded profile,
  terminal failure and separate host artifacts.
- [CPU owner](../checks/os-cpu-clock-native.py) covers CPU-B, CPU-L and DSU
  request order, sensor waits, no grant and retained failure, with modeled
  driver completion and the real grant decoder and DSU table.

The checked A76 CPU-clock build and raw evidence are in
`~/.cache/sunstoneos/build/resume-thermal-1025-complete/` and
`~/.cache/sunstoneos/evidence/resume-thermal-1025/`. The policy host check,
40 thermal native cases, 51 preparation cases, eight CPU adapter/owner cases
and eleven linked report cases pass. The host entry also passes its profile
and post-capture routing check. No physical temperature response, scheduling
latency or thermal recovery has been qualified.

Power and thermal integration remains incomplete:

- **CPU operating points.** The optional CPU clock build uses
  the pinned 26 MHz tables, speed-bin limits and session grant. The tables
  extend to 1,794 MHz for CPU-L and 2,002 MHz for CPU-B, subject to the bin.
  The driver lowers clusters that it raised above **1,014 MHz** to that
  thermal ceiling, preserving voltage. It can also lower an inspected,
  supported inherited point when the session grants that cluster and ceiling;
  each inherited lowering consumes an authorized change. Source-rate and
  voltage checks precede the transition. The driver checks thermal permission
  again after any voltage ramp, before its first clock write, so a late
  THROTTLE cannot raise a cluster above the ceiling. Refusal preserves the
  old frequency and confirmed voltage and releases the leases.
  Restoration needs authority for the
  original point and another change. Inherited thermal transitions never
  change voltage. Missing or exhausted grants leave inherited rates unchanged.
  The ordinary OS build changes neither CPU clocks nor voltage.
- **GPU lifetime.** Critical thermal failure revokes the GPU backend through
  `board.fatal`. The resident supervisor ends renewal on a stalled GPU wait
  or failed recognition. The OS has no dynamic GPU operating-point control
  or reusable recovery after an uncertain job; resources remain retained
  until the prescribed reset or power removal.
- **NPU integration.** Only the optional
  [NPU service](#optional-npu-service) adds the NPU to
  the OS start list. NPU images use a fixed 492 MHz clock and 800 mV supply
  configuration. The
  [finite board profile](designs/npu-board.md#thermal-admission-and-orderly-stop)
  integrates thermal admission and orderly stop; the other NPU images have
  no thermal-policy callback. Their startup routes the shared SRAM to the NPU
  ([routing design](designs/npu-sram-route.md)). Their healthy CLOSE disables
  local clocks, asserts engine resets and routes the SRAM back to the CPU
  while retaining the power-domain and PLL ownership; it does not establish
  reusable whole-device power recovery.

CPU clock and PMIC voltage board changes still need their existing individual
approvals. Supervised manual recovery, resident watchdog recovery and physical
thermal qualification remain separate prerequisites and evidence gaps.

## Optional NPU service

`SUNSTONE_A733_OS_NPU=ON` links the NPU runtime and one application client
into the supervised OS with recorded input. Configuration stops when it is
combined with the resident lifetime, the microphone, the radio, the headset,
CPU operating-point grants or GPU linear layers.
`SUNSTONE_OS_NPU_CLIENT_MODEL` selects the catalogue entry of
`tools/npu-session-models.json` that the client runs (default 0, the
192-byte to 72-byte convolution). Only that model and its input and
reference vectors are linked; the other four entries have no caller in the
OS. `SUNSTONE_OS_NPU_CATALOGUE` names the artifact map;
[npu-catalogue.py](../apps/os/npu-catalogue.py) compares each embedded file
with its size and SHA-256 hash in the catalogue. The session binds the
SHA-256 of the linked model's SHA-256 digest.

- **Memory.** The service owns `0x47400000..0x47c00000` as one write-back
  DMA32 pool, separate from the board's local pool; that 8 MiB fits any one
  catalogue entry. The link check refuses any loaded section in that
  interval, and startup checks the write-back mapping of every 2 MiB block.
  The runtime then takes the linked model's exact requirement from the
  pool's start: 6 MiB for the default convolution.
- **Session.** A 272-byte extension follows the 808-byte OS session. It
  names an owner distinct from every board and thermal owner, a PCK600 client
  other than the board's, one to eight jobs, the pool, the catalogue digest
  and the digests of the admission, clock, power, supervision and DDR
  records. Preparation refuses NPU authority for an image without the
  service. A link-check record without an `npu` field describes an image
  without it.
- **Tasks.** A bootstrap task, one registered application client and the NPU
  service worker run on core 1, which receives INTID 97. The bootstrap and
  the client read the counter clock, which masks interrupts, so both start at
  EL1t; the task privilege hook admits their entry points only while
  `os_npu_start` creates them. The application calls (`apps/os/npu.h`) refuse
  every other task.
- **Thermal gate.** The OS thermal service admits a job only at the normal
  level with all five channels read within the last second. Any other level
  or a critical failure stops the NPU, and so does the next NPU call or
  bootstrap step once the oldest channel reading is 5 s old. The stop is
  terminal and leaves only CLOSE. A new freshness time alone wakes no task;
  the bootstrap and the client are notified only when the allow or stop
  state changes.
- **Application client.** [npu-client.c](../apps/os/npu-client.c) loads the
  selected model and, for each job, copies the linked input and fills every
  output tensor with the word `0xc35a3ca5`, as the convolution SUBMIT does.
  The runtime cleans and invalidates the output lines before the job, so an
  output byte that the job does not write keeps its pattern byte; an
  invalidate alone would discard the pattern, and the runtime check
  (`checks/npu-session.py --mode runtime`) requires the whole output span
  cleaned and invalidated. After the
  job, the client compares the outputs with the linked reference, counts the
  bytes that kept the pattern and records the SHA-256 digest of the outputs.
- **Failure.** A mismatched or late service reply, the end of the lifetime
  or a failed stop fails the board and keeps the pool, the runtime and every
  power and clock owner. No restart within the lifetime exists. A thermal
  stop before LOAD or before a SUBMIT is a refusal: the client closes the
  NPU and the board stays live.

### NPU report: USB0 operation 6

A request with operation 6 and zero flags, count, offset and generation
returns the 104-byte report of `struct os_npu_report` alone, without the
64-byte frame header. Any other field value stops USB0, and so does
operation 6 on an image without the service. The mimus operations 0 to 4
and the fault report (operation 5) are unchanged; the supervised and
resident images without the service link the same `.text` as before.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | magic `0x5255504e` ("NPUR") |
| 4 | 4 | version 1 |
| 8 | 4 | size, 104 |
| 12 | 4 | finished: 0 before the client publishes, then 1 |
| 16 | 8 | session epoch |
| 24 | 8 | session generation |
| 32 | 4 | catalogue entry used by the client |
| 36 | 4 | jobs selected by the session |
| 40 | 4 | jobs completed and compared |
| 44 | 4 | completed jobs whose outputs differ from the reference |
| 48 | 4 | refused: 1 when a thermal stop, including one for stale thermal data, ended the jobs early |
| 52 | 4 | retained: 1 after a runtime, transport or hardware failure or the end of the lifetime |
| 56 | 4 | stopped: 1 when the runtime stop completed |
| 60 | 4 | output bytes of the last completed job |
| 64 | 4 | of those, bytes that kept the fill pattern |
| 68 | 4 | fill pattern, `0xc35a3ca5` |
| 72 | 32 | SHA-256 of the last completed job's outputs |

Before the client finishes, the report carries only the first four fields
and the session identities; every other field is zero. The client writes
the complete report once and then stores finished=1 with release ordering;
nothing writes the report afterwards. A retained failure fails the board,
and USB0 sends no frame after that, so a retained report is never readable
over USB; it stays in RAM.

[npu-report.py](../apps/os/npu-report.py) decodes the report and classifies
it as pending, matched, mismatch or refused; retained applies only to a
report read from RAM. A numerical mismatch appears as a completed report
with a nonzero mismatch count. A job that wrote nothing appears as a
mismatch whose pattern count equals its output size. The
[supervised launcher](../tools/check-a733-os-supervised.py) requests the
report once after a completed capture when the image's link check records
the NPU profile, and saves it as `npu-report.bin` with its decoding and
classification in the run record. A report of another session keeps its
decoding error instead of a classification. A retained failure leaves that
request unanswered, which ends the run. A capture that leaves less than
100 ms of the 59-second window, and any image without the profile, gets no
operation 6 request.

[os-npu.py](../checks/os-npu.py) runs the linked image's session validator,
constructor, allocator, startup binding, application calls, task frames,
client and USB0 exchange in an AArch64 emulator. Service replies, application
call replies for the client cases, TinyUSB calls, scheduler and cache effects
are modeled. With `--default-image`, it also runs operations 5 and 6 on an
image without the service. The first task frames of the bootstrap, the
client and the service worker carry SPSR 4 (EL1t); a frame for the same
entry points outside `os_npu_start` carries SPSR 0 (EL0t).

The supervised NPU build with the default model measured 995,209 bytes of
`.text` and `.rodata` (949,408 and 45,801); the 2 MiB code region keeps
1,100,912 bytes free after the remaining read-only sections. Linking all five
catalogue models added 1,004,512 bytes of `.rodata`. The service's static
state is 140,329 bytes, including three 32 KiB task stacks and the report. No NPU job, interrupt delivery or thermal
behavior of this image has run on a board.

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

## Radio station

The radio build admits board workload 1. The board composition then runs the
radio as it does in the board image ([board radio](board-radio.md)). It
starts the radio platform on SDC1, loads the firmware and associates. It
secures the link with WPA2, using entropy from the crypto engine, acquires an
IPv4 address by DHCP or uses a fixed one, and makes one TCP exchange as
client or listener. The crypto engine serves only that
entropy source; the image has no SHA-256 or AES request service, because no
part of the image submits such requests. The radio result is not part of the
USB0 output: the mimus result frame has no free field for it. The peer of
the TCP exchange observes the exchange.

`main.c` refuses radio and crypto engine owners equal to a board owner. The
`radio` start entry ([radio.c](../apps/os/radio.c)) checks the owners that the
OS session adds. Before any provider exists, it copies the radio profile and
checks it against the session's SHA-256. It then refuses a radio or crypto
engine owner equal to the thermal owner or, in the microphone build, the DMA
owner. The EL3 publication hook publishes the profile and firmware span and
the four chip-ID words, as the board image's hook does.

The board's radio profile bounds assume a session that ends 60 seconds after
it starts: bootstrap 40 s, startup 50 s, and the radio lifetime and client
deadline 10 ms before the session ends. The supervised radio build keeps them.
The module owner requests four-bit 25 MHz SDIO before its first platform poll
and refuses a slow-card fallback before firmware transfer. At that rate the
firmware bus-time lower bound is 0.242 s, versus 46.689 s at one bit and 400 kHz
([firmware budget](board-radio.md)). Actual startup latency remains unmeasured.
The resident radio build
replaces the bounds with [radio_profile.c](../apps/os/radio_profile.c):
bootstrap 120 s and startup 130 s after the session start, and the session's
unbounded lifetime for the radio and its client. These offsets are
configuration, not a board measurement. A listener then waits for its peer
for the life of the session.

A resident slot needs the radio profile and firmware span as a third asset at
`0x43000000`, after the recognition inputs and the session.

## Bluetooth headset recognition

The headset build runs the [native Bluetooth application](bluetooth-native.md)
on the board's shared providers. The application runs H4/HCI over UART1,
pairs securely with the headset, serves the hands-free audio gateway and
receives audio over SCO or eSCO. The image admits the repeated profiles
(version 3). The headset's voice button starts one recording through the
hands-free voice-recognition procedure. Each recording publishes one segment
into one of two slots: 8 kHz in CVSD (mode 2) or, in the mSBC build, 16 kHz
decoded by the pinned libsbc (mode 3). Mode 3 falls back to CVSD when the
headset or the controller cannot use mSBC. The image never includes the
Bluetooth image's optional call control. Its USB0 output and its own
recognizer are not linked; the profile's recognition models are the mimus
board's own.

### One owner serves the AIC8800 module

The AIC8800 module carries Wi-Fi on its SDIO function and Bluetooth on UART1.
Its power and enable pins, its SDIO card and its firmware are one resource:
the firmware upload applies the Bluetooth patch and then loads the Wi-Fi
image, and the module has no second independent load.

- Without the radio, the Bluetooth application owns the module, as in the
  Bluetooth image. Its owner task powers the module, loads the firmware with a
  bootstrap-only radio session and runs HCI.
- With the radio, the board's radio owner owns the module and loads the
  firmware once with its full Wi-Fi session
  ([radio session](radio-session.md)). The Bluetooth application attaches to
  that module as a second client (`bluetooth_application_attach`). The radio
  owner task polls the application after each radio poll through
  `os_module_poll`, which the radio build's [main.patch](../apps/os/main.patch)
  adds to `apps/board/main.c`. The client takes the session's firmware
  generation while Wi-Fi is up, reconnecting or recovering.

Each client has its own lease and its own failure:

- The Wi-Fi station holds the session's runtime lease, as in the radio
  build.
- The Bluetooth client holds UART1, the H4/HCI binding to the firmware
  generation, the wake pins PM3 and PM4 and its own recovery latch. The wake
  pins join the radio platform before its first poll. An added admission in
  the radio's native adapter authenticates them, as the platform's admission
  contract requires.
- A Bluetooth failure retains UART1 and the Bluetooth stack and leaves the
  module, the Wi-Fi session and the board's recovery latch to the radio. The
  added admission does not read the Bluetooth recovery latch, so a Bluetooth
  failure before the radio's admission does not block that admission.
- A module failure, including a radio failure that retains the session,
  ends the Bluetooth client at its next poll.

The image still ends the session on a headset failure, as described below,
so in this image either failure ends the session; the separation keeps each
client's retention to its own resources. The attachment refuses a module that
the Bluetooth profile does not name: the profile's radio platform and session
owners, topology, radio identity and firmware spans must equal the radio's.

### Capture client

The capture client ([headset.c](../apps/os/headset.c)) is the segment
consumer. It requests a recording only while a recognition generation waits
for audio, through the application's request word
([repeated capture](bluetooth-native.md#one-voice-recognition-activation-records-one-segment)):

- In workload 0 it requests ACCEPT, which admits the headset button.
- In workload 4 each new generation, started by the Enter key, requests START,
  which records at once and sends `+BVRA: 1` to the headset. If that
  recording ends without a segment, as after a headset cancel, the client
  requests ACCEPT, so the button records the generation. A newer generation
  upgrades a pending ACCEPT to START. A START refused because an earlier
  recording is still running is requested again if that recording ends
  without a segment.

Each request passes the published count from the client's last status read,
taken before its borrow found nothing. A recording that publishes between the
borrow and the request makes the application refuse the request, so the
client never admits the button again for a generation that already has its
segment.

Because requests are made only while a generation waits, the next published
segment is the newest generation's. The client borrows it in place, converts
a CVSD segment to a 16 kHz WAV with the Bluetooth image's 8-to-16 kHz
conversion, or wraps a 16 kHz mSBC segment unchanged, acknowledges it and
publishes the WAV for that generation. The segment's SHA-256 is the
application's. A segment that arrives after its generation is captured is
acknowledged without touching that WAV. The recognizer takes the WAV through
the same [recognition.patch](../apps/os/recognition.patch) as the microphone
build.

The client waits on its task notification for at most 250 ms, within its 1 s
progress window. The Bluetooth owner notifies it when a recording ends and
after a stop or disconnection, and `os_headset_capture` notifies it when the
recognizer requests a new generation.

Published segments never contain a lost packet, because a loss stops capture
and discards the segment. A refused borrow or request, capture stopped for any
reason other than the supervised capture window, a failed Bluetooth owner
or a failed board ends the session through `board_fail`. The owner retains
its transports and the client keeps its buffers. In the supervised lifetime
they stay held until manual power removal. In the resident lifetime both
headset services stop reporting and the watchdog resets the board.

### The schedule follows the lifetime

In the supervised lifetime the application keeps the 60-second schedule of
the Bluetooth image: firmware by 30 s, HCI identification by 35 s, headset
setup by 40 s and recordings until 38 s; the recognizer has the remaining
time. After the capture window closes, a pending request is never answered
and the session ends at its deadline.

In the resident lifetime the application has no end. Firmware loading has
120 s from the session start, the bound of the resident radio. When the
firmware first becomes ready, the stage schedule moves so that its 30 s
firmware bound falls at that moment. HCI identification and headset setup
then keep their supervised budgets of 5 s and 10 s after the firmware.
Recording has no window: the headset button records segments until the link
fails, and a stop for any reason closes the audio link within 2 s. These
bounds are configuration, not a board measurement.

The configuration refuses the headset together with the microphone, because
the recognizer takes one capture source, and the mSBC option without the
headset. At run time, the headset pin entry refuses PG or PM pins that
another owner already holds. With the radio, it refuses a module pin
authority or PM latch other than the radio's.

## Recognition on the Cortex-A76 cores and granted CPU operating points

Every OS build sets `configTASK_DEFAULT_CORE_AFFINITY` to the A55 mask 0x3F
([a733_smp_placement.h](../include/a733_smp_placement.h)), so a task created
without an affinity runs only on cores 0 to 5. The recognizer runs on core 2
and the engine's second inference worker (`apps/mimus/parallel_runtime.cc`)
on core 3, through
[recognition-placement.patch](../apps/os/recognition-placement.patch) or the
GPU linear build's own recognizer patch. The shared-peripheral interrupts
and the PMIC task are on core 0; other unplaced tasks may run on any A55
core, including the recognition pair. The
options below are off by
default; the [design](designs/cpu-a76-dvfs.md) and its
[measurements](designs/cpu-a76-dvfs-measurements.md) give the budgets.

- **`A733_A76_ADMITTED`**, a CMake option and compile definition. The
  recognizer moves to core 6 and the inference worker to core 7, and
  `start.c` asserts that the GPU linear build's `OS_RECOGNITION_CORE` equals
  the header's core. Before the scheduler
  the thermal start reads the CPU clock report once (eight reads after the
  mapping check of the oscillator, fuse and CPU PLL pages) and refuses the
  start, panic 11, when CPU-B is not on its locked PLL with a decodable rate.
  The recognizer prints a `CLOCK` line before and after each recognition.
- **`SUNSTONE_A733_OS_CPU_CLOCK`**, supervised lifetime and
  `A733_A76_ADMITTED` only. The session appends the CPU clock driver's
  96-byte grant record and is 904 bytes, or 1,088 with the microphone. The
  admission loads the record against the board session's epoch and
  generation and refuses a grant owner equal to any other owner. Only
  approval step `TOP` grants both clusters, at the bin's top points, and
  then the owner also raises the DSU
  ([CPU top points](designs/cpu-top-opp.md#the-default-path)). The
  thermal task owns the driver ([CPU clock](cpu-clock.md#the-integrated-os-image-owns-the-driver-in-its-thermal-task));
  a record that names no cluster changes no clock or voltage. The recognizer
  starts each timed run only after the owner has finished its requests; an
  owner that has not finished within one second ends the session with an
  `ERROR` line. The link check records the build as `cpu_clock`.

Preparation (`apps/os/prepare.py`) writes a session whose grant names a
cluster only from an approval record of schema
`a733-os-cpu-clock-approval-v1` in the entry record's `cpu_clock_approval`.
The approval names the board identity and port, the image SHA-256, the
session's epoch, generation and lease, the step (A1: CPU-B at 1,196 MHz; A2:
CPU-B at the highest point of the board's bin; A3: CPU-L at 1,196 MHz;
`TOP`: both clusters at the bin's top points), the cluster, the highest
point, the change count, the user's approval text and date, and the board's
read-only B1 record (`a733-os-cpu-clock-b1-v1`: a passed run with a grant
that named no cluster, the fuse word, DCDC3 and DCDC5 readings, pattern and
LFM enables clear). The image refuses to start unless CPU-B reads boot0's
1,014 MHz, because the FEL ROM return does not reset CPU-B. Every grant field must equal
the approval's, and the grant's approval hash is the record's SHA-256. The
session size must agree with the link check's `cpu_clock` record. No new start entry, interrupt, task or resident service is
added.

## Performance-mode service: USB0 operation 8

`SUNSTONE_A733_OS_PERF=ON` adds the high and low performance modes
([user guide](performance-modes.md), [design](designs/performance-modes.md)).
It needs `SUNSTONE_A733_OS_CPU_CLOCK` and may be combined with
`SUNSTONE_A733_OS_NPU`, whose profile then also carries the CPU grant; the
session is then 1,176 bytes. `SUNSTONE_A733_OS_PERF_BOOT=high|low` (default
`high`) sets the boot mode, which the image keeps in `os_perf_boot_mode` and
the link check records as `perf_boot_mode`, beside `perf_modes: true` and `perf_operation: 8`. Every
link check records `perf_modes`; the host requires the exact opcode capability.

- **Owner.** The thermal task's CPU clock owner (`apps/os/cpu.c`) runs one
  service step (`apps/os/perf.c`) after each driver burst, once per tick. It
  replaces the start's one request per granted cluster: once every sensor has
  a reading at NORMAL (at most 500 ms), it reads every block back and
  switches the CPU domains to the boot mode, and only then reports ready to
  the recognizer. The NPU starts at the boot mode's rate.
- **Grant.** This build accepts a grant of both clusters, or of none.
  Other CPU-clock builds retain their TOP and B1 admission rules. Preparation binds a two-cluster grant to an approval record
  of schema `a733-os-perf-modes-approval-v1`: the board, port, image, session
  identities, the user's approval, the board's B1 record, the bin's high
  points of both clusters and the change count.
- **NPU.** After its session's local stop, the NPU bootstrap task waits for
  retune requests instead of suspending, and changes PLL_NPU with
  `a733_npu_startup_retune`.

A request with operation 8 carries its command in the flags byte: 0 report,
1 read back, 2 switch. A switch names the mode in offset bits 9:8 (1 high,
2 low), the blocks in bits 4:0 (CPU-L 0, CPU-B 1, GPU 3, NPU 4) and the
session's epoch in the generation field. A nonzero count, a command above 2,
a report or read back with any offset or epoch, or offset bits outside
`0x31f` stop USB0, as for every other operation; images without the service
stop USB0 on operation 8. The answer is the 192-byte `struct os_perf_report`
of `apps/os/perf.h` alone, without the 64-byte frame header:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | magic `0x4d465250` ("PRFM") |
| 4 | 4 | version 1 |
| 8 | 4 | size, 192 |
| 12 | 4 | publication sequence, even |
| 16 | 8 | session epoch |
| 24 | 8 | session generation |
| 32 | 4 | answer to this request: 0 report, 1 accepted, 2 busy, 3 stale epoch |
| 36 | 4 | number USB0 gave the accepted command, else 0 |
| 40 | 4 | number of the command last taken by the service |
| 44 | 4 | number of the command last completed |
| 48 | 4 | state: 1 while a command or the boot switch runs |
| 52 | 4 | command: 1 read back, 2 switch |
| 56 | 4 | result: 2 done, 3 refused, 4 stopped |
| 60 | 4 | reason (`enum a733_perf_reason`), 0 when none |
| 64 | 4 | failed block, 5 when none |
| 68 | 4 | boot mode: 1 high, 2 low |
| 72 | 4 | mode the readback shows: 1 high, 2 low, 3 mixed, 0 unknown |
| 76 | 4 | speed-bin column, `0x0400` for vf0400; 0 without a table entry |
| 80 | 4 | CPU grant changes left |
| 84, 88, 92 | 4 each | masks of the owned blocks, the blocks read and the stopped clocks (bit 0 CPU-L, 1 CPU-B, 2 DSU, 3 GPU, 4 NPU) |
| 96 | 80 | per block: target MHz and mV, read-back MHz and mV |
| 176 | 4 | the command's block selection |
| 180 | 4 | the command's mode |
| 184 | 4 | the command's elapsed microseconds |
| 188 | 4 | reserved |

USB0 copies the publication with at most four attempts; a copy that does
not settle, and any request before the first publication, is answered busy.
USB0 accepts a read back or a switch into a one-slot mailbox only while no
command runs and the mailbox is empty, and otherwise answers busy. A switch
that names another epoch is answered as stale and posts nothing.
`tools/sunstone-perf.py` sends these requests; the supervised launcher's
`--perf-sequence` switches before each recognition and reads back after the
capture.

## Build and checks

Each row below builds in the supervised lifetime and, with
`SUNSTONE_A733_OS_RESIDENT=ON`, in the resident lifetime. The start list and
the interrupt table follow the options:

| Options | Start list | Table INTIDs |
| --- | --- | --- |
| None | `mimus`, `thermal` | 71 |
| `SUNSTONE_A733_OS_AUDIO` | `mimus`, `thermal`, `audio` | 71, 143 |
| `SUNSTONE_A733_OS_RADIO` | `mimus`, `thermal`, `radio` | 71 |
| `SUNSTONE_A733_OS_AUDIO`, `SUNSTONE_A733_OS_RADIO` | `mimus`, `thermal`, `audio`, `radio` | 71, 143 |
| `SUNSTONE_A733_OS_HEADSET`, with or without `SUNSTONE_A733_OS_MSBC` | `mimus`, `thermal`, `headset` | 71 |
| `SUNSTONE_A733_OS_RADIO`, `SUNSTONE_A733_OS_HEADSET`, with or without `SUNSTONE_A733_OS_MSBC` | `mimus`, `thermal`, `radio`, `headset` | 71 |
| `SUNSTONE_A733_OS_GPU_LINEAR`, resident only, with any row above | That row's entries, then `gpu-linear` | That row's INTIDs, then 95 |
| `SUNSTONE_A733_OS_NPU`, supervised only, alone | `mimus`, `thermal`, `npu` | 71, 97 |
| `SUNSTONE_A733_OS_PERF` with `SUNSTONE_A733_OS_CPU_CLOCK`, supervised only, with or without `SUNSTONE_A733_OS_NPU` | That build's entries | That build's INTIDs |

Configuration stops when `SUNSTONE_A733_OS_AUDIO` and
`SUNSTONE_A733_OS_HEADSET` are both set, and when `SUNSTONE_A733_OS_MSBC` is
set without the headset. With `SUNSTONE_A733_OS_GPU_LINEAR` it stops unless
the lifetime is resident,
`A733_GPU_OUTPUT_EL0_READ_ONLY` is not given, `SUNSTONE_GPU_FIRMWARE` names the
pinned firmware and `SUNSTONE_A733_OS_MODEL_DIR` holds the models
([recognition dispatch](recognition-dispatch.md#build-gate)). The supervised radio and headset builds link, but at
the admitted SDIO clock firmware loading cannot finish within their bootstrap
bounds (see [Radio station](#radio-station)).

```sh
cmake -S . -B BUILD ... -DSUNSTONE_MIMUS_BOARD_APPLICATION=ON -DSUNSTONE_A733_OS=ON \
  [-DSUNSTONE_A733_OS_RESIDENT=ON] [-DSUNSTONE_A733_OS_AUDIO=ON] [-DSUNSTONE_A733_OS_RADIO=ON]
cmake -S . -B BUILD ... -DSUNSTONE_MIMUS_BOARD_APPLICATION=ON -DSUNSTONE_A733_OS=ON \
  -DSUNSTONE_A733_OS_HEADSET=ON -DSUNSTONE_BT_SECURITY_DIR=<pinned Zephyr BR/EDR sources> \
  [-DSUNSTONE_A733_OS_RESIDENT=ON] [-DSUNSTONE_A733_OS_RADIO=ON] \
  [-DSUNSTONE_A733_OS_MSBC=ON -DSUNSTONE_BT_SBC_DIR=<pinned BlueZ libsbc sources>]
cmake --build BUILD --target sunstone-a733-os
```

`SUNSTONE_BT_SBC_DIR` names the directory that holds `COPYING.LIB` and the
`sbc` source directory, the paths that `apps/bluetooth/msbc-source.json` lists.

The configuration needs the same inputs as the mimus board image; the radio
build also uses the radio stacks that the board image already compiles. The
headset build verifies the pinned Zephyr security sources with
`apps/bluetooth/security-source.py`, as the Bluetooth image does, and the
mSBC build checks each libsbc source against
`apps/bluetooth/msbc-source.json` and compiles it with
`SBC_HIGH_PRECISION`, as the Bluetooth image's mSBC build does. Each link
runs [os-image-link.py](../checks/os-image-link.py), which reads the final ELF
and fails the build unless:

- each board application hook, `a733_smp_diagnostic_start`,
  `a733_smp_contract_valid` and `vApplicationIRQHandler` has exactly one
  global definition;
- no per-device interrupt hook from a single-application image is linked;
- every table INTID is a distinct SPI with a valid core and priority, and its
  handler is a linked function;
- every start entry has a distinct name and distinct linked functions;
- the start list and the table INTIDs are those of the build in the table
  above;
- the base and overlay models, the recorded WAV and the recognizer arena do
  not overlap one another or any allocated image section, and lie inside the
  stage-2 loader's load window `0x41000000..0x47000000`, below the monitor
  reservation. The check reads the input extents from the generated
  `assets.h`, the arena from `apps/mimus/recorded.h` and the window from
  `boot/a733/stage2/loader.c`. In the radio and headset builds the profile and
  firmware span `0x43000000..0x43100000` follows the same rules.

The result is written to `os-image/link-check.json` in the build directory,
with the image hash, build identity, lifetime and the microphone, radio and
headset selections. The
supervised preparation uses it as the image manifest.

[os-image-model.c](../checks/os-image-model.c) compiles the image's table,
supervision and admission sources on the host, for each lifetime with and
without the microphone, the radio and the headset:

- every INTID from 32 to 1019 is delivered; only INTID 71 on CPU0 reaches the
  thermal handler and, in the microphone build, only INTID 143 on CPU0 reaches
  the DMA0 handler; route programming and an inherited-pending refusal of each
  entry are checked against a modeled distributor;
- the supervising task runs every 20 ms from time zero and the services first
  report at 0.3 s; with every service reporting, the watchdog is renewed about
  once per second for 120 modeled seconds and never expires;
- when USB0 first reports at 1.5 s, nothing closes, the first renewal follows
  within 20 ms of that report, and renewal continues;
- each service stalling at 70 s stops renewal within its window: seven
  services, eight with the microphone, nine with the radio or the headset,
  ten with the microphone and the radio and eleven with the radio and the
  headset, and the watchdog expires four seconds after the last renewal;
- for each of those services, never reporting leads to expiry at four
  seconds with no earlier close, and the task closes at that expiry if the
  watchdog has not reset the board;
- a service that reports and then stalls before another service's first
  report closes the task when its window expires;
- a refused renewal leads to expiry at four seconds;
- the nominal session and contract are admitted, with refusals of single
  changed fields: 25 supervised, 27 resident, 46 supervised microphone, 48
  resident microphone, 28 supervised radio, 30 resident radio, 49 and 51 for
  the radio with the microphone, 27 supervised and 29 resident for the
  headset, and 30 and 32 for the radio with the headset. The microphone
  refusals include the other lifetime's DMA established facts. The radio
  refusals include workloads 0 and 4 and two board radio rules; the other
  builds refuse workload 1. The headset refusals include a missing SID
  authority or profile digest.

The command is in the file's header comment.
[os-image-stage2.py](../checks/os-image-stage2.py) runs the stage-2 entry path
and passes its contract to this model (see [boot slots](boot-slots.md#checks)).

[os-image-radio.py](../checks/os-image-radio.py) compiles `apps/os/radio.c`
and, in a resident build, `apps/os/radio_profile.c` with the radio build's own
flags and runs them under unicorn. The radio entry admits the nominal
profile and refuses a changed profile, owners equal to the thermal or DMA
owner, another workload, profile size or address, and other memory
attributes; start requires both radio tasks. The resident bounds give the
fixed bootstrap and startup offsets and the session's lifetime, and refuse a
60-second session, a profile that already carries a deadline and a changed
profile.

[os-image-headset.py](../checks/os-image-headset.py) compiles
`apps/os/headset.c` with a headset build's flags, links the real WAV
conversions and runs it under unicorn against a modeled segment publisher
with the Bluetooth application's request word, borrow, acknowledgment and
ticket rules. The
model follows the build's radio, resident and mSBC options. It covers the
pin entry with its 18 refusals, or 24 with the radio, and startup with the
session's lifetime. With the radio, startup creates only the capture client
and attaches the application to the radio's module with the radio owner as
Bluetooth owner. In workload 4 the client requests nothing while no
generation waits and requests START for a new generation. It keeps a captured
generation's WAV when a stray segment arrives, requests ACCEPT after a
recording ends without a segment and upgrades ACCEPT to START for a newer
generation. A recording that publishes between the client's borrow and its
request refuses the request, and a START refused while an earlier recording
runs is requested again after that recording ends without a segment. In
workload 0 it requests ACCEPT only. A refused borrow
or request, a failed application and capture stopped for a deadline, backlog
or loss each end the session; the supervised capture window does not. One owner iteration polls a live application and
retains it after a board failure, and the resident build reports progress
only before a failure. The mSBC build wraps a wideband segment unchanged and
refuses an unknown capture state; other builds refuse a wideband segment.
The recognizer's request rules and the client's wake-up are also checked.

[os-image-identity.py](../checks/os-image-identity.py) runs the linked
image's EL3 publication hook under unicorn. Each build admits its own
workloads and publishes the recognition inputs; the radio build also
publishes the profile and firmware span and the chip-ID record, and the
headset build the copied Bluetooth profile, its span and the chip-ID record.
With both, workload 1 is admitted and the Bluetooth profile is copied from
`0x43008000`.
A second call, a Bluetooth profile of another size and a refused profile
publish nothing more.

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

[os-image-dispatch.py](../checks/os-image-dispatch.py) checks the GPU linear
build's placement table, dispatcher, mapping rules, pool pages, arena
assertion, supervision of GPU waits and configuration gate on the host
([recognition dispatch](recognition-dispatch.md#host-checks));
[pvr-mimus-service.py](../checks/pvr-mimus-service.py) and
[gpu-recognition-engine.py](../checks/gpu-recognition-engine.py) check its GPU
session and hook ([gpu-mimus-service.md](gpu-mimus-service.md)).

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
  identities;
- in radio or headset builds, the profile and firmware evidence described
  below, and the exclusive radio DDR reservation `0x43000000..0x43100000`.

The bundle holds the image, the session with the linked build identity, the
board's SRAM launcher rebuilt for the OS request magic `0x415337334f534c31`,
the OS contract magic and the session size, every evidence byte the record
cites, and the three recognition inputs.

Radio and headset preparation uses [radio-assets.py](../apps/os/radio-assets.py).
The entry record's `radio_assets` field is an absolute `path` and `sha256`
reference to an `a733-os-radio-assets-v1` JSON record. That record has
`reviewed: true`, the entry's `identity`, `port`, `image_sha256`, `epoch`,
`generation` and `lease`, and a nonempty `lifetime` shared by its platform
facts. It describes these existing inputs:

| Field | Required input |
| --- | --- |
| `radio_profile` | Path and SHA-256 of the immutable board radio profile, only when radio is linked |
| `headset_profile` | Path and SHA-256 of the 832-byte Bluetooth recognition profile, only when headset is linked |
| `platform` | Path and SHA-256 of an `a733-radio-sdio-id-v1` record referencing the reviewed topology, pins, startup, bootstrap and recovery facts |
| `chip_id`, `sid_authority`, `radio_sid` | With radio, the four secure chip-ID words, session SID authority and path/hash of its evidence |
| `radio_ce`, `radio_entropy` | With radio, path/hash references matching the profile's crypto-engine and entropy evidence digests |
| `bluetooth_wiring`, `bluetooth_sid`, `bluetooth_transport`, `bluetooth_scheduling` | With headset, path/hash references matching the profile's wiring, SID, fresh-transport and scheduling evidence digests |
| `payloads` | Four objects with absolute `path`, `sha256` and integer `address`, in ADID, Bluetooth patch, patch-table and Wi-Fi firmware order |

The existing [radio firmware catalogue](radio-firmware.md) fixes each payload's
size and hash; the profile's decoded chip identity selects the Wi-Fi variant.
The profiles must agree on shared module owners, topology, PM authority, chip
identity and firmware spans. Radio uses workload 1 and its profile at
`0x43000000`; headset alone uses workload 0 or 4 at that address. With both,
the headset profile goes at `0x43008000`. The four shared firmware spans stay
inside `0x43010000..0x43100000`. Upload ranges cannot overlap one another,
the image, the session or the recognition inputs.

The platform facts retain the [radio qualification prerequisites](radio-sdio-qualification.md).
The startup record must set the existing `peri0_400mhz_branch_verified` predicate
for the firmware owner's four-bit mode. Native startup independently observes
and leases that branch throughout use.
Preparation also requires the linked fast-mode function, which the older
slow-only images omitted. Constructor checks and exact-image software evidence
establish that the selected module owner requests it.
Fitted supply routing and stable inherited PM2 behavior remain mandatory.
An authenticated PM record with zero mask and value selects the normal OS
[pin ownership contract](pins.md#configuration-preserves-neighbors-and-primes-output-latches).
A complete mask still requires verified latch history or a verified reset
contract. Neither form supplies fitted-board facts or authorizes another
module power/reset writer.

Preparation freezes every referenced evidence file, profile and firmware blob
once. The upload list refers to those frozen files, including only one copy of
each firmware role when both clients share the module. Before launch, bundle
validation derives the same list from frozen bytes without reading the original
input paths. The existing launcher preloads and verifies those segments with
the recognition inputs. Staging does not establish the physical startup timing
described under [Radio station](#radio-station).

[os-radio-staging.py](../checks/os-radio-staging.py) checks combined preparation,
frozen verification, preload bytes and range/identity refusals with labeled
synthetic facts. It also checks single-profile selection, standalone Bluetooth
and plain OS compatibility. The native Bluetooth profile validator, compiled
for the OS headset profile, admits the staged headset bytes. These checks reuse
the linked radio/headset image and unchanged runtime evidence; their synthetic
facts cannot authorize a board run.

[check-a733-os-supervised.py](../tools/check-a733-os-supervised.py) launches
one bundle under an existing coordinator claim. It revalidates the frozen
bundle and record, requires the `supervised-ram-recovery` and
`os-supervised-software` prerequisites, and uses the shared supervised
transport, which holds the `tools/usb-access.py` board lock and makes one
attempt. Capture is the mimus board protocol over USB0; the microphone build
allows at most two recognitions in the 59-second capture window. Recorded
workload 2 permits one recognition and requests only its transcript and final
status. The mode comes from the frozen session; missing storage never selects it.
Radio workload 1 also permits one recognition.
For an image with the NPU profile, the capture is followed by one NPU report
request ([operation 6](#npu-report-usb0-operation-6)).

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
A modeled USB0 device then serves the capture callback. With the NPU profile
set in a copy of the frozen link check, operation 6 follows a completed
capture once, after any fault report, and its matched and mismatch reports
are classified. A report of another session is kept unclassified, an
unanswered request ends the run, and a failed capture or one that leaves
less than 100 ms of the window sends no request. Without the profile no operation 6 is sent.
In the CPU clock build it also prepares an A2 grant for a modeled board
`3-3` and refuses an approval for another port or session, a B1 record whose
bin tops at 1,508 MHz, a grant above the approval, the A1 step at 2,002 MHz,
a missing approval, a second change, the other cluster and an approval
without a grant. No device, queue database or FEL program is opened.

### First bounded launch with recorded audio

The smallest OS launch uses the supervised lifetime, staged recorded audio
and explicit board workload 2. Recorded builds admit that workload in either
lifetime; `OS_STAGED_WORKLOAD` excludes the microphone, headset and radio
options. Both the workload rule and staged USB0 mode use it. The USB0 owner must first finish its provider
and device-stack startup. Recognition then authenticates the staged models
and WAV and produces one transcript without an external USB disk.

The [autonomous card bundle](designs/autonomous-card-os.md) supplies the
resident recorded session, models and WAV from the selected slot. Its complete
performance profile uses eight workers and a fresh per-boot CPU grant for both
clusters' vendor operating points. Resident supervision requires only the
selected PMIC, USB0, recognizer and thermal tasks; absent USB-host tasks do not
block renewal. No host trigger or transcript acknowledgment is required.
Physical cold entry, ownership and reset qualification remain separate.

Workload 0 still requires storage attachment and a sector-zero read completed
during recognition. Workload 4 additionally requires a boot keyboard and its
neutral-to-Enter trigger. Both storage modes retain their Type-C freshness,
attachment identity, copied-sector and recognition/storage overlap checks.
These profiles remain required for storage integration; workload 2 establishes
no storage or keyboard coverage. All three call whole-recording recognition.
Paced replay is a separate mimus service operation.

Select `SUNSTONE_A733_OS=ON`, with `SUNSTONE_A733_OS_RESIDENT`,
`SUNSTONE_A733_OS_AUDIO`, `SUNSTONE_A733_OS_RADIO`,
`SUNSTONE_A733_OS_HEADSET`, `SUNSTONE_A733_OS_GPU_LINEAR`,
`SUNSTONE_A733_OS_CPU_CLOCK` and `A733_A76_ADMITTED` all `OFF`.
The resulting image uses the 808-byte session and requests no CPU operating-point
change. Its recognition task and inference worker run on A55 cores 2 and 3.

This configuration still starts all eight CPUs through
[cached boot](../platform/a733-smp-cached/boot.c) before the shared scheduler
runs. Disabling A76 recognition changes task placement; it does not remove
secondary startup, coherence, interrupt delivery or scheduling from the image.
The current CPU qualification image has physically completed its finite
all-eight workload and per-core timer checks, as recorded in the
[handoff](handoff.md). That evidence establishes the tested CPU path; the
integrated OS composition still needs its own supervised execution.

Offline preparation requires these inputs for the selected board and image:

- A passed [OS link check](../checks/os-image-link.py) of the exact ELF and a
  reviewed workload-2 session template. Workload 2 occupies the existing board
  field at byte 400. Its latch, Type-C and USB2 owner, pin, source and host-supply
  fields must be zero. The separate USB reference field at byte 532 must remain
  mode 1 (cold initialization); single-core ROM-takeover mode 2 is refused.
- A reviewed `a733-os-supervised-entry-v1` record binding the image, template,
  USB identity and port, session epoch, generation and lease. Its board DDR
  reservation covers `0x41000000..0x42200000`.
- Platform evidence for the CPU/DSU transition, cache and dirty-alias exclusion,
  Secure and Nonsecure DRAM access, exclusive CPU/memory/DMA ownership,
  exclusion of remote writers, USB clocks/power/reset ownership and device
  mappings, and physical counter continuity. The inherited
  [validator](../tools/check-a733-smp-observe.py) requires evidence for each
  condition. A record labeled `physical_coherence_result=under-test` does
  not qualify coherence or resolve a failed startup.
- Thermal initial-state, electrical, clock and supervision records bound to
  the image and board. These authorize the sensor and interrupt owner,
  supplies, SID shadows, mappings, APB1/ADC clock profile and permanent
  resource retention. The session carries each record's hash.
- The exact base model, overlay and 16 kHz mono WAV declared by
  [the model manifest](../apps/bluetooth/recognition-models.json) and
  [the recorded-input manifest](../apps/mimus-board/inputs.json). Their separate
  DDR reservation covers `0x44000000..0x45000000`, `0x45000000..0x45100000`
  and the recognition arena at `0x46000000..0x47000000`.
- A reviewed one-attempt recovery plan: demonstrated FEL access, explicit
  reconnection before resumption, microSD preservation and whole-board power
  removal before any retained resource is reused.

The coordinator must then bind the frozen bundle to its existing claim and
the two prerequisites `supervised-ram-recovery` and `os-supervised-software`.
The software record must cover the exact image, SRAM launcher, host sources,
admission cases, launcher cases, capture and lifecycle checks.
[The launcher](../tools/check-a733-os-supervised.py) validates these bindings
before device access. It captures one workload-2 result within 59 seconds;
the native contract expires after 60 seconds. Success requires the complete
transcript and a final live status with matching session identities. The USB
frame retains version 1 and reports storage state 3, meaning no host workload
was requested. Recognition uses status and text operations; a sector request
ends the session. The existing prior-boot fault-report operation remains
available. Workloads 0 and 4 continue to require the copied sector and
recognition/storage overlap evidence.
The image ends at a terminal halt, so recovery requires whole-board power
removal even after successful capture. Stop at the first transport failure.

The physical entry record is still missing. Synthetic records used by host
checks establish validation behavior and cannot authorize this launch.
The current CPU results do not supply USB, thermal, RAM or recovery authority
for this image. This supervised configuration keeps WDT0 disabled; resident
watchdog-expiry qualification is not its prerequisite.

The recorded-audio build and link check pass with the options above.
[The focused check](../checks/os-staged-recorded.py) passes 26 cases using the
compiled AArch64 readiness, recognizer copy and USB frame producer with the
actual host capture. It covers startup and later failure, zero Type-C/USB2
accesses in workload 2, and storage overlap, freshness, sector export and
keyboard acknowledgment in workloads 0 and 4. USB providers, scheduling and
inference completion are modeled. The staged preparation check passes 27
cases, including native admission with cold USB reference mode 1 and refusal
of host authority or ROM takeover. The linked publication hook passes eight
cases. Evidence is under `os-staged-recorded-20261024/` in the local cache.
No integrated OS physical execution is established by these checks.

## Not yet integrated

- **Continuous recognition.** Each generation captures one finite utterance.
  Streaming capture into the recognizer needs a pinned mimus change.
- **Headset failure without ending the session.** A Bluetooth client
  failure is contained to its own resources at the module, but the image
  ends the session on any headset failure, also while the radio is still up.
- **SHA-256 and AES requests.** The crypto engine serves only radio entropy;
  no part of the image submits hash or cipher requests.
- **HDMI, camera and GPU graphics.** These remain separate images. The GPU
  linear option integrates recognition compute into the OS.
- **Resident NPU and recognition acceleration.** The supervised OS integrates
  a finite NPU application with its existing model catalogue. Resident NPU
  startup and cold-card authority are not yet integrated. The NPU does not
  produce the exact sums required by the mimus recognition path.
- **Physical use.** The supervised image needs a reviewed entry record
  from physical evidence and a queued board test; the microphone build also
  needs a connected, reviewed microphone. The radio and headset builds need
  supervised preparation that stages their profile and firmware, and a
  reachable access point or paired headset. The supervised radio and headset
  builds request four-bit 25 MHz SDIO and still need physical qualification and
  measured completion within their unchanged 40 s and 30 s bootstrap bounds.
  The build with both needs a
  staged span that holds both profiles and the shared firmware. The resident image needs every
  stage-2 entry prerequisite, a qualified watchdog-expiry recovery, and
  measured service windows; the resident microphone build also needs a
  reviewed audio entry record with the resident DMA facts.
