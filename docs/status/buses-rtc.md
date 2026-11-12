# Buses, pins, RTC and watchdog

**Status: implemented.**

Native internal buses, GPIO, UART, SPI, PWM, ADC and RTC paths have software evidence. The exposed I2C controllers (TWI0 to TWI3 and S_TWI2) have a native class service with clock, reset, pin, interrupt and deadline bindings and host evidence; the board scan of all five controllers with nothing attached passes.

**Software evidence:** host (engine and clock words); linked OS image and host admission for the scan image.

**Board-qualified scope:** An empty-bus scan of TWI0-3 and S_TWI2 at 96 to 100 kHz and 385 to 400 kHz (open, 128 address probes, close, lines high). Inherited PMIC reads, RTC RC-mode progression,
watchdog reset, retained breadcrumbs and returning FEL execution have physical
records. These results do not qualify every exposed bus or reset scenario.

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Shared PMIC provider, DCDC1 read | qualified on the board | register observation only | `buses-rtc-pmic-dcdc1-read` |
| Exposed I2C controllers (TWI0-3, S_TWI2) | qualified on the board (empty bus) | 128 of 128 address NACKs on each of ten scans; 107 us per probe at 100 kHz | [design](../designs/header-i2c.md), [class interface](../../include/sunstone_i2c.h) |
| GPIO, mux, pull, drive, interrupt, debounce | host-tested | bank-qualified ownership | [pins](../pins.md) |
| UART (nine controllers; UART2 IRQ and DMA applications) | host-tested | | [UART](../uart.md) |
| SPI0-4 and S_SPI FIFO paths; SPI3 DMA command-response | host-tested | 4096-byte response; 65.792 ms of clocks at 500 kHz | [SPI](../spi.md), [native SPI DMA](../designs/spi-native-dma.md) |
| PWM (fan, LED) and GPADC0-3 | host-tested | LRADC electrically blocked | [PWM](../pwm.md), [ADC](../adc-application.md) |
| RTC RC-mode progression | qualified on the board | 1 s in about 1.1 s | `buses-rtc-rtc-rc-progression` |
| RTC calendar, crystal initialization, non-waking alarms | host-tested | | [RTC](../rtc-application.md) |
| RTC calendar set, one alarm and retention from the internal RC | qualified on the board (read after the alarm unrun) | alarm at target + 1 RTC s, 0.22 ms to finish; RTC second about 1.19 s | [design](../designs/rtc-rc-alarm.md) |
| Watchdog expiry, retained breadcrumbs, FEL recovery | qualified on the board | | `buses-rtc-watchdog-fel-return`; [watchdog](../watchdog.md) |
| LEDs, buttons, infrared, Ethernet, debug | open | | [board wiring](../board-wiring.md) |

## Open work

1. Qualify the exposed I2C controllers electrically with an attached peripheral: rise time, bank voltage, clock stretching and the 400 kHz margin. The empty-bus scan (see the evidence table) establishes the binding only.
2. Run the corrected `rtc-alarm` image once to qualify the calendar read after the alarm, then qualify crystal selection, calibration, wake and calendar retention across power removal. The RTC runs from the uncalibrated internal RC (about 1.19 s per RTC second); no 32.768 kHz accuracy is established.
3. Qualify UART, SPI and PWM electrically: wiring, timing, IRQs, DMA visibility and waveform.
4. Qualify ADC values, accuracy, IRQs and recovery; resolve the LRADC electrical block.
5. Audit the remaining board functions (LEDs, buttons, infrared, Ethernet and debug) against the wiring record.

The sections below hold each area's scope.

## Pins


**Board-qualified scope:** None.

Bank-qualified GPIO, mux, pull, drive, interrupt and debounce ownership is implemented. Header electrical behavior, IRQs and unresolved mappings are unqualified. See [pins](../pins.md).

## I2C

**Board-qualified scope:** The shared PMIC provider initializes, reads DCDC1,
reaps the request, releases the client lease and restores inherited bus and pins
(record `buses-rtc-pmic-dcdc1-read` in [buses-rtc.jsonl](../evidence/buses-rtc.jsonl)).
[pmic-supply-observation.jsonl](../evidence/pmic-supply-observation.jsonl) records second-board read-only selectors.
These are register observations rather than analog voltage measurements.

Header TWI0-3 and S_TWI2 are bound by `apps/os/i2c.c` and `drivers/i2c/a733_i2c_bus.c`: an application declares the controllers in its manifest, and the OS holds an exclusive clock scope and the two pins of each open controller, programs the rate from the live parent clock, and completes each transfer from the controller interrupt. See [the design](../designs/header-i2c.md) for the vendor sequence, the routes and the board scan prediction, and [the controller inventory](../../drivers/i2c/README.md) for the wiring evidence.

