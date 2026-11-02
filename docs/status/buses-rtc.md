# Buses, pins, RTC and watchdog

**Status: implemented.**

Native internal buses, GPIO, UART, SPI, PWM, ADC and RTC paths have software evidence. Exposed I2C resource bindings are missing.


**Board-qualified scope:** Inherited PMIC reads, RTC RC-mode progression,
watchdog reset, retained breadcrumbs and returning FEL execution have physical
records. These results do not qualify every exposed bus or reset scenario.

## Open work

1. Bind the exposed I2C controllers (TWI0-3, S_TWI2) to clocks, mutexes and resources.
2. Qualify the RTC alarms, clock switching, absolute time, calibration, wake and calendar retention across power modes.
3. Qualify UART, SPI and PWM electrically: wiring, timing, IRQs, DMA visibility and waveform.
4. Qualify ADC values, accuracy, IRQs and recovery; resolve the LRADC electrical block.
5. Audit the remaining board functions (LEDs, buttons, infrared, Ethernet and debug) against the wiring record.

Records for each area are in the sections below.

## Pins


**Board-qualified scope:** None.

Bank-qualified GPIO, mux, pull, drive, interrupt and debounce ownership is implemented. Header electrical behavior, IRQs and unresolved mappings are unqualified. See [pins](../pins.md).

## I2C

**Board-qualified scope:** The shared PMIC provider initializes, reads DCDC1,
reaps the request, releases the client lease and restores inherited bus and pins
in `pmic-provider-probe-20261018/physical/runtime/session.json`.
`sys-supply-probe-20261018/runtime/` records second-board read-only selectors.
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

SPI0-4 and S_SPI master FIFO paths and selected DMA are implemented. Peer wiring, IRQ, DMA and cache behavior, and recovery are unqualified. SPI4 activation refuses its unproven package mapping, and the manual supplies no SPI4 DMA request. See [SPI](../spi.md) and [platform](../spi-platform.md).

## RTC and watchdog

**Board-qualified scope:** Read-only inherited-RC observations show one second
of RTC progression in about 1.1 seconds in
`rtc-rc-readonly-20261018/physical/observation.json`. Watchdog expiry, retained
fault breadcrumbs and FEL recovery have evidence in `wdt-fel-return-20261031/`
and `dev-runs-1031/`.

Calendar setting, cold crystal initialization, non-waking alarms and a serialized two-client service are implemented. Returning RTC images need the current watchdog, breadcrumb and runner integration. See [RTC](../rtc-application.md), [alarm qualification](../../apps/rtc/qualification.md) and [watchdog](../watchdog.md).

## Remaining functions

**Board-qualified scope:** None.

Audit fitted LEDs, buttons, infrared, Ethernet, debug and other blocks against [board wiring](../board-wiring.md). Distinguish usable routes from unconnected SoC interfaces and optional population before claiming support.

## Evidence

Board results are records in `docs/evidence/buses-rtc.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-buses-rtc-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/buses-rtc.jsonl`.
<!-- evidence-table:end -->
