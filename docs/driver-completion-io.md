# I/O register completion audit

This audit covers every file under `drivers/adc`, `audio`, `bluetooth`, `i2c`,
`net`, `pinctrl`, `pwm`, `rtc`, `spi`, `uart` and `watchdog` at revision
`5e0737ea`, with the corrections recorded below. It distinguishes controller
completion from configuration readback.
A successful bus write or matching configuration word does not establish a
completed reset, transfer or clock transition. The findings are source review;
they do not qualify hardware.

## Primary references

Manual citations refer to the A733 User Manual V1.00, source revision
`d0dc4d615be00b76e3a0818d0818eff8f69ffa15`, PDF SHA-256
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The URL and archive are pinned by `a733-user-manual-1.00` in
[sources.lock.json](../sources.lock.json). The cached text is
`~/.cache/sunstoneos/sources/camera-research/A733_User_Manual_V1.00.txt`.
Page numbers below are printed manual pages.

The Linux BSP references use revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Their source-lock keys or component
manifests preserve URLs and hashes:

- RTC: [apps/rtc/sources.json](../apps/rtc/sources.json), `rtc-sunxi.c`,
  `wait_bits_cleared` and `sunxi_rtc_settime`.
- I2S: source-lock keys `board-audio-snd-sunxi-i2s-c` and
  `board-audio-snd-sun60iw2-i2s-c`.
- SPI: source-lock keys `a733-spi-ng-c` and `a733-spi-ng-h`.
- UART: source-lock keys `a733-sunxi-uart-ng-core-c` and
  `a733-sunxi-uart-ng-h`.
- Radio: [radio SDIO reference sources](../drivers/net/radio-sdio-reference-sources.json)
  and the `radio-platform-power-delay` and `radio-data-aicwf_sdio-c`
  source-lock keys. Cached paths are relative to the sources directory above.

## Completion sites and dependent operations

