# Native fan and status LED application

The supervised PWM application retains one native owner for continuous fan
operation, duty changes and clean stop/restart. It drives PWM0 channel 5 on PD5
at 40 kHz, with requested active ticks from 1 through 599 in each 600-tick
period. The optional fitted status LED uses PL7 as an active-high GPIO.
Registers and copied USB diagnostics establish software state; waveform, gate
voltage, discharge time, off-current and visible LED behavior need physical
qualification. No physical PWM operation has been attempted.

The [board wiring audit](board-wiring.md) is the canonical routing and
population record. It accounts for the fan switch, LED, exposed header routes,
optional display waveform and ADC connections. Its source hashes and rendered
sheets are preserved under
`~/.cache/sunstoneos/evidence/pwm-adc-topology-20261018/`.

## Ownership and startup

EL3 privately seals the complete 872-byte session before provider construction.
The board prefix selects supervised workload 2: USB0 and the common PMIC worker.
The PWM suffix binds a separate owner, initial-state, memory, power, supervision
and optional LED authority, inherited pin fields, exact APB0/APBS0 profiles,
command limit and session lifetime. The image fixes channel, frequency and
polarity. Requests cannot select raw registers or arbitrary pins.

The common clock registry reserves the whole PWM0 controller exclusively.
Channel 5 shares a clock pair with channel 4; reset and global controls are
not independently assigned. Stable OSC, REF and fabric reservations protect
its inherited source and bus. A PERI0-derived APB0 also reserves that parent.
The driver does not retune shared parents. The optional GPIO LED additionally
reserves APBS0 and verifies its exact admitted source/divider. Only DCXO or
SYS_CLK24M is accepted. APBS1 and main fabric reservations do not cover APBS0.

The common rail and PMIC services retain BLDO1 and verify its programmed
1.8 V value. The LED option also retains and verifies DCDC1 at 3.3 V.
Physical supply, settling and writer exclusion remain separate admission facts.
PL GPIO writes use the existing provider and a complete authenticated output
latch shadow; GPIO samples cannot reconstruct that shadow. Neighboring outputs,
including their latch values, remain preserved.

Cold admission requires a gated/reset PWM0 controller, no inherited capture,
group or complementary mode, and owned disabled/drained GIC interrupt 67
(SPI index 35). The pinned DTS uses `GIC_SPI 35`; the manual interrupt table
lists absolute interrupt 67. This is not serial SPI controller numbering.
Clock/reset, pin, voltage and remote writers must be excluded. Native access
checks EL1/task/interrupt context and common register-mutex ownership.
Controller setup occurs once; no post-fault reset or retry exists. Startup
reaches a verified disabled waveform and GPIO-low fan pin before accepting
application commands.

## Commands retain ownership across clean stops

Cycle mode uses SYS_CLK24M at 24 MHz with pair divider and prescaler both one.
A 25,000 ns period contains 600 ticks, so its period field is 599. Active ticks
select duty in increments of 1/600. Zero output uses STOP; constant-high output
is not part of this profile. The finite-pulse driver mode remains available
for its separate frozen diagnostic image.

Commands are START with active ticks, SET_DUTY with active ticks, STOP, and
optional LED GPIO level. One owner task serializes a single pending request.
Each 80-byte request binds the full session digest, epoch, generation,
monotonic sequence, command, argument and a 20–100 ms completion budget.
Acceptance and completion are distinct acknowledgements. Duplicate sequence
numbers are idempotent only when every request byte matches. Changed duplicates,
stale sequences and new requests while busy are rejected without hardware
replay. Status polls and duplicates never extend deadlines.

A start or duty change first masks the owned interrupt, disables its source,
clears PWM enable and clock pass, and drains only its own stale pending state.
The existing PD5 lease then changes to GPIO output-low. While disabled, the
owner waits for period readiness, writes and checks the new period/counter,
resumes mux 7 under the same lease, re-arms the interrupt and enables PWM last.
The final old pulse may be truncated and a bounded low interval separates
profiles. The application makes no glitch-free edge claim.

Each start requires its own interrupt observation. The ISR masks only interrupt
67 and publishes an atomic notice. The task preserves that notice through
register-lock contention, disables the PWM interrupt source and acknowledges
only channel 5's W1C status bit. Continuous output then runs without a 40 kHz
interrupt stream. Old notices are invalidated only after source disable and
owned pending drain. Clean STOP verifies PWM enable and clock mask, then
verifies GPIO-low. It keeps the clock, pin and rail leases for START; it does
not release/reacquire them or reset the controller.

