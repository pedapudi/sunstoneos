# Radio SDIO operating points and physical admission

This audit is for native radio-driver developers and the board coordinator.
The implemented four-bit 25 MHz path is an initial qualification mode.
It does not satisfy the vendor-rate requirement. Radio operation remains
physically unqualified, including identity, firmware loading and RF traffic.

## The selected reference targets SDR104 at 208 MHz

The published A733 kernel configuration selects `CONFIG_AW_MMC_V5P3X`.
The SoC device tree identifies SDC1 as `allwinner,sunxi-mmc-v5p3x` at
`0x04021000`. The host driver selects its controller-1 implementation from
that compatible string and `device_type = "sdc1"`.

The board's SDC1 node enables four-bit SDIO and SDR25, SDR50, DDR50 and SDR104
timing, with `max-frequency = <208000000>` and kernel tuning. SDR104 is the
single-data-rate UHS-I mode with a protocol limit of 208 MHz.
The AIC D80 bootstrap driver also requests 208 MHz, bounded by the host maximum.
The 150 MHz setting in the adjacent SDC0 node belongs to microSD.

The v5p3x driver requests twice the card rate for single-data-rate operation.
It compares rounded rates from the device tree's PERI1 400 MHz and 300 MHz
parents, then records half the resulting module rate as the card clock.
With those nominal parent rates, a 416 MHz module request selects 400 MHz,
yielding a 200 MHz card clock. This is a source-derived prediction.
Actual PLL state, card capabilities and electrical timing remain unobserved.

The [source record](../drivers/net/radio-sdio-reference-sources.json) gives
URLs, revisions, hashes, licenses and inspected sections. These sources are
host-side references; Linux does not execute in SunstoneOS.

## Native startup still lacks the UHS sequence

The existing [platform](radio-platform.md) enumerates at one bit and 400 kHz.
Its four-bit request sets the card width, switches to PERI0 divided by eight,
and verifies a 25 MHz card clock. Firmware owners require that implemented mode.

The following reference steps remain missing from the native path:

| Reference step | Native gap and required evidence |
| --- | --- |
| Read common-card high-speed, UHS and drive-strength capabilities | Enumeration currently reads common control addresses 0, 1 and 8. Add the capability observations before selecting a higher mode. |
| Negotiate 1.8 V signaling through CMD5 and CMD11 | Native CMD5 never sets its 1.8 V request bit (S18R) and has no CMD11 state. Bind the protocol transition to the verified fixed I/O supply. |
| Set four-bit width, drive strength and bus-speed selection | Width exists; drive-strength and speed selection do not. Preserve unrelated register bits and verify selection before changing host timing. |
| Program the selected v5p3x timing and clock parent | Native old timing and PERI0 at 25 MHz do not implement the PERI1 UHS profile. Obtain shared-provider ownership and verify the resulting rate. |
| Tune SDR50 or SDR104 using CMD19 | Native submission accepts only CMD3, CMD5, CMD7, CMD52 and CMD53. Add bounded tuning and independent data comparison before admitting normal traffic. |
| Deliver SDIO completion and function interrupts | Native service masks INTID 194 and polls. IRQ delivery, bounded cancellation and reusable shutdown remain required. |

The board node sets `sunxi-dis-signal-vol-sw`. The selected voltage callback
therefore returns without changing a regulator. The command-level voltage
protocol and a PMIC voltage change are separate operations. Importing the
microSD rail-switch procedure would not reproduce this radio reference.
Any new clock point needs a design and coordinator approval before a board run.

## Register observations cannot resolve the fitted wiring

The schematic routes PM2 through R61 to Bluetooth reset and through R1130 to
the module's low-power oscillator input. Their fitted population is unknown.
The schematic also names DCDC8 for radio I/O, while the device tree names
BLDO5 and CLDO1. The [platform admission contract](radio-platform.md) retains
these uncertainties and preserves PM2.

The [radio observation list](../apps/board/radio-facts.json) uses the existing
read-only bundle reader. It contains SID identity, speed-bin and clock words,
then PM/PG configuration and RTC clock controls behind their prerequisites.
It never reads an SDIO aperture or communicates with the radio module.
The RTC reads require both its bus gate and reset release.
PIO reads require the recorded APB clock route; a mismatch skips them.

GPIO DATA reads reveal input samples or live GPIO output latches.
They do not recover hidden latches for alternate-function pins or establish
resistor population, continuity, pad voltage or an oscillator waveform.
The PMIC is behind I2C and cannot be sampled by reading a FEL MMIO address.
The power owner must supply fresh rail snapshots through its shared provider.

The coordinator may use this command after live identity, permissions, recovery
state and queue admission permit the read batch. Run it once per selected
board, with a fresh evidence directory. The reader takes the port lock,
preserves and verifies the ROM scratch area, and stops after a transport failure.

```sh
SUNSTONE_USB_PORT=3-3 "$HOME/.cache/sunstoneos/venv/bin/python3" -B \
  tools/board-bundles-next-read.py apps/board/radio-facts.json \
  "$HOME/.cache/sunstoneos/evidence/radio-facts-3-3-ATTEMPT"
```

Expected: the selected board's saved SID, speed-bin word `0x496` on 3-3
or `0x495` on 3-1, and `complete=true`, `scratch_restored=true` in `session.json`.
The saved bin captures are `board-session-20261025-3/port-3-3/bb9-read/` and
`board-session-20261025-4/port-3-1/bb9-read/` under the evidence cache.
The saved SID words in address order are `32c05100 4c004900 58a30214 40892413`
on 3-3 and `32c05100 4c004900 58a30214 40792413` on 3-1.
These come from the saved DDR sessions under
`~/.cache/sunstoneos/evidence/dev-runs-1031/3-3-ddr-final-restore/` and
`3-1-ddr-final-survey/`; they do not establish current powered state.
Every skipped word needs an explanation before it supports admission.
No PM/PG pin configuration or RTC fanout value is predicted.
Use port 3-1 and a separate evidence directory for an independently needed batch.
No board command was executed for this audit.

Physical inspection or an authoritative fitted BOM must establish the radio
supply routes, R61/R1130 population and module reset/LPO requirements.
Measured I/O voltage and any required LPO waveform complete that evidence.
The user need not identify the PCB revision from memory.

## Physical identity needs a different runnable image

The existing identity application (since removed) has a
strict authority producer and a zero-authority refusal image. The refusal image
prints `REFUSED radio SDIO identity` and exits with status 7 before constructing
providers. It is useful for host instruction checks and grants no board access.

An admitted identity image requires fitted rail/reset/LPO evidence, its pin
contract and a reviewed returning lifetime. The current application retains
resources even after success and requires whole-board power removal afterward.
It cannot be queued for ordinary reusable returns while that closure is missing.
After those prerequisites are resolved, compare the existing one-bit and
four-bit CMD53 identity results before firmware/HCI identity, association,
traffic, reconnect and coexistence runs. Headset interoperability additionally
requires a headset; none is attached.