| Driver and function | Register and reference | Current behavior and assessment |
| --- | --- | --- |
| `adc/a733_adc.c`, `startup`, `a733_adc_read` | `GP_CTRL.ADC_EN`; `GP_DATA_INTS` +0x38; manual 18.12.6.1 steps 8–10, pp1525–1526; 18.12.8.13, p1538 | Enable is configuration. Samples publish only when the selected data-pending bit is set. Correct completion predicate; a zero status returns AGAIN. |
| `adc/a733_adc.c`, `startup` | `GP_SR_CON.TACQ/FS_DIV`; manual 18.12.5.2 and 18.12.6.1, p1525 | Programs 100µs acquisition and 1kHz sampling. The data-pending test supplies conversion completion. No additional undocumented startup delay is inferred. |
| `adc/a733_lradc.c`, `startup`, `a733_lradc_read`, `clear_data` | `LRADC_CTRL.EN`; `LRADC_INTS.DATA0`; manual 18.13.6.2, 18.13.8.1/.3, pp1549–1553 | Clears inherited pending data before enable and publishes only a fresh pending sample. W1C is read back; one concurrent conversion is acknowledged. Correct under the admitted sampling interval. |
| `audio/a733_i2s_platform.c`, `a733_audio_parent_poll` | `PLL_AUDIO1_CTRL` +0x280, ENABLE/LOCK_ENABLE/LOCK/OUTPUT_GATE; manual 4.1.4.1–.2, pp25–27, and 4.1.6.51 | Cold startup waits at least 3µs between three consecutive LOCK samples, then 20µs before output enable. Correct. Existing-state adoption checks LOCK without changing the PLL. |
| `audio/a733_i2s_platform.c`, `a733_audio_parent_poll` | AUDIO gate configuration +0x191c and effective status +0x199c; manual 4.1.6.254 and corresponding status table | Waits for both effective output status bits before publishing the stable lease. Correct; control readback alone is not accepted. |
| `audio/a733_i2s_platform.c`, `local_poll` | I2S0 bus gate/reset +0x120c | Holds reset for 10µs before release. This is a conservative local reset interval; no controller completion bit is defined for this CCU R/W field. |
| `audio/a733_i2s_stream.c`, `a733_i2s_stream_start`, `a733_i2s_stream_poll` | `FCTL.FRX/FTX`, bits24/25; manual 17.1.8.6, p1058 | Writes flush once, enters FLUSH, and waits for self-clear before arming DMA. The provider excludes self-clearing bits from configuration readback. Correct. |
| `audio/a733_i2s_stream.c`, `a733_i2s_stream_poll` | `FSTA.TX_CNT`; manual 17.1.8.7, p1059 | EOF waits for the DMA owner to retire and FIFO free count to reach 128. FIFO empty alone is not a serializer fence; the stream retains its platform resources until stop/close. |
| `audio/a733_hdmi_audio.c`, `a733_hdmi_audio_prepare` | HDMI sampler FIFO reset +0x3100, MC reset +0x4002, CTS calculation +0x3205 | Uses a 5µs delay after sampler reset and 5ms after CTS trigger, following pinned `dw_avp.c`/`dw_fc.c` referenced by the driver. MC reset is treated as a command. A focused delayed sampler/CTS model is still needed; no completion bit is asserted by this audit. |
| `i2c/a733_i2c.c`, `step` | `S_TWI0.CNTR.INT_FLAG`, status +0x10; manual 18.11.5.1/.6.2–.3, pp1474–1481 | Clears the old flag with the command, waits for the new flag and checks the exact protocol status. Correct; bounded by 10ms for the transaction. |
| `i2c/a733_i2c.c`, `a733_i2c_read8` | `CNTR.M_STP`, status IDLE=0xf8; manual 18.11.9.4/.5 and 18.11.10 equivalents | Waits for STOP clear and idle status before publishing output. Arbitration loss skips STOP. Correct. Close changes only configuration after the completed transaction. |
| `i2c/a733_twi.c`, `a733_twi_poll` | `CNTR.INT_FLAG`, `CNTR.M_STP`, status +0x10 | SEND/WAIT and STOP/STOP_WAIT preserve the same predicates as above across bounded polling calls. No next byte is sent before the preceding status is accepted. Correct. |
| `i2c/a733_twi_native.c`, `a733_twi_native_prepare` | TWI software reset +0x18; manual 18.11.9.7, p1490 | Observes reset clear during adoption; does not issue software reset. Its other writes configure the divider, enhancement and bus enable. No missing wait after a software-reset request exists here. |
| `net/a733_radio_platform.c`, platform polling | Supply/module enable; bus gate/reset +0xd1c | Waits 10ms after supply observation, 50ms after module enable, and 10µs with controller reset held. The pinned radio power sources define the external sequence; physical module/rail applicability remains a separate qualification gap. SDHC command completion is delegated to `sunxi_sdio`, outside this file ownership. |
| `net/aic8800_sdio_transport.c`, `completed` | CCCR IOEx +2 / IORx +3 | ENABLE_WRITE starts a bounded enable deadline; ENABLE_READY polls IORx function1 ready. Correct. SDIO controller completion precedes processing the CMD52 response. |
| `net/aic8800_sdio_transport.c`, `completed` | AIC pad controls +0xf1/+0xf2 | Waits 1ms after the final pad write and pad restoration. Pinned `radio/.../aic8800_bsp/aicsdio.c` lines1670–1685 contains the pad write and `msleep(1)`. Correct elapsed-time dependency. |
| `net/aic8800_firmware.c`, `advance`, `aic8800_firmware_step` | Bluetooth patch-table type4 power entry | Waits the claimed power delay after the matched write confirmation. Delay is supplied as at least half a millisecond by the transport. Firmware confirmations establish protocol acceptance; they do not establish undocumented remote hardware settling. |
| `pwm/a733_pwm.c`, `hardware(CONFIGURE)`, `cycle` step3 | Channel5 `PCR.PWM_PERIOD_RDY`, bit11; manual 18.7.8.25, p1347 | Checks ready before writing PPR. Startup preserves the admitted baseline and completed control writes in private substeps while busy. The service path also returns PENDING. Foreign PCR fields and deadline expiry retain ownership. Correct. |
| `pwm/a733_pwm.c`, `a733_pwm_start`, `hardware(RUN)` | `PCR.PWM_PUL_START`, bit10; manual 18.7.8.25, p1347 | Starts once; finite completion requires self-clear and the period interrupt. Correct. A permanently asserted start bit cannot finish before the deadline. |
| `rtc/a733_rtc_initialize.c`, `wait_clear`, `initialize` | `LOSC_CTRL.HHMMSS_ACCE/YMD_ACCE`, bits8/7; manual 9.1.7.1, p392 | Waits before and after each calendar write, then 2ms after clear. One-second bound comes from the manual; the extra delay follows BSP `wait_bits_cleared`, lines319–337. Correct. Calendar values are validated through a coherent read. |
| `rtc/a733_rtc_initialize.c`, `source` | LOSC crystal gate/source, health and selected-source status; manual 9.1.7.1/.2, pp391–393 | Requires four healthy observations at least 3µs apart and polls the selected source/status with a one-second bound. Readback of the mux word alone does not complete startup. |
| `spi/a733_spi.c`, `a733_spi_begin`, `a733_spi_poll` RESETTING | `GCR.SRST`, bit31; manual 18.9.8.1, p1405 | Requests reset once; waits for self-clear before TCR/FCR initialization. Correct. |
| `spi/a733_spi.c`, `a733_spi_poll` FIFO_RESET | `FCR.TF_RST/RF_RST`, bits31/15; manual 18.9.8.5, p1413 | Waits for both reset bits to clear, then checks idle/FIFO/buffer counts before enabling. Correct. |
| `spi/a733_spi.c`, `a733_spi_poll` ACTIVE/HOLD | `TCR.XCH`, `ISR.TC`, FIFO and buffer counts; manual 18.9.8.2/.4/.6/.27, pp1406,1411,1414,1432 | Requires TC, XCH clear and empty FIFOs/buffers. DMA request gating and another idle check precede completion. Setup/hold delays use elapsed time. Correct. |
| `spi/a733_spi_platform.c`, register stages | CCU SPI gate/reset | Holds reset for 10µs on entry and shutdown. GIC mask propagation is checked separately. Immediate CCU readback validates configuration; SPI self-reset is awaited by the core. |
| `uart/a733_uart.c`, `a733_uart_begin`, `a733_uart_poll` CONFIGURING | `HALT.CHANGE_UPDATE`, bit2; manual 18.10.6.4, p1441, 18.10.9.18, pp1466–1467 | Requests update once and waits for self-clear before clearing DLAB and enabling operation. Correct. |
| `uart/a733_uart.c`, `a733_uart_poll` CONFIGURING | `FCR.XFIFOR/RFIFOR`, bits2/1; manual 18.10.9.7, p1452 | FCR is write-only; reading its address returns IIR. Source writes the reset command and then configuration. A reset-bit readback wait would be invalid. The current code follows that distinction. |
| `uart/a733_uart.c`, DRAINING, `a733_uart_dma_release` | `LSR.TEMT`, `USR.BUSY/RFNE/TFE`; manual 18.10.6.5 and 18.10.9 status descriptions | Requires the serializer empty, RX empty, busy clear and TX FIFO empty before publishing drain/release. Correct; software FIFO submission is not completion. |
| `uart/a733_uart_platform.c`, register stages | CCU UART gate/reset | Holds reset for 10µs and checks GIC propagation. Core CHANGE_UPDATE supplies the controller-specific completion. |
| `watchdog/a733_watchdog.c`, arm/disarm/feed | `WDT_CTRL.RESTART`, mode/config/output; manual 8.3.6.4–.7, pp379–381 | RESTART is a keyed write-one command, with no documented completion/status wait. Mode/config/output use readback. A fabricated RESTART self-clear wait would add no documented guarantee. |
| `watchdog/a733_rom_return.c`, prepare/restore | Redirect entry/flag SRAM registers | These are boot-protocol storage words; the driver checks each write and orders entry before publication, invalidation before restoration. They have no documented asynchronous completion field. |

