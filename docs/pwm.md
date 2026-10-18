# Native PWM fan application

The supervised PWM application drives the board's routed PWM0 channel 5 on PD5.
It programs a finite 40 kHz, 50% active-high train of 4,000 pulses, observes the
actual interrupt path, stops the waveform and transitions the still-owned pin to
GPIO output-low. Registers and copied USB diagnostics establish software state;
waveform, gate voltage, discharge time and actual fan behavior remain physical
qualification requirements. No physical operation has been attempted.

The [board wiring audit](board-wiring.md) is the canonical PWM/ADC routing and
population record. It covers the fan switch, status LED, exposed header routes,
optional display waveform, ADC selection straps, unconnected inputs and source
discrepancies. Its source hashes and rendered sheets are preserved under
`~/.cache/sunstoneos/evidence/pwm-adc-topology-20261018/`. The ADC worker reuses
that audit; this application does not implement or qualify ADC sampling.

## Ownership and operation

The complete session is hashed and privately sealed in EL3 before provider
construction. Its board prefix selects supervised workload 2: USB0 and the
shared PMIC worker, without radio or USB host. The PWM suffix binds distinct
owner/authority, initial-state, memory, power and supervision evidence, inherited
PD5 fields, exact APB0 clock profile and a bounded operation interval. There is
no raw-register or arbitrary pin/channel input. The native profile fixes period,
duty, polarity and pulse count.

The common clock registry reserves the entire PWM0 controller exclusively.
Channel 5 shares a clock pair with channel 4, and controller reset/global controls
cannot be assigned independently. Stable OSC, REF and fabric reservations protect
the inherited 24 MHz source and bus; an inherited APB0 PERI0 profile also reserves
that parent. The application never retunes a shared parent. The common rail and
PMIC services retain BLDO1 and verify its programmed 1.8 V value. Physical power
and source-qualified settling remain independent admission facts.

Cold admission excludes every PWM0, clock/reset, PD5, voltage and remote writer.
It requires a gated/reset controller, no inherited capture/group/complementary
mode, owned disabled/drained SPI67 and a reviewed fan circuit/load. Local gate
and reset setup occurs once; no post-fault reset or retry exists. The actual
native adapter checks EL1/task/interrupt context and common mutex ownership
before a restricted controller/register access.

The first profile uses SYS_CLK24M with pair divider 1 and channel prescaler 1.
A 25,000 ns period contains 600 ticks, so the period field is 599; the active
field is 300. Pulse field 3,999 selects 4,000 pulses, nominally 100 ms. Every
configuration write leaves PUL_START clear. Period readiness and plan readbacks
precede the final hardware start. Pulse completion requires PUL_START to clear.
The source planner rejects unsupported fractional ticks and duration/field
limits instead of silently rounding.

EL3 configures owned SPI67 as a primary-routed, level-high Group1NS interrupt
while it remains masked. It rejects unexplained pending or active state. The
ISR masks that SPI and publishes an atomic notification without entering PWM,
pin or power services. The owner task observes channel 5's status, disables its
interrupt enable and acknowledges only its W1C status bit. Subsequent finite
pulse completion is polled with the same absolute deadline; this avoids a
40 kHz interrupt stream.

Normal stop verifies pulse completion, clears and reads back PWM enable and
clock gating, then uses `a733_pins_pwm0_fan_low()` on the existing lease. The
fixed PD5 transition disconnects the alternate mux, preserves drive/pull and
neighbor fields, preloads the owned DATA clear alias, selects GPIO output and
verifies its low data bit. It neither releases nor reacquires ownership. A
failed or late write faults the pin manager and retains the token.

The transition uses a driven-low state. An input pull-down and digital-low
observation cannot establish Q2 off: GPIO input thresholds and weak pull/leakage
can leave a conducting gate voltage. At the admitted 1.8 V pad supply, the A733
input-low limit is 0.54 V; Q2's minimum threshold is 0.45 V at 250 microamps.
The A733 datasheet's GPIO table specifies output-low at most 0.2 V. Using
nominal R89 = 10 kilohms and Q2's 5 microamp maximum gate leakage gives a
50 mV series drop, or 0.25 V including that output-low limit. This design
estimate excludes resistor tolerance and transient gate charge. Threshold
voltage is not an off-current guarantee. Physical capture must establish gate
discharge, off-current and attached fan behavior before reporting fan-off.

