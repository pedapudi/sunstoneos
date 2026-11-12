# Header I2C: the controllers, the class and the board scan

**Status: implemented and host-tested; board run pending.**

This document describes how an application reaches the five two-wire controllers that the 40-pin header exposes: TWI0, TWI1, TWI2, TWI3 and S_TWI2. It states the vendor sequence for bringing up one controller, the differences between that sequence and the OS service, and the board scan that qualifies the binding with nothing attached.

## Controllers, pins and interrupts

All five controllers have the register map of the A733 User Manual V1.00 section 18.11.9. They differ in the data below, which is the per-part table in `drivers/i2c/a733_i2c_bus.c`.

| Controller | Base | Gate and reset word | Parent | Interrupt (INTID) |
| --- | --- | --- | --- | --- |
| TWI0 | `0x02510000` | CCU `0xe80`, gate bit 0, reset bit 16 | APB1 | SPI 11 (43) |
| TWI1 | `0x02511000` | CCU `0xe84`, gate bit 0, reset bit 16 | APB1 | SPI 12 (44) |
| TWI2 | `0x02512000` | CCU `0xe88`, gate bit 0, reset bit 16 | APB1 | SPI 13 (45) |
| TWI3 | `0x02513000` | CCU `0xe8c`, gate bit 0, reset bit 16 | APB1 | SPI 14 (46) |
| S_TWI2 | `0x07085000` | R_CCU `0x19c`, gate bit 2, reset bit 18 | APBS1 | SPI 205 (237) |

The base addresses and interrupts come from the pinned board kernel device tree (`sun60iw2p1.dtsi`, nodes `twi0` to `twi3` and `s_twi2`). The gate and reset words come from the pinned clock drivers (`ccu-sun60iw2.c` and `ccu-sun60iw2-r.c`) and the manual, sections 4.1.6.147 to 4.1.6.150.

The board description (`drivers/board/orangepi_zero3w.c`) routes each controller to the header:

| Controller | SCL pin (header) | SDA pin (header) | Function |
| --- | --- | --- | --- |
| TWI0 | PB2 (5) | PB3 (3) | 8 |
| TWI1 | PB7 (40) | PB8 (38) | 8 |
| TWI2 | PE1 (23) | PE2 (19) | 2 |
| TWI3 | PE3 (21) | PE4 (26) | 2 |
| S_TWI2 | PL4 (28) | PL5 (27) | 3 |

The function numbers are the manual's multiplexing tables for ports B, E and L. The header positions are from the board manual V1.0 section 3.16.4 and the V1.2 schematic sheet 17, which names the nets `EXT_TWI0_SCK`, `EXT_TWI0_SDA` and `S-TWI2`. The pinned device tree places S_TWI2 on PL10 and PL11; the schematic routes those pins to the Type-C sideband signals, and its header pins 27 and 28 use PL5 and PL4, so the board description follows the schematic. TWI2 and TWI3 share PE1 to PE4 with SPI3, and TWI1 shares PB7 and PB8 with I2S0; the pin manager refuses a second owner of a pin.

## The class interface

`include/sunstone_i2c.h` is the application interface. An application declares the controllers in an `i2c` section of its `manifest.json`, each with the highest rate it needs. The OS grants a declared controller when the board description routes it to the header; a controller that the manifest does not declare is never touched, and the OS image routes the interrupt of a declared controller only.

Calls return at once. The application opens a bus at a rate, submits a transfer (a list of messages ending with one STOP), and collects the result. The caller keeps the messages and their buffers valid until the result is collected. One transfer per bus is in flight.

Completion is interrupt-driven end to end. The application's submit wakes the OS owner task, which writes START under a kernel critical section. The controller interrupt then runs the engine (`drivers/i2c/a733_i2c_bus.c`) for each protocol step: it writes the next byte or reads the received one, and clears the interrupt flag. The interrupt that ends the last step publishes the result and wakes the application task directly. No task runs between the last interrupt and the application, and nothing polls on the tick. Every transfer has a deadline: the owner sleeps on the precise timer and, at the deadline, requests STOP and reports a timeout.

Serialization is by ownership. The OS owner task holds an exclusive clock scope for each open controller (`A733_CLOCK_TWI0` to `A733_CLOCK_TWI3`, `A733_CLOCK_S_TWI2`) and a pin lease for its two pins, so no other owner can use the controller or its pins; the single in-flight transfer is the only user. The gate and reset word of S_TWI2 is shared with S_TWI0 and S_TWI1, so every write to it holds the common register lock.

## Vendor sequence and the diff of our code

The manual, section 18.11.6.1, gives the bring-up of a controller, and sections 18.11.6.2 and 18.11.6.3 give the write and read sequences. The board kernel's `twi` driver is not part of the pinned inputs, so the sequence below is the manual's.