Plain configuration writes also occur in these files: ADC thresholds/enables,
I2S format/count/request control, TWI divider/control, radio clock words, PWM
gates/enables/PPR/counters, RTC alarm registers, SPI counts/request gates,
UART interrupt/mode/request gates, watchdog policy and GPIO mux/drive/pull/data.
Their readbacks validate the retained configuration. W1C interrupt status is a
command and can reassert for a new event. No controller-completion wait should
be inferred from an unrelated matching word.

## Files with no additional completion protocol

The following adapters forward accesses to the sequencers listed above. They
introduce no additional hardware command: `adc/a733_adc_native.c`,
`audio/a733_audio_leases.c`, `audio/a733_i2s_platform_native.c`,
`net/a733_radio_platform_native.c`, `pinctrl/a733_pins_native.c`,
`pwm/a733_pwm_native.c`, `spi/a733_spi_platform_native.c` and
`uart/a733_uart_platform_native.c`.

`pinctrl/a733_pins.c` writes GPIO mux, drive, pull and output latches. Its R/W
fields and write-one SET/CLR operations have no apply/busy handshake in the
manual GPIO register descriptions. Output latch readback is restricted to
outputs. `rtc/a733_rtc.c` writes alarm targets, enables and W1C pending status;
manual 9.1.7.7–.12 defines no calendar access-busy protocol for those words.
`rtc/a733_rtc_boot.c` releases its bus reset and installs a disabled GIC route;
it does not write the RTC calendar or switch its oscillator.

These files contain no hardware register writes:

- `audio/a733_i2s.c`, `audio/dma_service_i2s_irq.c`.
- `bluetooth/bt_acl.c`, `bt_avdtp.c`, `bt_h4.c`, `bt_hci.c`, `bt_hfp_ag.c`,
  `bt_l2cap.c`, `bt_rfcomm.c`, `bt_sco.c`, `bt_sdp.c`, `bt_sdp_server.c`,
  `bt_security.c`.