## Failure, results and remaining modes

Cancellation and operation deadline revoke success before new USB queueing.
The driver retains clock, rail, pin and uncertain register ownership. It does
not poll a healthy core to manufacture a fault, reset the controller or retry
uncertain writes. An uncertain active output may require manual full power
removal; retained diagnostics never claim that cancellation switched the fan
off. A finite pulse counter does not recover stalled MMIO or guarantee an
analog inactive level after a fault.

Copied USB records preserve the plan, controller/counter/status registers,
interrupt observation, waveform-stop and GPIO-low facts, raw PMIC snapshot,
absolute deadline, owner state and errors. Pending/failure records remain
separate from success. Bytes already queued in TinyUSB cannot be retracted;
a cancelling host permanently discards that session's success and any partial
frame. Retained shared locks can stop USB too, so error transfer is not
guaranteed and RAM diagnostics remain owned.

Physical requests require current-instance image/DDR/topology authority,
actual supply/circuit/pad ownership, known fan/load permission and manual
recovery. If USB is the only supply, unplugging it removes board/card power;
additional supplies must also be removed. The coordinator verifies the new
instance after reconnect. This application never arms a watchdog or writes
persistent storage.

Other routed channels, continuous output, broader duty/polarity/rate profiles,
complementary/dead-zone/group operation, capture and input counting remain
required modes. They are not inferred from this fixed fan profile. The kernel
and manual route descriptions also do not establish attached accessories or
actual component population.

## Source adaptation

The register layout, pulse/cycle encoding and enable/disable ordering follow the
pinned Orange Pi U-Boot `sunxi_pwm.c` and `sunxi_pwm.h`, with their GPL terms
preserved in the native driver. The original FDT/global synchronous lifecycle
cannot enforce the common FreeRTOS resource owner, bounded deadlines or retained
failure state. The adapter retains the applicable hardware semantics while
adding those required interfaces. A733 manual sections 18.7 and 18.14 define
pulse completion, GPIO and atomic DATA clear behavior. No U-Boot or Linux runtime
is required by the image.

## Software validation and physical prerequisites

The focused provider check composes the actual PWM, PMIC, rail, clock and pin
cores. The retained-pin check uses the existing atomic DATA/mux model and
executes only the new fixed transition. Their cases cover the exact finite
plan, one-shot interrupt contention, normal stop, neighboring fields and
retention after uncertain writes or expiry.

The single native image links the complete board provider composition and
PWM task. Its instruction checks execute the actual task, native context
guards, register aperture, clock registry, ISR masking and USB staging. Shared
service replies, MMIO, time and scheduling are modeled. Cancellation, expiry
and pin-transition failure revoke success; the first masked IRQ notification
survives a busy register lock. The host check uses the actual producer, const
build identity, ARM launcher, sealed bundle, fragmented record and cancellation
path, and rejects every missing PWM authority fact. These checks establish
software behavior only.

The frozen image, exact compiled closure and results are under
`~/.cache/sunstoneos/evidence/pwm-native-20261018/`. The fixture identity is
explicitly software-only and cannot authorize a physical USB target. Future
physical admission needs actual current-instance authorities for the supply,
controller, pin, interrupt, circuit/loading and recovery facts. Fan attachment
and permitted operation require confirmation.

Electrical source references are the pinned `A733_Datasheet_V0.93.pdf`,
section 5.4/table 5-3, schematic sheet 5 and the
[WNM2021-3/TR manufacturer datasheet](https://www.lcsc.com/datasheet/C239556.pdf).
The retained-low transition uses A733 manual section 18.14; finite pulse,
period-ready, enable and clock-mask semantics use sections 18.7.8.25,
18.7.8.18 and 18.7.8.12.
