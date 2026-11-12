# Buses, pins, RTC and watchdog

**Status: implemented.**

Native internal buses, GPIO, UART, SPI, PWM, ADC and RTC paths have software evidence. Exposed I2C resource bindings are missing.

**Board-qualified scope:** Inherited PMIC reads, RTC RC-mode progression,
watchdog reset, retained breadcrumbs and returning FEL execution have physical
records. These results do not qualify every exposed bus or reset scenario.

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Shared PMIC provider, DCDC1 read | qualified on the board | register observation only | `buses-rtc-pmic-dcdc1-read` |
| Exposed I2C controllers (TWI0-3, S_TWI2) | open | no clock, mutex or resource bindings | [controller inventory](../../drivers/i2c/README.md) |
| GPIO, mux, pull, drive, interrupt, debounce | host-tested | bank-qualified ownership | [pins](../pins.md) |
| UART (nine controllers; UART2 IRQ and DMA applications) | host-tested | | [UART](../uart.md) |
| SPI0-4 and S_SPI FIFO paths; SPI3 DMA command-response | host-tested | 4096-byte response; 65.792 ms of clocks at 500 kHz | [SPI](../spi.md), [native SPI DMA](../designs/spi-native-dma.md) |
| PWM (fan, LED) and GPADC0-3 | host-tested | LRADC electrically blocked | [PWM](../pwm.md), [ADC](../adc-application.md) |
| RTC RC-mode progression | qualified on the board | 1 s in about 1.1 s | `buses-rtc-rtc-rc-progression` |
| RTC calendar, crystal initialization, non-waking alarms | host-tested | | [RTC](../rtc-application.md) |
| Watchdog expiry, retained breadcrumbs, FEL recovery | qualified on the board | | `buses-rtc-watchdog-fel-return`; [watchdog](../watchdog.md) |
| LEDs, buttons, infrared, Ethernet, debug | open | | [board wiring](../board-wiring.md) |

## Open work

1. Bind the exposed I2C controllers (TWI0-3, S_TWI2) to clocks, mutexes and resources.
2. Qualify the RTC alarms, clock switching, absolute time, calibration, wake and calendar retention across power modes.
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

Header TWI0-3 and S_TWI2 lack native clock, mutex and resource bindings. See [the controller inventory](../../drivers/i2c/README.md).

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
3 records in `docs/evidence/buses-rtc.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `buses-rtc-pmic-dcdc1-read` | 2026-10-18 |  | `` | returned | passed |  |
| `buses-rtc-rtc-rc-progression` | 2026-10-18 |  | `` | returned | passed | rtc_second_duration 1.1 s |
| `buses-rtc-watchdog-fel-return` | 2026-10-31 |  | `` | watchdog-reset | passed |  |
<!-- evidence-table:end -->
