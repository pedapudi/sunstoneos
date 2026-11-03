# Radio power sequence and supply topology

Status: **host-tested; the board has not run it.** The software follows the
vendor sequence below. No rail has been enabled on a board by SunstoneOS.

This document is for driver developers and the board coordinator. It states
which supplies power the AIC8800D80 module, the order in which the vendor
kernel brings them up, and how the native radio platform
(`drivers/net/a733_radio_platform.c`) reproduces that order.

## The module needs VBAT, I/O power and two control lines

Schematic V1.2 sheet 14 powers module U5 from two nets. `VCC-3V3-WIFI` feeds the
module's VBAT pin through a ferrite bead. `VCCIO_1V8_WIFI` feeds its VDDIO pin
and the pull-ups of the six SDIO lines. Sheet 6 lists `VCC-3V3-WIFI` among the
loads of DCDC1, whose selector the board reports as code 23, which is 3.3 V
(`docs/power.md`). The control lines are PM1 (`WL_REG_ON`, active high) and PM0
(`WL_HOST_WAKE`). PM2 is the shared Bluetooth reset and low-power-oscillator net.

## The vendor kernel enables two 1.8 V supplies, then PM1

The board device tree (`sun60i-a733-orangepi-zero3w.dts`, `&rfkill`) gives the
`wlan` node `wlan_power = "axp8191-bldo5", "axp8191-cldo1"` with
`wlan_power_vol = <1800000>, <1800000>`, annotated as the supplies of the PG and
PM pin banks, and `wlan_regon = PM1`. The `bt` node names the same two supplies.
Neither node names DCDC1, DCDC8 or any other output. The pinned
`sunxi-wlan.c` and the AIC8800 board support code (`aicsdio.c`) give this order:

1. Probe: configure PM1 as an output and drive it low. The Bluetooth helper
   likewise configures PM2 as an output and drives it low.
2. The SDC1 host driver applies its pin state: PG0 to PG5 on function 2 with
   pull-ups, 20 mA drive on PG0 and 10 mA on PG1 to PG5.
3. `aicbsp_platform_power_on()` clears the host's DDR50 capability, then calls
   `sunxi_wlan_set_power(1)`.
4. For BLDO5, then CLDO1: set the voltage to 1.8 V, read it back and fail if it
   differs, then enable the output. The device tree gives both outputs a 1 ms
   enable ramp delay. CLDO1 is `regulator-boot-on` and `regulator-always-on`;
   BLDO5 has neither property, so it is off until this step.
5. Wait 10 ms, then drive PM1 high.
6. Wait 50 ms, then rescan the SDIO bus (CMD5, CMD3, CMD7 and the common
   register reads).

DCDC1 stays enabled throughout. The kernel never touches it for the radio.

## DCDC8 is not a radio supply

The earlier documents named DCDC8 as the schematic's radio I/O supply. The
schematic does not support that. Sheet 2 (power sequence) lists DCDC8 as
"VDD-DRAM (reserved)", 1.8 V. Sheet 6 gives the DCDC8 output a net named
`DCDC8` that no other sheet uses. Sheet 2 lists CLDO1 as the supply of
`VCC-PM`, `VCC33-18-USB` and `VDD18-LPDDR`. The vendor sequence above never
programs or reads DCDC8. In the vendor device tree DCDC8 supplies the UFS
VCCQ pins and one I2C controller's pad power.

The disputed DCDC8 selector (code 100, 1.8 V in U-Boot and 3.1 V in the Linux
table) therefore does not decide radio admission. The BLDO5 and CLDO1 selectors
are not disputed: U-Boot and Linux both define `500 mV + 100 mV * code` on the
mask `0x1f`, so 1.8 V is code 13 (`0x0d`).

Two facts remain unproven by the sources. Sheet 14 does not draw the source of
`VCCIO_1V8_WIFI`: the name appears only on that sheet. And the schematic names
no supply for the `VCC-PG` net, while the device tree annotates BLDO5 as
`vcc-pg`. The most consistent reading is BLDO5 for PG and CLDO1 for PM, with
`VCCIO_1V8_WIFI` on one of the two. Both are enabled in the vendor sequence, so
the choice does not change what the sequence does. The board observation below
separates the readings.

## One read-only observation separates the readings

`GPIO_POW_VAL` at `0x02000048` reports the voltage that the SoC detects on the
`VCC-PG` pin. Bits 13:12 are `10` for 3.3 V, `11` for 1.8 V and `00` or `01`
when the pin is not supplied (A733 User Manual V1.00, section 18.14.6.2).
`GPIO_POW_MOD_SEL` at `0x02000040` bits 13:12 reports the selected mode.
PM is in the always-on pin block with its own pair of registers
(`SGPIO_POW_VAL`, `SGPIO_POW_MOD_SEL`). A run that reads these two words, plus
the BLDO5, CLDO1 and DCDC8 enable and selector bytes through the PMIC provider,
writes nothing and predicts:

| Reading | PMIC bytes at handoff | PG detected voltage |
| --- | --- | --- |
| BLDO5 supplies PG (vendor annotation) | BLDO5 off | `00` or `01`, not supplied |
| PG is already on another 1.8 V rail | BLDO5 off or on | `11` |
| PG is on DCDC8 at the Linux-table 3.1 V | DCDC8 code 100 | `10`, 3.3 V |

The finite observer `apps/power-supply-observe` already reads the PMIC bytes;
the pin-block words come from the existing read-only bundle reader
(`apps/board/radio-facts.json`).

## The native sequence follows the vendor order

`a733_radio_platform_poll()` runs these steps, with the same 10 ms and 50 ms
intervals:

1. Acquire clocks and a rail lease: BLDO5 exclusive, DCDC1 and CLDO1 shared.
2. Observe DCDC1, BLDO5 and CLDO1 through the PMIC provider. DCDC1 and CLDO1
   must be enabled at 3.3 V and 1.8 V. BLDO5 must be enabled at 1.8 V, or off.
   An enabled BLDO5 at any other voltage refuses; nothing is written.
3. Mask the SDC1 interrupt, then acquire the pins. PM1 is driven low. PG0 to PG5
   take function 2.
4. If BLDO5 was off, `a733_pmic_submit_power_on()` writes its selector (code 13,
   bits 7:5 preserved), reads it back, sets bit 2 of register `0x21` and reads
   that back. It refuses without any write if BLDO5 is already on.
5. Observe all three supplies again. Every one must now be enabled at its
   voltage.
6. Wait 10 ms, drive PM1 high, wait 50 ms, then enable the SDC1 module clock and
   enumerate.

Only BLDO5 can be powered on, only to 1.8 V, only under an exclusive lease.
CLDO1 also feeds the LPDDR 1.8 V rail, so the provider never writes it.

## Differences that remain from the vendor sequence

- PM2 is left as inherited. The vendor drives it low as an output at probe.
- The vendor clears the host's DDR50 capability. The native host has no UHS mode
  yet.
- The vendor requests the PM0 interrupt as a wake source. The native platform
  samples PM0 by polling.
- The vendor never configures `GPIO_POW_MOD_SEL` for PG; it keeps the value that
  the boot firmware left. The native platform does not write it either.
- The native platform starts SDIO identification at one bit and 400 kHz and
  widens to four bits at 25 MHz. The vendor ends at 208 MHz.
