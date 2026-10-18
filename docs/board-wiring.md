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

## Optional storage and other interfaces remain in scope

The manual's hardware table lists optional eMMC and UFS capacities. Its
wording about default population is ambiguous, and the schematic includes
both devices. Neither document proves that either device is populated on
the attached board. The eMMC identification timeout therefore remains an
unresolved result, not proof of an absent device.

The schematic also has dedicated sheets for power sequencing, LPDDR5,
USB Type-C, microSD, HDMI, PCIe, and header signals. Sheet 5 includes the
PWM-controlled fan, power LED, run LED, reset net, and power-on test point.
Reconcile these circuits with the [hardware matrix](hardware.md) and actual
population as their drivers are implemented. Shared pin functions require
explicit ownership; a header label does not make every alternate function
available simultaneously.