| Step | Manual | Service |
| --- | --- | --- |
| 1 | Select the pin function. | The pin manager acquires the two pins with the SoC pull-up and drive level 0 (the device tree's 10 mA), with the inherited configuration as the expected state. The pull-up is required: the board holds no external pull-up for these nets in the record. |
| 2 | Clear the gate in the bus gating register. | The service requires gate and reset both clear before it starts. A controller that is already enabled is refused, not reset. |
| 3 | Clear the reset bit, then set it. | The reset bit is set once, from the cleared state, and the service waits 2 us. |
| 4 | Set the gate. | Set after the reset bit, with readback. |
| 5 | Program `TWI_CCR` from the APB1 input (`Fscl = Fin / (2^N * (M + 1) * 10)`). | The same formula, with the parent decoded from the live APB1 or APBS1 register (24 MHz oscillator, or PERI0 for APB1) and held by the clock lease. The service picks the highest rate not above the request and sets the 30% duty bit above 100 kHz. The manual's example for 400 kHz uses N = 1 and M = 2; the service takes the divisor with the smallest N, which gives the same rate. |
| 6 | Set `BUS_EN` and `A_ACK`; with interrupts set `INT_EN`. | The service sets `BUS_EN` and `INT_EN`, and sets `A_ACK` per byte as the manual's read sequence requires. |
| write | START; the first interrupt; device ID; each interrupt, the next byte; STOP. | The same, one interrupt per step. A zero-length write sends the address only. |
| read | START; device ID; repeated START; clear `A_ACK` at the byte before the last; STOP. | The same. |
| NACK | Not specified; status 0x20, 0x30 and 0x48 report it. | STOP is sent after every address or data NACK and after a bus error, and not after lost arbitration (0x38). |

Differences that matter for the board run:

- The service runs the soft reset (`TWI_SRST`) after the gate is open and waits for it to clear; the manual's bring-up does not list it. The S_TWI1 diagnostic (`apps/usb-vbus`) does the same.
- The service reads the lines (`TWI_LCR`, bits 4 and 5) after open and before close and requires both high. These are digital readings; they establish neither rise time nor the pull-up value.
- STOP raises no interrupt, so the service reports a transfer when it requests STOP, and the next START is hardware-ordered after it. The service checks that the bus is idle (`TWI_STAT` is 0xf8, the STOP and START bits are clear and both lines are high) before each start and counts the starts that find it busy, and it requires idle before the gate closes.
- The pin bank supply (VCC-PB, VCC-PE, VCC-PL) is inherited. The service does not read or change it.

## Board scan with nothing attached

The application `apps/i2c-scan` opens each controller in turn at 100 kHz and at 400 kHz, probes the 128 seven-bit addresses (0x00 to 0x7f) with a zero-length write, and closes the controller. The decoder `apps/i2c-scan/host.py` lists every departure from the prediction.

**Prediction.** With no device attached, every probe ends with status 0x20 (address and write sent, not acknowledged) and the engine's `NACK_ADDRESS` result; no probe is acknowledged; each scan takes 256 controller interrupts (the START interrupt and the address interrupt for each probe); the lines read high after open and after close; the interrupt counters show no spurious or stray interrupt; every start finds the bus idle. The controller interrupts are INTIDs 43 to 46 and 237.

**Timing.** At 100 kHz, one address byte and its acknowledge slot take nine clocks, 90 us. With the START and STOP conditions (about 10 us together) and two interrupt services (about 5 us each), a probe measured from the START request to the last interrupt is expected between 105 and 135 us. At 400 kHz the nine clocks take 22.5 us and a probe is expected between 35 and 60 us. The application adds a wake of the owner task and a wake of itself for each probe, about 40 to 80 us together; a whole scan is expected between 17 and 30 ms at 100 kHz and between 8 and 20 ms at 400 kHz. The decoder reports the mean, minimum and maximum probe times and the scan time.

**Time budget.** The session lifetime is 60 s, the finish bound 55 s and the progress window 3 s. Startup and admission take at most about 1.2 s. Opening a controller takes under 1 ms, closing under 1 ms. The ten scans take about 0.25 s in total. If every probe instead ran to its 5 ms deadline, a scan would take 0.64 s and the ten scans 6.4 s, which is under the manifest's 10 s run bound, and the application reports progress after each scan, at most 0.64 s apart. An overrun therefore ends in a decoded timeout record, not a watchdog reset.

**What the scan does not establish.** It establishes the binding: clock, reset, pin function, pull-up, interrupt and engine, and that no address answers. It does not establish the electrical timing (rise and fall times, the bus capacitance), the bank voltage, behavior with a device that stretches the clock, or the 400 kHz margin with a real pull-up. Those need an attached peripheral and a logic analyzer.
