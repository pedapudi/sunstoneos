# Buses, pins, RTC and watchdog: historical status record 2026-11-01

This log holds the prose evidence of [the current status](../status/buses-rtc.md) as it stood on the date above. It is not updated. Board results that have queue entries are in the records `docs/evidence/buses-rtc.jsonl`.

**Status: implemented.**

Native internal buses, GPIO, UART, SPI, PWM, ADC and
RTC paths have software evidence. Exposed I2C resource bindings remain missing.

**Board-qualified scope:** Inherited PMIC reads, RTC RC-mode progression,
watchdog reset, retained breadcrumbs and returning FEL execution have physical
records. These results do not qualify every exposed bus or reset scenario.

Evidence paths below are relative to `~/.cache/sunstoneos/evidence/` unless
an absolute build path is given.

## Pins

**Board-qualified scope:** None.

Bank-qualified GPIO/mux/pull/drive/interrupt/debounce ownership is implemented.
Normal OS PM-output priming and stricter inherited diagnostic latches are
separate contracts. Header electrical behavior, IRQs and unresolved mappings
remain unqualified. See [pins](../pins.md).

## I2C

**Board-qualified scope:** The shared PMIC provider initializes, reads DCDC1,
reaps the request, releases the client lease and restores inherited bus/pins in
`pmic-provider-probe-20261018/physical/runtime/session.json`.
`sys-supply-probe-20261018/runtime/` records second-board read-only selectors.
These are register observations rather than analog voltage measurements.

Header TWI0–3 and S_TWI2 lack native clock/mutex/resource bindings. Internal
PMIC, Type-C and camera clients do not cover them.
[The controller inventory](../../drivers/i2c/README.md) records routes and gaps.

## UART

**Board-qualified scope:** None.

Native FIFO/IRQ/platform support covers nine controllers. UART2 has finite IRQ
and DMA applications. The DMA profile requires stopped channels, disabled
requests, TEMT and RX idle before matched teardown. Uncertain stops retain
ownership; the DMA0 parent lease and allocation stay reserved after successful
UART teardown. `uart-native-20261018/`,
`uart-irq-20261019/root-integration.json` and
`uart-dma-20261019/root-integration.json` model those paths. Serial wiring,
electrical timing, IRQs and DMA visibility remain unqualified. UART1 conflicts
with Bluetooth. RS-485/9-bit claims differ between manual and selected
`uart-v100` data; other routes/modes lack a consumer. See [UART](../uart.md).

## PWM and ADC

**Board-qualified scope:** None.

PD5/PWM0 channel-5 fan and optional PL7 LED support stop/restart and retained
faults. Physical waveform, fan-off current and LED output remain unqualified;
an uncertain failure may leave output active. `pwm-service-20261018/` and
`pwm-single-core-20261018/` record modeled scope. S_PWM lacks a clock-owner
scope; remaining channels and capture/counting remain open.

GPADC0–3 divider observations and threshold windows are implemented; GPADC4
reaches the display connector. Physical value/accuracy, IRQ and recovery tests
remain open. Evidence is in `adc-application-20261018/`,
`adc-compare-20261028/` and `adc-returning-final-20261018/`.
LRADC is electrically blocked: V1.2 R23 pulls toward nominal 1.8 V AVCC while
the converter range is 0–1.35 V. Fitted population, idle-overrange authority and
key ladder remain unresolved. `lradc-20261020/root-integration.json` is modeled.
See [PWM](../pwm.md), [ADC](../adc-application.md) and
[LRADC sources](../../apps/adc/lradc-sources.json).

## SPI

**Board-qualified scope:** None.

SPI0–4/S_SPI master FIFO paths and selected DMA are implemented. The SPI3
1.8 V PE0–3 application supplies deferred IRQ and command-plus-4096-byte DMA
receive. Evidence is in `spi-irq-20261020/root-integration.json` and
`spi-dma-native-1026/final-snapshot/`. Peer/wiring, IRQ, DMA/cache and recovery
remain unqualified. SPI4 activation refuses its unproven package mapping, and
the manual supplies no SPI4 DMA request. Platform quad is refused without
IO2/IO3 routes. No fitted SPI storage is
established. See [SPI](../spi.md) and [platform](../spi-platform.md).

## RTC and watchdog

**Board-qualified scope:** Read-only inherited-RC observations show one second
of RTC progression in about 1.1 seconds in
`rtc-rc-readonly-20261018/physical/observation.json`. Watchdog expiry, retained
fault breadcrumbs and FEL recovery have evidence in `wdt-fel-return-20261031/`
and `dev-runs-1031/`.

Calendar setting, cold crystal initialization, non-waking alarms and a serialized
two-client service are implemented. Stale/duplicate notifications are refused
before callbacks; focused evidence is in
`~/.cache/sunstoneos/build/buses-rtc-1031/`. Physical alarms, clock switching,
absolute time, calibration and wake remain unqualified. Calendar retention
across applicable power modes remains unqualified. Returning RTC
images need current watchdog/breadcrumb/runner integration. SNTP is not composed
with RTC. Autonomous cold-boot expiry and broader reset recovery remain separate
unqualified requirements. See [RTC](../rtc-application.md),
[alarm qualification](../../apps/rtc/qualification.md) and [watchdog](../watchdog.md).

## Remaining functions

**Board-qualified scope:** None.

Audit fitted LEDs, buttons, infrared, Ethernet/debug and other blocks against
[board wiring](../board-wiring.md). Distinguish usable routes from unconnected
SoC interfaces and optional population before claiming support.
