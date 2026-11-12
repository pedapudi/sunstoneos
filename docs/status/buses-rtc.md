# Buses, pins, RTC and watchdog

**Status: implemented.**

Native internal buses, GPIO, UART, SPI, PWM, ADC and RTC paths have software evidence. The exposed I2C controllers (TWI0 to TWI3 and S_TWI2) have a native class service with clock, reset, pin, interrupt and deadline bindings and host evidence; the first board scan did not return a result.

**Software evidence:** host (engine and clock words); linked OS image and host admission for the scan image.

**Board-qualified scope:** Inherited PMIC reads, RTC RC-mode progression,
watchdog reset, retained breadcrumbs and returning FEL execution have physical
records. These results do not qualify every exposed bus or reset scenario.

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Shared PMIC provider, DCDC1 read | qualified on the board | register observation only | `buses-rtc-pmic-dcdc1-read` |
| Exposed I2C controllers (TWI0-3, S_TWI2) | host-tested | interrupt-driven class service; the board scan is unqualified | [design](../designs/header-i2c.md), [class interface](../../include/sunstone_i2c.h) |
| GPIO, mux, pull, drive, interrupt, debounce | host-tested | bank-qualified ownership | [pins](../pins.md) |
| UART (nine controllers; UART2 IRQ and DMA applications) | host-tested | | [UART](../uart.md) |
| SPI0-4 and S_SPI FIFO paths; SPI3 DMA command-response | host-tested | 4096-byte response; 65.792 ms of clocks at 500 kHz | [SPI](../spi.md), [native SPI DMA](../designs/spi-native-dma.md) |
| PWM (fan, LED) and GPADC0-3 | host-tested | LRADC electrically blocked | [PWM](../pwm.md), [ADC](../adc-application.md) |
| RTC RC-mode progression | qualified on the board | 1 s in about 1.1 s | `buses-rtc-rtc-rc-progression` |
| RTC calendar, crystal initialization, non-waking alarms | host-tested | | [RTC](../rtc-application.md) |
| Watchdog expiry, retained breadcrumbs, FEL recovery | qualified on the board | | `buses-rtc-watchdog-fel-return`; [watchdog](../watchdog.md) |
| LEDs, buttons, infrared, Ethernet, debug | open | | [board wiring](../board-wiring.md) |

## Open work

1. Qualify the exposed I2C controllers on the board. The first scan (`buses-rtc` record `1794478857548541189-7cb7b581`) ended without a result: the CPU clock owner refused a step during the return. The cause is not identified. A control run of the hello-compute image built from this branch separates a defect in the shared changes (interrupt-record sizing, clock scopes, the service start entry) from the scan application; an application that waits for `ss_clock_ready()` before finishing, as the other CPU-return applications do, is the second discriminator.
2. Qualify the RTC alarms, clock switching, absolute time, calibration, wake and calendar retention across power modes. The board runs the RTC from the internal RC oscillator (source control 0x401c); the alarm opener and the calendar writer accept only the external 32.768 kHz source, so qualifying alarms and absolute time without changing the clock source needs a reviewed RC-source mode in `drivers/rtc/a733_rtc.c` and `drivers/rtc/a733_rtc_initialize.c`.
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
4 records in `docs/evidence/buses-rtc.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `buses-rtc-pmic-dcdc1-read` | 2026-10-18 |  | `` | returned | passed |  |
| `buses-rtc-rtc-rc-progression` | 2026-10-18 |  | `` | returned | passed | rtc_second_duration 1.1 s |
| `buses-rtc-watchdog-fel-return` | 2026-10-31 |  | `` | watchdog-reset | passed |  |
| `1794478857548541189-7cb7b581` | 2026-11-12 | 3-3 | `693b6d5a51bf` | watchdog-reset | inconclusive | run_total 20.654 s |
<!-- evidence-table:end -->