- `net/a733_radio_entropy.c`, `a733_radio_session.c`, `aic8800_data.c`,
  `aic8800_dispatch.c`, `aic8800_network.c`, `aic8800_openssl.c`,
  `aic8800_openssl_libc.c`, `aic8800_runtime.c`, `aic8800_sae.c`,
  `aic8800_sdio_codec.c`, `aic8800_security.c`, `aic8800_supplicant.c`,
  `aic8800_supplicant_os.c`, `aic8800_wifi.c`, `sunstone_net.c`,
  `sunstone_sntp.c`.
- `pinctrl/a733_pin_tables.c`, `watchdog/a733_maintenance_watchdog.c`.

The remaining non-C files are `i2c/README.md` and
`net/radio-sdio-reference-sources.json`; neither executes register accesses.
Bluetooth protocol command completion is distinct from local MMIO completion.
The maintenance watchdog delegates the hardware reload to its native provider.

## Models preserve pending hardware state

The baseline fixtures at `5e0737ea` could not establish these waits:

- `checks/a733-spi.c` clears GCR and FIFO reset bits in the next `now()` call.
  The first poll sees completed reset. A driver that omits either wait can pass.
- `checks/i2s-platform.c` sets PLL LOCK during the ENABLE write and effective
  gate status during output enable. It exercises stuck lock and gate status,
  but a normal delayed lock and unstable lock sequence are absent.
- `checks/i2s-stream.c` clears FIFO flush bits inside the write callback.
  DMA-before-flush and permanently stuck flush are not covered.
- `checks/a733-uart.c` exposes an explicit `update_pending` switch but writing
  CHANGE_UPDATE does not start a timed operation. Normal startup can finish
  without a pending interval. Its separate busy case remains useful.
- `checks/a733-pwm.c` stores PPR directly and leaves PERIOD_RDY zero. Finite
  pulse completion is manually injected. This covers caller state, but does
  not establish readiness timing or a controller-driven start transition.

The repaired C fixtures use the shared virtual-time transition helper
[`register_latency.h`](../checks/register_latency.h). Writing a request
must retain its active bit until its scheduled completion. Reads before that
time return the old active state; readback of configuration must not advance
the transition. The fixtures reject dependent writes while active. SPI covers
delayed and stuck controller/FIFO reset. UART starts its update interval from
the command write, preserving its existing busy and timeout cases. I2S covers
delayed lock, an interrupted sequence of lock samples, the 20µs settle interval,
delayed gate status and delayed FIFO flush. A separate FreeRTOS case retains
the stream on stuck-flush timeout before DMA or direction enable.

PWM startup now keeps its progress in a private `configure_step` field. It
admits the baseline once, waits before programming control, then waits before
writing PPR. Its report layout and numeric phase values remain unchanged.
The model tests initial busy, busy following the driver's control write,
stuck-busy deadline expiry and foreign control bits. The busy cases prohibit
PPR, pin acquisition and waveform start. The constructed delays exercise
ordering; the manual does not establish those particular durations.

Existing transfer, interrupt, cache, alias, contention, output and ownership
cases remain. Source comparisons of register offsets remain valuable, but
they establish layout agreement rather than asynchronous completion. The
three reviewers approved the PWM design and final code before commit. No
voltage, rail, clock-point, thermal or storage-write behavior changed.

## Focused verification

Clang21 AddressSanitizer/UndefinedBehaviorSanitizer builds pass for the SPI,
UART, PWM and I2S platform models. UART drain/capacity/modes, I2S gate/clock
fault/monotonic/rate cases and the unchanged composed PWM continuous service
also pass. The AArch64 PWM core compiles with the repository runtime headers.
The FreeRTOS I2S normal stream, simultaneous duplex and stuck-flush cases pass
in QEMU. These checks establish software behavior with modeled peripherals.

One execution of the same four focused host closures measured the following
wall times, including process and sanitizer startup. The corrected fixtures
include the additional completion cases; these times are check costs, with no
inference about device throughput.

| Closure | Baseline milliseconds | Corrected milliseconds |
| --- | ---: | ---: |
| SPI | 4.754 | 4.448 |
| UART | 4.243 | 4.669 |
| PWM | 4.268 | 4.774 |
| I2S platform | 4.318 | 4.203 |

Raw logs, baseline source snapshots, executable hashes and timing records are
under `~/.cache/sunstoneos/build/model-fidelity-1031/io/`. The PWM change adds
63 lines and removes five across its driver, private structure and fixture.
The other timing fixtures add 92 lines and remove eight. Test binaries used
the same compiler and sanitizer flags before and after.

The full native PWM profile remains blocked during CMake configuration by
the existing `apps/pwm/single-core.patch` hunk at `platform/a733/platform.c`.
The failed cache and configure log are preserved. No board was accessed;
physical transition latency and peripheral qualification remain open.