## UART


**Board-qualified scope:** None.

Native FIFO, IRQ and platform support covers nine controllers; UART2 has finite IRQ and DMA applications. Serial wiring, electrical timing, IRQs and DMA visibility are unqualified. UART1 conflicts with Bluetooth. See [UART](../uart.md).

## PWM and ADC


**Board-qualified scope:** None.

Fan and LED PWM, and GPADC0-3 divider observations with threshold windows, are implemented. Physical waveform, fan-off current, LED output, ADC value and accuracy, and recovery are unqualified. LRADC is electrically blocked: V1.2 R23 pulls toward the nominal 1.8 V AVCC while the converter range is 0-1.35 V. See [PWM](../pwm.md), [ADC](../adc-application.md) and [LRADC sources](../../apps/adc/lradc-sources.json).

## SPI


**Board-qualified scope:** None.

SPI0-4 and S_SPI master FIFO paths and selected DMA are implemented; the SPI3 command-response DMA application is host-tested ([design](../designs/spi-native-dma.md)). Peer wiring, IRQ, DMA and cache behavior, and recovery are unqualified. SPI4 activation refuses its unproven package mapping, and the manual supplies no SPI4 DMA request. See [SPI](../spi.md) and [platform](../spi-platform.md).

## RTC and watchdog

**Board-qualified scope:** Read-only inherited-RC observations show one second
of RTC progression in about 1.1 seconds (record `buses-rtc-rtc-rc-progression` in
[buses-rtc.jsonl](../evidence/buses-rtc.jsonl)). Watchdog expiry, retained
fault breadcrumbs and FEL recovery have evidence in the record
`buses-rtc-watchdog-fel-return` of the same file.

Calendar setting, cold crystal initialization, non-waking alarms and a serialized two-client service are implemented. Returning RTC images need the current watchdog, breadcrumb and runner integration. See [RTC](../rtc-application.md), [alarm qualification](../../apps/rtc/qualification.md) and [watchdog](../watchdog.md).

## Remaining functions

**Board-qualified scope:** None.

Audit fitted LEDs, buttons, infrared, Ethernet, debug and other blocks against [board wiring](../board-wiring.md). Distinguish usable routes from unconnected SoC interfaces and optional population before claiming support.

## Evidence

Board results are records in [`docs/evidence/buses-rtc.jsonl`](../evidence/buses-rtc.jsonl); add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-buses-rtc-20261101.md).

<!-- evidence-table:begin -->
9 records in `docs/evidence/buses-rtc.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `buses-rtc-pmic-dcdc1-read` | 2026-10-18 |  | `` | returned | passed |  |
| `buses-rtc-rtc-rc-progression` | 2026-10-18 |  | `` | returned | passed | rtc_second_duration 1.1 s |
| `buses-rtc-watchdog-fel-return` | 2026-10-31 |  | `` | watchdog-reset | passed |  |
| `1794478857548541189-7cb7b581` | 2026-11-12 | 3-3 | `693b6d5a51bf` | watchdog-reset | inconclusive | run_total 20.654 s |
| `1794482316080920295-1fa0e7b5` | 2026-11-12 | 3-3 | `3d5cda73c6ec` | returned | passed | run_total 22.727 s |
| `1794482360148455232-b0b0b849` | 2026-11-12 | 3-3 | `6af5ee3fa083` | returned | passed | run_total 22.802 s |
| `1794482394892578429-4199afcd` | 2026-11-12 | 3-3 | `576462699622` | host-error | passed | run_total 23.166 s, rate_100k_main 96153 Hz, rate_400k_main 384615 Hz, probe_100k_main 107 us, probe_400k_main 40 us, scan_100k 15.6 ms, scan_400k 7.1 ms, interrupts_per_scan 256 |
| `1794482702632861790-8e32fccf` | 2026-11-12 | 3-3 | `e9110e0c556f` | returned | failed | run_total 27.101 s, alarm_wait 4 s, alarm_latency 0.22 ms |
| `1794482776569788451-05dabdc7` | 2026-11-12 | 3-3 | `427f6502a73c` | returned | failed | run_total 27.822 s, alarm_wait 4.68 s, calendar_advance 59 s, rtc_second_estimate 1.19 s, alarm_latency 0.22 ms |
<!-- evidence-table:end -->
