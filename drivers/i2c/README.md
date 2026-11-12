# Header I2C controllers

SunstoneOS has a bounded polled TWI transaction engine and native bindings for the
camera controllers TWI9 and TWI11. The separate Type-C adapter binds S_TWI1; the
PMIC uses S_TWI0. The four main controllers TWI0 to TWI3 and S_TWI2, which the
40-pin header exposes, have a separate interrupt-driven engine
(`a733_i2c_bus.c`) and an OS class service (`apps/os/i2c.c`); the
[design](../../docs/designs/header-i2c.md) describes them, and the application
interface is `include/sunstone_i2c.h`. The wiring evidence below remains the
source of the routes in the board description.

The board manual V1.0, section 3.16.4, gives these header routes. The pinned
board device tree gives their SoC pins and a 400 kHz requested bus rate:

| Controller | Header SDA | Header SCL | SoC pin pair (SCL, SDA) |
| --- | --- | --- | --- |
| TWI0 | 3 | 5 | PB2/PB3 |
| TWI1 | 38 | 40 | PB7/PB8 |
| TWI2 | 19 | 23 | PE1/PE2 |
| TWI3 | 26 | 21 | PE3/PE4 |

The device tree selects S_TWI2 on PL10/PL11 and declares a HYM8563 RTC child.
That declaration does not establish a fitted external RTC. The V1.2 schematic
routes PL10/PL11 to the Type-C sideband pins and routes S_TWI2 to header pins
27 (SDA) and 28 (SCL) on PL5/PL4 with function 3, which is the route the board
description uses.

TWI1 shares PB7/PB8 with I2S0. TWI2 and TWI3 share PE1–4 with SPI3; PE1 also
belongs to the UART6 route. The pin manager preserves these conflicts. The service refuses a controller whose
gate or reset is already open, requires the engine registers idle, enables the SoC
pull-up, and retains the controller after an uncertain close. Bank voltage and
the pull-up value remain unestablished, and electrical recovery cannot be
inferred from a software timeout.

The source records are `board-manual`, `board-schematic`, and
`board-gpu-power-sun60i-a733-orangepi-zero3w-dts` in
[the source lock](../../sources.lock.json). The manual and schematic are
publisher documents; the device tree is dual-licensed GPL/MIT Linux source at the recorded
revision. They remain host-side development references. No reference bytes
are added to the runtime by this inventory.
