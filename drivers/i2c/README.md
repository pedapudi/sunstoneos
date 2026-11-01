# Header I2C bindings remain unimplemented

SunstoneOS has a bounded TWI transaction engine and native bindings for the
camera controllers TWI9 and TWI11. The separate Type-C adapter binds S_TWI1;
the PMIC uses S_TWI0. These paths do not establish native access to the four
main I2C controllers exposed at the 40-pin header.

The board manual V1.0, section 3.16.4, gives these header routes. The pinned
board device tree gives their SoC pins and a 400 kHz requested bus rate:

| Controller | Header SDA | Header SCL | SoC pin pair |
| --- | --- | --- | --- |
| TWI0 | 3 | 5 | PB2/PB3 |
| TWI1 | 38 | 40 | PB7/PB8 |
| TWI2 | 19 | 23 | PE1/PE2 |
| TWI3 | 26 | 21 | PE3/PE4 |

The device tree selects S_TWI2 on PL10/PL11 and declares a HYM8563 RTC child.
That declaration does not establish a fitted external RTC. The pin manager
also describes an S_TWI2 alternate function on PL4/PL5. A supported pin-table
alternative does not establish the fitted board route.

`a733_twi_init_controller` selects only TWI9, TWI11 or S_TWI1. Its native
camera adapter admits only TWI9 or TWI11 and reserves one camera-bus mutex.
The clock registry has no dedicated scope for TWI0–3 or S_TWI2. Completing
header I2C therefore needs shared clock scopes and serialized bus ownership,
followed by controller selection, native pin/clock bindings and a real caller.
A pin-table entry alone supplies none of those operations.

The camera adapter currently plans at most 100 kHz on both routes. The board
device tree requests 100 kHz for TWI9 and 400 kHz for TWI11. Higher-rate
qualification must resolve that difference and use an admitted peer. Neither
that device-tree setting nor the header table measures electrical timing.
New rate profiles require the coordinator's clock-point review before a run.

TWI1 shares PB7/PB8 with I2S0. TWI2 and TWI3 share PE1–4 with SPI3; PE1 also
belongs to the UART6 route. Native admission must preserve these pin conflicts,
exclude autonomous/DMA writers, establish pull-ups and bank voltage, and retain
ownership after an uncertain transaction. Electrical recovery cannot be
inferred from a software timeout.

The source records are `board-manual`, `board-schematic`, and
`board-gpu-power-sun60i-a733-orangepi-zero3w-dts` in
[the source lock](../../sources.lock.json). The manual and schematic are
publisher documents; the device tree is dual-licensed GPL/MIT Linux source at the recorded
revision. They remain host-side development references. No reference bytes
are added to the runtime by this inventory.