The USB interface uses byte 0 for a copied 512-byte status record, byte 1 for
permanent cancellation, and byte 2 followed by the 80-byte command. Fragmented
commands have a bounded receive window. Clients must finish reading a record
before sending another command. The host's ordered `--command` options expose
`start:TICKS`, `duty:TICKS`, `stop` and `led:0/1`. It sends each command once,
polls copied acknowledgements, and stops at a decoded retained failure.

## Electrical observations have limited meaning

The schematic's sheet 6 connects BLDO1 to VCC-PD, VCC-PL, VCC-IO and VCC-PE.
PMIC U1B pin P6 and C19, 2.2 microfarads, identify that output branch.
The fan's separate 5 V source reaches its connector through R88. Sheet 5
places nominal R89 = 10 kilohms between PD5 and Q2's gate.

The retained-low transition disconnects the mux, preserves drive/pull and
neighbor fields, preloads the owned DATA clear alias, selects GPIO output and
checks low. Resume requires the exact retained GPIO-low lease and a verified
disabled PWM producer. An uncertain transition faults the manager and retains
the token.

At the admitted 1.8 V pad supply, the input-low limit is 0.54 V; Q2's minimum
threshold is 0.45 V at 250 microamps. An input pull-down and digital-low sample
therefore cannot establish Q2 off. The GPIO output-low limit is 0.2 V. Using
nominal R89 and Q2's 5 microamp maximum gate leakage gives a 50 mV drop, or
0.25 V including that output-low limit. This estimate excludes resistor
tolerance and transient gate charge. Threshold voltage is not an off-current
guarantee; physical capture must qualify discharge and actual fan behavior.

PL7 feeds the LED through R86 while DCDC1 feeds the same LED through R85.
A requested GPIO-low state therefore does not necessarily mean optical off.
GPIO LED control does not implement the pin's alternate S_PWM0 mode.

## Failure and remaining qualification

Cancellation and deadlines revoke success before newly queueing USB output.
Session expiry is checked while idle as well as during commands. Faults retain
clock, rail, pin and uncertain register ownership permanently. Continuous mode
has no finite pulse-count bound: an active output can remain active after a
fault until every supply is removed. No diagnostic claims that cancellation,
a deadline or an uncertain write stopped the fan. Retention does not recover
stalled MMIO.

Bytes already queued in TinyUSB cannot be retracted. A cancelling host discards
that session's output; command acknowledgements must match the requested
sequence and arguments. Retained shared locks can prevent USB error transfer,
so RAM diagnostics remain owned without a guaranteed copied failure response.
If USB is the only supply, unplugging it removes board power. Additional
supplies must also be removed, and the coordinator verifies a new instance
after reconnect. The application does not arm a watchdog or write storage.

The finite image and its evidence remain frozen under
`~/.cache/sunstoneos/evidence/pwm-native-20261018/`. Reusable operation has
separate source, build and focused evidence under
`~/.cache/sunstoneos/evidence/pwm-service-20261018/`. The focused provider case executes the actual PWM, PMIC, rail, pin and clock
cores through repeated operations and fault retention. A separate pin case
covers exact retained resume and uncertain transition writes. The single linked
image passes repeated command/IRQ/USB cases with modeled MMIO and shared service
replies, including command and idle expiry. The actual host producer, launcher,
fragment decoder and authority checks pass; initialization or command failure
prevents the next endpoint operation. These are software checks only.
Software-only authorities cannot authorize a physical USB target. A physical request needs current
image/DDR/instance authority, supply and pad ownership, known load permission,
and the reviewed full-power-removal procedure.

Other routed channels, constant endpoints, broader frequency/polarity profiles,
S_PWM output, complementary/dead-zone/group operation, capture and input
counting remain required modes. Controller support does not establish attached
accessories, actual component population or physical qualification.

## Source adaptation

The native driver preserves GPL terms for the register layout and ordering
adapted from pinned Orange Pi U-Boot `sunxi_pwm.c` and `sunxi_pwm.h`.
Its FDT/global synchronous lifecycle cannot enforce the required common
FreeRTOS owner, deadlines or retained failure state. No U-Boot or Linux runtime
is required. A733 manual sections 18.7.6.2 and 18.7.8.25 define cycle mode,
period readiness and enable ordering; section 18.14 defines GPIO behavior.
Electrical references are `A733_Datasheet_V0.93.pdf`, section 5.4/table 5-3,
schematic sheets 5–6 and the
[WNM2021-3/TR datasheet](https://www.lcsc.com/datasheet/C239556.pdf).
