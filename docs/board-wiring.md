# Board wiring and physical recovery

The manufacturer publishes a V1.2 schematic and component-position drawing,
plus a V1.0 user manual. Their hashes and download URLs are pinned in
[sources.lock.json](../sources.lock.json). These describe the documented board;
the attached board's revision and optional component population still need
physical confirmation. Wiring evidence does not establish driver functionality.

The [manufacturer's support page](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/service-and-support/Orange-Pi-Zero-3W.html)
links the [schematic folder](https://drive.google.com/drive/folders/199S7SkbqVDjMpPT7tkcbBX6kmbfnz94r)
and [manual folder](https://drive.google.com/drive/folders/1ExSLqUoKxfNj2n8demccJj07aOQpTBW1).
The support page was retrieved over HTTP because its HTTPS endpoint was
unreachable. Downloads remain outside Git under
`~/.cache/sunstoneos/sources/board-documents/`.

Fetch or verify the pinned copies with:

```sh
python3 tools/fetch-sources.py board-schematic board-position board-manual
```

## BOOT and GND provide a physical FEL input

The board has test pads rather than a BOOT pushbutton. Connecting BOOT to GND
requires a physical contact fixture or removable wire connection. This is a
preparation step for the protected watchdog trial, not a requirement for normal
USB reconnection or software integration. Do not attach anything until the actual
board's pad layout is identified and power is disconnected.

The [manual](https://drive.google.com/file/d/1R1MC9erKTztd87GvJ_UFE132x6KgPATe/view),
section 2.9, printed pages 34–35 (PDF pages 41–42), instructs connecting the
BOOT and GND test points before applying USB power to enter FEL. FEL is the
SoC ROM's USB recovery service. Printed page 35 shows the pads beside the
mini-HDMI connector, near its adjacent mounting hole. BOOT is nearest the
HDMI connector; GND is the next pad toward the board edge. The third pad is
PWRON in the position drawing and is not part of the BOOT-to-GND connection.

Schematic sheet 5 connects BOOT through resistor R84, 1 kΩ, to the FEL net.
Capacitor C267, 1 nF, connects BOOT to ground. Sheet 8 connects FEL to SoC
ball 1W1. The A733 datasheet identifies that ball as an input with a pull-up.
This is a dedicated boot input, separate from the power-on input.

The [captured ROM analysis](watchdog.md) shows that four low samples of bit 8
at `0x03000024` select FEL before the normal storage path. Its observed value
was `0x101`, with that bit high. Physical pad assertion has not been tested.
The user's USB power cycle reached FEL with a microSD card inserted, but that
observation does not prove storage-independent recovery after a watchdog reset.

Preparation for the first protected reset requires physical access:

1. Confirm that the board matches the manual's pad layout. Disconnect power
   before attaching a stable, removable connection between BOOT and GND.
2. Reconnect the same USB cable on host port `3-1`, keeping the connection
   between the pads in place. Preserve the inserted microSD card.
3. Read the FEL input through the tool below. All four samples must be low.
   Keep the connection in place throughout the eventual watchdog reset trial.
4. Remove the connection with power disconnected after the protected trial.

The existing session permission rule covers FEL reconnects until host reboot.
No additional permission command is needed while that rule remains installed.
The first reset still needs its own reviewed diagnostic and physical result;
these preparation steps do not qualify watchdog recovery.

The protected watchdog trial is deferred because the physical BOOT connection
is unavailable. USB power-control investigation is closed. No power command or
cable move was performed; the board remains on host port `3-1`. Preserve the
read-only findings under
`~/.cache/sunstoneos/evidence/recovery-power-20261017/`.

Manual USB reconnection remains available for supervised tests with a reviewed
recovery plan. It does not qualify unattended watchdog recovery or an intentional
reset into an unknown storage boot path. The following input-check procedure is
retained for future qualification. No physical connection is currently requested.


```sh
python3 tools/check-fel-input.py \
  --fel "$HOME/.cache/sunstoneos/build/sunxi-a733/sunxi-fel" \
  --rom "$HOME/.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.bin"
```

The [input checker](../tools/check-fel-input.py) pins the FEL tool and captured
ROM, compares the reset and input-routine bytes, and reads the input four times.
It saves and restores its SRAM workspace and records each transaction. It
does not enable a watchdog or reset the board. A high sample produces exit
status 2 after restoration. A transport failure stops requests and records the
failed device instance. Low samples establish the input level during the check;
they do not establish reset behavior or guarantee that a loose contact will hold.

The focused [host check](../checks/fel-input.py) passes low and mixed input
samples, ROM mismatch, transport failure, malformed responses, and restoration
mismatch. It verifies SRAM preservation where communication succeeds and
immediate stopping after a simulated transport failure. Raw output and input
copies are in `~/.cache/sunstoneos/evidence/fel-input-preparation-20261016/`.
No physical input check has run. Existing hardware and performance evidence
was reused without repeating those tests.

## TYPEC24 has a documented DisplayPort route

The pinned `board-schematic` V1.2, sheets 10–11, routes DisplayPort through
TYPEC24. Sheet 11 labels it “TypeC DP Port”; this conclusion does not rely on
COMB0 capability alone. The four connector pairs are:

| Sheet-10 net pair | Sheet-11 path | TYPEC24 pins, P/N |
| --- | --- | --- |
| `COMB0-LANE0-P/N` | C223/C224, `TX-1P/N` | A2/A3 |
| `COMB0-LANE1-P/N` | `RX-1P/N` | B11/B10 |
| `COMB0-LANE2-P/N` | `RX-2P/N` | A11/A10 |
| `COMB0-LANE3-P/N` | C226/C227, `TX-2P/N` | B2/B3 |

SoC `DP-AUXP/N` balls A24/B24 reach TYPEC24 SBU1/SBU2, pins A8/B8,
through C221/C222. The separate `DP-HPD` ball `1A15` is unconnected; do not
assume a direct HPD wire. TYPEC16 instead carries USB0 and input power, with
its SBU pins unconnected. It is not the documented DP connector.

The pinned `board-manual` V1.0 advertises DP1.4 on printed page 2 and describes
Type-C display accessories/output on pages 8 and 38. At kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, the
[exact-board DTS](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60i-a733-orangepi-zero3w.dts#L401)
defines COMB0 DP/USB orientation switching at lines 401–428, DP alternate-mode
SVID `0xff01` at lines 2630–2636, and enabled `allwinner,drm-dp` at
lines 2829–2845. Document identities remain in [the source pins](../sources.lock.json).

Actual board revision/population and physical link operation remain unqualified.
Native Type-C negotiation/orientation, HPD handling, COMB0 exclusion against USB3,
link training and display output remain required. Published routing and source
configuration do not establish successful negotiation or visible output.

## Audio and camera routes are independent of mimus

Schematic sheets 14 and 18 identify separate external and Bluetooth audio buses.
No microphone is available. These connections define implementation targets;
electrical operation and audio capture remain untested.

| Audio signal | Documented route |
| --- | --- |
| External I2S0 master clock | Header pin 7, PB4 |
| External I2S0 bit clock | Header pin 12, PB5 |
| External I2S0 frame clock | Header pin 35, PB6 |
| External I2S0 input | Header pin 38, PB8 |
| External I2S0 output | Header pin 40, PB7 |
| Bluetooth PCM input to SoC | Module PCM_DOUT to I2S1_DIN0 |
| Bluetooth PCM output from SoC | I2S1_DOUT0 to module PCM_DIN |
| Bluetooth PCM clocks | I2S1_BCLK and I2S1_LRCK to module PCM_CLK and PCM_SYNC |

The radio drawing labels module U5 as CDW-20800D8-00. Wi-Fi uses four-bit
SDIO; Bluetooth has UART data and RTS/CTS, plus wake/reset signals and the
PCM bus. Firmware identification and supported voice transport still require
investigation. PCM wiring alone does not establish headset audio support.

Sheet 18 exposes UART1 TX/RX on PE12/PE13 at header pins 29/31. Bluetooth
uses UART1 through PG6–PG9, including RTS/CTS. These are alternative pin routes
of one controller; they cannot serve independent serial clients concurrently.
UART2 PB0/PB1 appears at header pins 11/13. UART6 PE0/PE1 shares header pins
24/23 with SPI3 chip select 0 and clock. The [UART driver record](uart.md)
documents controller capabilities and unresolved device-tree pin definitions.
Actual board revision and electrical operation remain unverified.

Sheet 13 routes four data lanes and one clock pair to each 22-pin camera
connector. CAM1 uses MCSIA, I2C bus TWI11, and control nets PE5/PE6. CAM2 uses
MCSIB, TWI9, and PE9/PE10. Both receive the `VCC_3V3_CSI` supply.
OV5640 module compatibility requires matching the connector pinout, clock,
control signals, voltage requirements, and cable orientation before connection.

Sheet 17 routes four DSI1 data lanes and a clock pair to the 30-pin LCD
connector. It also exposes backlight PWM, display reset, tearing-effect
synchronization, display power enable, touch reset/interrupt, TWI12, ADC4,
and supply rails. Panel selection remains open; controller support and a
selected panel's initialization sequence are separate requirements.

## Current boards have no populated eMMC or UFS

The user confirms that neither current board has physical eMMC or UFS populated.
The manual lists optional capacities and the schematic includes both devices;
those references describe board options, not the fitted configuration. The
absence record comes from the user, not the earlier eMMC timeout. Do not probe
these absent devices. Both microSD cards remain within the storage support and
preservation requirements.

The schematic also has dedicated sheets for power sequencing, LPDDR5,
USB Type-C, microSD, HDMI, PCIe, and header signals. Sheet 5 includes the
PWM-controlled fan, power LED, run LED, reset net, and power-on test point.
Reconcile these circuits with the [hardware matrix](hardware.md) and actual
population as their drivers are implemented. Shared pin functions require
explicit ownership; a header label does not make every alternate function
available simultaneously.

## PWM outputs and analog inputs have specific board uses

The V1.2 schematic routes PWM0 channel 5 through PD5 to the Q2 fan switch.
The fan connector receives 5 V through R88; the PD5 signal bank uses BLDO1.
The kernel fan node's `dc1sw2` supply annotation does not describe that fan
power circuit. A connected fan has not been observed. Stopping PWM does not
by itself establish that Q2 is off; the final pin state and electrical response
require qualification.

The status LED uses PL7, whose alternate function is S_PWM0 channel 5.
Sheet 5's nearby `PWM0-7` label conflicts with the pin assignment on sheet 9
and the kernel pin configuration. The display connector exposes PWM1 channel 7
on PJ25 at pin 17. That output requires a display or other suitable accessory.

The general-purpose analog-to-digital converter (GPADC) has these schematic
connections:

| Channel | Board connection |
| --- | --- |
| GPADC0 | Boot-selection resistor network |
| GPADC1 | DDR-parameter selection divider |
| GPADC2 | Board-identification divider |
| GPADC3 | USB upgrade and COMB1 selection divider |
| GPADC4 | LCD selection input, display connector pin 21 |
| GPADC5–6 | Marked unconnected |
| GPADC7 | No route shown; the manual describes seven GPADC pads despite eight kernel channel entries |

The separate low-resolution converter input LRADC0 has a 10 kΩ pull-up to
AVCC through R23. The schematic does not show the five-key resistor ladder
described by the kernel configuration. The PMIC's own GPADC pin is unconnected.
AVCC uses CLDO3; actual voltage and fitted selection resistors remain unverified.
For LRADC, this nominal 1.8 V pull-up exceeds the manual's documented 0–1.35 V
input range. Neither the schematic nor datasheet establishes safe idle
overrange tolerance. [Raw LRADC sampling](adc-application.md#lradc0-raw-observations-require-separate-electrical-authority)
requires an independent electrical determination before physical use.
The A733 datasheet V0.93 section 5.3 separately lists the ADC supply and positive
reference at a typical 1.8 V, with operating minima and maxima unspecified.
The internal reference setting therefore does not establish the CLDO3 supply
setting or measured AVCC voltage. Native sampling must verify its admitted
supply configuration independently and report raw codes until calibrated.
These selection inputs are candidates for read-only board monitoring. Their
routing does not authorize external signal injection or changes to boot straps.

The source hashes, header PWM alternatives and circuit references are preserved
in `~/.cache/sunstoneos/evidence/pwm-adc-topology-20261018/audit.json`.
