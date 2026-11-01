# I/O register completion audit

This audit covers every file under `drivers/adc`, `audio`, `bluetooth`, `i2c`,
`net`, `pinctrl`, `pwm`, `rtc`, `spi`, `uart` and `watchdog` at revision
`298c5a34`, including its merged native adapters, with the explicit ordering
correction in `1e15e490`. The inventory contains
61 C files and two non-C files. It distinguishes controller
completion from configuration readback.
A successful bus write or matching configuration word does not establish a
completed reset, transfer or clock transition. A data synchronization barrier
(DSB) completes architectural accesses; the device can still need a status
check or minimum hold. The findings are source review;
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

The U-Boot GPADC and PWM comparisons use revision
`b791be842935b27268ae3d00e943a9075495f30a`, cached under
`~/.cache/sunstoneos/sources/u-boot-orangepi-b791be842935b27268ae3d00e943a9075495f30a/`.
The audio CCU helpers are under `i2s-platform/`; their hashes and URLs are
pinned in [the I2S platform sources](i2s-platform.md#pinned-sources-define-the-initial-profile).
The cached watchdog BSP source is `board-kernel/sunxi_wdt.c`, from
[`bsp/drivers/watchdog/sunxi_wdt.c`](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/watchdog/sunxi_wdt.c)
at the Linux revision above, SHA-256
`0fdde1b27191be41604a7a529ddbcfc53dbd88f81009eb683fd9b691d5c335a9`.

## Completion sites and dependent operations

| Driver and function | Register and reference | Current behavior and assessment |
| --- | --- | --- |
| `adc/a733_adc.c`, `startup`, `a733_adc_read` | `GP_CTRL.ADC_EN`; `GP_DATA_INTS` +0x38; manual 18.12.6.1 steps 8–10, pp1525–1526; 18.12.8.13, p1538 | Enable is configuration. Samples publish only when the selected data-pending bit is set. Correct completion predicate; a zero status returns AGAIN. |
| `adc/a733_adc.c`, `startup` | `GP_SR_CON.TACQ/FS_DIV`; manual 18.12.5.2 and 18.12.6.1, p1525 | Programs 100µs acquisition and 1kHz sampling. The data-pending test supplies conversion completion. No additional undocumented startup delay is inferred. |
| `adc/a733_lradc.c`, `startup`, `a733_lradc_read`, `clear_data` | `LRADC_CTRL.EN`; `LRADC_INTS.DATA0`; manual 18.13.6.2, 18.13.8.1/.3, pp1549–1553 | Clears inherited pending data before enable and publishes only a fresh pending sample. W1C is read back; one concurrent conversion is acknowledged. Correct under the admitted sampling interval. |
| `audio/a733_i2s_platform.c`, `a733_audio_parent_poll` | `PLL_AUDIO1_CTRL` +0x280, ENABLE/LOCK_ENABLE/LOCK/OUTPUT_GATE; manual 4.1.4.1–.2, pp25–27, and 4.1.6.51 | Cold startup waits at least 3µs between three consecutive LOCK samples, then 20µs before output enable. Correct. Existing-state adoption checks LOCK without changing the PLL. |
| `audio/a733_i2s_platform.c`, `a733_audio_parent_poll` | AUDIO gate configuration +0x191c and effective status +0x199c; manual 4.1.6.254 and corresponding status table | Waits for both effective output status bits before publishing the stable lease. Correct; control readback alone is not accepted. |
| `audio/a733_i2s_platform.c`, `local_poll` | I2S0 bus gate/reset +0x120c | Holds reset for 10µs before release. This is a conservative local reset interval; no controller completion bit is defined for this CCU R/W field. |
| `audio/a733_i2s_stream.c`, `a733_i2s_stream_start`, `a733_i2s_stream_poll` | `FCTL.FRX/FTX`, bits24/25; manual 17.1.8.6, p1058 | Writes flush once, enters FLUSH, and waits for self-clear before arming DMA. The provider excludes self-clearing bits from configuration readback. This establishes the manual flush predicate. The additional vendor playback preparation and stop delays remain unimplemented, as detailed below. |
| `audio/a733_i2s_stream.c`, `a733_i2s_stream_poll` | `FSTA.TX_CNT`; manual 17.1.8.7, p1059 | EOF waits for the DMA owner to retire and FIFO free count to reach 128. FIFO empty alone is not a serializer fence; the stream retains its platform resources until stop/close. |
| `audio/a733_hdmi_audio.c`, `a733_hdmi_audio_prepare` | HDMI sampler FIFO reset +0x3100, MC reset +0x4002, CTS calculation +0x3205 | Requires true 5µs after sampler reset and 5ms after CTS trigger, following pinned `dw_avp.c`. Checked rounded-clock endpoints add 1µs. MC reset is a command. The focused adapter fixture covers both fractional-time boundaries and deadline refusal. |
| `i2c/a733_i2c.c`, `step` | `S_TWI0.CNTR.INT_FLAG`, status +0x10; manual 18.11.5.1/.6.2–.3, pp1474–1481 | Clears the old flag with the command, waits for the new flag and checks the exact protocol status. The protocol predicate is correct and bounded by 10ms. The legacy provider uses explicit DSB architectural completion before each dependent access; see the ordering table. |
| `i2c/a733_i2c.c`, `a733_i2c_read8` | `CNTR.M_STP`, status IDLE=0xf8; manual 18.11.9.4/.5 and 18.11.10 equivalents | Waits for STOP clear and idle status before publishing output. Arbitration loss skips STOP. The protocol predicate is correct. Close changes only configuration after the completed transaction; its write completes architecturally before ownership release. |
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
| `watchdog/a733_watchdog.c`, arm/disarm/feed | `WDT_CTRL.RESTART`, mode/config/output; manual 8.3.6.4–.7, pp379–381 | RESTART is a keyed write-one command, with no documented completion/status wait. Mode/config/output use readback and the legacy provider completes its architectural accesses with DSB. A fabricated RESTART self-clear wait would add no documented guarantee. |
| `watchdog/a733_rom_return.c`, prepare/restore | Redirect entry/flag SRAM registers | These are boot-protocol storage words; the driver checks each write and requests entry before publication, invalidation before restoration. DSB completes each architectural access before the dependent publication/restoration step. They have no documented asynchronous completion field. |

Plain configuration writes also occur in these files: ADC thresholds/enables,
I2S format/count/request control, TWI divider/control, radio clock words, PWM
gates/enables/PPR/counters, RTC alarm registers, SPI counts/request gates,
UART interrupt/mode/request gates, watchdog policy and GPIO mux/drive/pull/data.
Their readbacks validate the retained configuration. W1C interrupt status is a
command and can reassert for a new event. No controller-completion wait should
be inferred from an unrelated matching word.

## Native write ordering and vendor inter-write delays

The following table traces the completion sites above to the executing MMIO
provider. `DSB both` means `DSB SY; access; DSB SY`, and `DSB after` means
`access; DSB SY`, as defined in [the MMIO accessors](../include/a733_mmio.h).
A post-store DSB precedes the next dependent write, poll or timestamp in these
providers. A data memory barrier (DMB) orders accesses without supplying that
completion guarantee. None of these providers substitutes DMB for DSB.

Architectural completion and device application remain separate. An early
acknowledgment permitted by Device-nGnRE can precede the endpoint effect.
DSB does not establish PLL lock, FIFO flush, watchdog restart acceptance or
remote firmware progress. The status predicates, readbacks and minimum holds
listed above remain necessary. The legacy returning payloads inherit the ROM's
memory attributes; these drivers do not independently admit that mapping.

| Sequence | Executing provider and barrier | Reference, delay and assessment |
| --- | --- | --- |
| GPADC/LRADC configure, W1C, enable, pending poll | `adc/a733_adc_native.c`: DSB both; masked writes explicitly DSB before old-word read and after store | Manual 18.12.6.1 and 18.13.6.2. Conversion pending gates publication. U-Boot `drivers/adc/sunxi_gpadc.c:19–38` excludes its 2µs reset hold for SUN60IW2; that hold is inapplicable to A733. Its 1500µs read delay precedes a pending query, while native code returns AGAIN until pending. |
| Audio PLL LDO/enable/lock-enable, output gate and effective-status poll | `audio/a733_i2s_platform_native.c`, reached by `apps/i2s/main.c` and `apps/os/audio.c` through `a733_audio_leases`: DSB both | Manual 4.1.4.1–.2 and cached `i2s-platform/ccu_gate.c:180–205`, `ccu_common.c:47–66`. Native strict 3µs sample and 20µs settle guards follow completed accesses. No fixed LDO-to-enable hold is present in that vendor helper. |
| I2S0 local reset, format, flush/counter, DRQ and direction | Same audio native provider: DSB both for each write and poll | Manual 17.1.8.6–.7 supplies self-clear/occupancy predicates. Local reset holds 10µs. Vendor playback adds repeated 1ms flush holds and a 250µs stop hold; see the applicability finding below. |
| I2S3 stream/module/reset/flush/DRQ | `apps/hdmi-audio/native.c:io_read/io_write`: DSB both; parent callback is adoption-only and refuses writes | Same I2S predicates; HDMI's internal route does not establish that external playback pop mitigation is mandatory. The vendor delay divergence remains recorded for route-specific review. |
| HDMI sampler reset, MC reset and CTS trigger | `display/a733_hdmi_phy.c:a733_hdmi_write` calls the selected barrier after each write; `platform/a733/hdmi_native.c` uses DSB both for byte/word accesses and DSB SY in its barrier callback | Cached `dw_avp.c:148/297`: 5µs and 5ms. Native checked 6µs/5001µs endpoints start after the actual completed write and preserve the original deadline. |
| Legacy inherited S_TWI0 DATA→CNTR→FLAG/status, STOP→idle, close→release | `i2c/a733_i2c.c` uses DSB both; `boot.h` supplies only its counter | Manual 18.11.5.1 and 18.11.9.4–.5 define the protocol predicates. Each architectural access completes before the dependent command/poll/release. No fixed inter-byte delay is specified. |
| Leased TWI DATA/control/status and adoption configuration | `i2c/a733_twi_native.c:load/store`: DSB both | Same manual protocol. Adoption checks inherited reset clear and does not request reset. Completion predicates remain in `a733_twi_poll`. |
| Radio supply→REGON→module clocks/reset; GPIO publication | `net/a733_radio_platform_native.c`: DSB both and DSB SY barrier callback; GPIO operations reach `pinctrl/a733_pins_native.c` DSB both | Cached `sunxi-wlan.c:160` and `aicsdio.c:430`: 10ms/50ms. Strict elapsed guards start after completed pin operations. The local 10µs SDHC reset hold has no cited vendor minimum. |
| AIC function enable, pad writes/restoration and Bluetooth power entry | `net/aic8800_sdio_transport.c` observes completed SDIO commands; controller MMIO belongs to the SDIO provider outside this inventory | Cached `aicsdio.c:1685` and `aic_bsp_driver.c:719–720`: 1ms/500µs from completed command/response, plus one provider tick. Host DSB cannot establish remote firmware register application. |
| GPIO latch before mux, drive/pull/mux and SET/CLR readback | `pinctrl/a733_pins_native.c`: DSB both | Manual GPIO register descriptions supply no apply/busy field or fixed hold. Each latch/configuration write completes architecturally before subsequent pin programming. |
| PWM reset/gate, control→PERIOD_RDY→PPR, enable/start | `pwm/a733_pwm_native.c`: DSB both; its baseline BGR read is plain after earlier completed stores | Manual 18.7.8.25: readiness/start predicates remain separate. Pinned U-Boot `drivers/pwm/sunxi_pwm.c` supplies no fixed inter-write hold for this sequence. |
| RTC oscillator, calendar, alarm and boot bus/GIC route | `apps/rtc/{initialize,native,runtime,service-port,snapshot}.c`: reads DSB both, writes DSB after; snapshot route64 also DSB after | Manual 9.1.7 and cached `rtc-native/rtc-sunxi.c:319–337`: post-busy 2ms guard uses 2001µs. GIC propagation is separately checked. |
| Returning RTC source/alarm operations | `apps/rtc-source-returning/main.c` and `apps/rtc-alarm-returning/main.c`: DSB after reads/writes | Same RTC predicates and holds. `apps/rtc-returning/main.c` is read-only and refuses its write callback; it adds no pending store. |
| SPI local reset, GCR/FCR reset→poll, setup→XCH and completion→hold | `spi/a733_spi_platform_native.c`: byte/word DSB both | Manual 18.9 and cached `spi/ng/spi-sunxi.c:347–378/543–555`: reset self-clear. Vendor 5µs poll spacing is cadence. Its 100µs ready-GPIO holds at 1032/1065 are slave-only; the native path is master. Caller setup/hold minima use post-access origins. |
| UART local reset, HALT/LCR/divisor→UPDATE→poll, FCR command/config and drain | `uart/a733_uart_platform_native.c`: DSB both, including each divisor/update write | Manual 18.10.6.4 and cached `uart/sunxi-uart-ng-core.c:sunxi_uart_force_lcr` preserve update ordering with no fixed configuration hold. The vendor 1µs console polling pauses at 721/738 are cadence. Local reset holds 10µs. |
| Watchdog policy→MODE enable→RESTART, disable→restoration and feed→return | `watchdog/a733_watchdog.c` uses DSB both; `checks/a733-watchdog-configuration.c` uses the same accessors for its CONFIG/OUTPUT-only trial | Manual 8.3.6.4–.7 and cached `board-kernel/sunxi_wdt.c:sunxi_wdt_set_timeout/start/ping`. Architectural completion precedes the next operation. RESTART has no acknowledgment field; the vendor's reset loop `mdelay(5)` follows restart and is not an inter-write hold. |
| ROM redirect ENTRY→FLAG publication and FLAG invalidation→ENTRY restoration | `watchdog/a733_rom_return.c` uses DSB both | [Captured ROM/boot0 inventory](vendor-firmware/boot-rom-and-boot0.md) records entry/flag dispatch. Readbacks check stored values; DSB supplies architectural completion between publication steps. No documented fixed hold exists. |
| Maintenance watchdog reload | `watchdog/a733_maintenance_watchdog.c` delegates to `platform/a733/maintenance_watchdog.c:a733_watchdog_hw_reload`: DSB both | Same watchdog register contract; configuration readback and counter guard validate admission and elapsed time. They do not acknowledge restart internally. |

The legacy TWI, watchdog and ROM redirect drivers use the named DSB accessors
without changing their register values, protocol predicates or ownership rules.
Each access adds two fixed barrier instructions and no extra MMIO. Compiling
the same sources with Clang21 at `-O2` changes AArch64 text size from
860 to 1120 bytes for TWI, 652 to 724 for watchdog, and 264 to 388 for redirect.
The total increase is 456 bytes; physical barrier latency is unmeasured. The native
regression compiles these exact three drivers and executes their AArch64
instructions. Posted transactions drain only when the model observes DSB;
separate timed effects hold TWI FLAG/STOP pending for 30µs. Nominal transfer,
arbitration without STOP, stuck STOP without output publication, watchdog
arm/feed/disarm and redirect prepare/restore pass. Replacing actual DSB
instructions with NOP, while preserving the same model hook, fails all three
subsystem cases at the first dependent MMIO access. These constructed delays
establish ordering coverage, without asserting physical peripheral latency.

The affected AArch32 watchdog expiry image also builds within its 2048-byte
SRAM limit, at 1720 bytes. Its instruction checks preserve ownership, refusal,
restoration and failed-restoration retention; the watchdog host check passes
with AddressSanitizer/UndefinedBehaviorSanitizer. The configuration-only
AArch32 diagnostic uses the same DSB accessors, builds at 1096 bytes and
passes its existing instruction refusal/restoration cases. Ordering evidence is under
`~/.cache/sunstoneos/build/model-fidelity-1031/ordering-followup/io/`.

The cached I2S BSP contains two playback sequence differences. In
`i2s/snd_sunxi_i2s.c:sunxi_i2s_dai_prepare`, lines1493–1511, it requests ten
TX flushes, each followed by 1ms, before clearing TXCNT. Capture requests one
flush without that delay. Native startup requests one flush and waits for its
self-clear, but clears the counter before that wait. The existing fixture
establishes the manual flush predicate. Vendor playback sequence parity
remains unverified.

In `sunxi_i2s_dai_tx_route`, lines1525–1543, the BSP disables TX DRQ, flushes
until FIFO free count is 128, waits 250µs, then disables TX and SDOUT. Its
comment identifies pop avoidance. Native stop disables DRQ and direction
before DMA retirement; normal EOF retains resources after observing FIFO
empty. The 250µs workaround is absent, and no source proves that retaining
resources alone reproduces it. These playback divergences require a bounded,
route-specific stop/prepare design before implementation; they are not newly
inferred PLL settle requirements.

The vendor's optional `clk-en-post-delay` at lines1429–1430 defaults to zero
when the property is absent, as lines2394–2399 show. A nonzero property or
runtime mixer setting needs its selected-board configuration before it can
become a native minimum. No selected nonzero value is established here.

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

Removing the controller-reset wait from a temporary SPI source copy fails
the fixture's assertion against writes while reset is pending. Removing the
FIFO-reset wait fails the assertion that polling must preserve pending state.
The committed driver remains unchanged for these mutation checks. Raw sources,
executable hashes and failure logs are in `wait-removal/` below the evidence
directory named below.

PWM configuration records each confirmed PCR observation in the existing
`report.pcr` field. A readiness timeout retains configuration phase10,
deadline error1, the last
observed PCR and the unchanged plan/deadline. Initial busy records `0x800`;
busy after control programming records the planned PCR with bit11 set.
Unexpected control bits retain error11 and the originating observation.
Focused assertions verify that later retained calls preserve these fields.

## Source minimum intervals account for clock rounding

The native provider in `platform/a733/counter_clock.c` converts the 24MHz
counter with integer division by 24. A difference of N floor-rounded
microseconds can represent an actual interval almost 1µs shorter than N.
The source minimum intervals therefore include one unit of rounding margin.

| Sequencer | Source minimum and exact cached reference | Implemented threshold and time origin |
| --- | --- | --- |
| I2S PLL | 3µs lock-sample spacing recommendation and 20µs after three stable samples; manual 4.1.4.1–.2, pp25–27 | Strictly more than 3 and 20 reported microseconds. Each origin follows the completed register access and its time check. |
| RTC calendar | 2ms after access-busy clear; `rtc-native/rtc-sunxi.c`, `wait_bits_cleared`, lines319–337 | Checked 2001µs target from a fresh post-status-read sample. The original deadline remains. A stopped provider fails after 4096 consecutive unchanged samples, resetting the count on progress. |
| HDMI audio | 5µs sampler reset and 5ms CTS calculation; `hdmi-research/kernel/bsp/drivers/drm/sunxi_device/hardware/lowlevel_hdmi20/dw_avp.c`, lines148/297 | Checked 6µs/5001µs targets from the real adapter's post-write time. A target at or after the existing deadline fails before dependent programming. |
| Radio power | 10ms supply before REGON and 50ms after REGON; `radio-platform/sunxi-wlan.c`, line160, and `radio/kernel/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.c`, line430 | Strictly more than 10000/50000µs, anchored after completed pin operations and their fresh time checks. |
| AIC pad configuration/restoration | 1ms after pad writes; the same cached `aicsdio.c`, line1685 | One extra provider tick after observed SDIO command completion. Addition is checked; polling intervals and maximum deadlines remain. |
| Bluetooth patch power entry | 500µs after the type4 writes; `radio/kernel/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.c`, lines719–720 | Round half a millisecond up to provider ticks, then add one tick. The hold begins from a receive timestamp sampled after the triggering write completed. |
| SPI chip select | Caller-defined minimum setup/hold; `include/a733_spi.h`, public transfer contract | Strict elapsed comparison for nonzero minima, with zero remaining zero. Setup starts after the CS write; hold starts after accepted controller completion. |

Native radio sessions require 1000 provider ticks per millisecond, giving a
1µs guard. The generic transport can use a coarser provider: the firmware
fixture uses a 100µs tick, and a 1ms provider can add up to 1ms per hold.
Those conservative guards must fit the original deadlines. No timer API,
operating point, voltage, power policy or polling cadence changed.

The local I2S, UART, SPI and SDHC reset holds use a conservative 10µs software
threshold. The cited sources establish no corresponding hardware minimum;
those thresholds remain unchanged. UART CHANGE_UPDATE, SPI reset, I2S flush
and conversion-ready waits observe hardware status rather than a minimum time.

Focused tests supply independent 24MHz fractional timestamps and charge 17µs
to triggering callbacks. They reject exact rounded endpoints and accept the
next microsecond, while verifying no dependent register write. RTC also covers
frozen time and endpoint refusal. HDMI covers both FIFO and CTS deadline
refusal. The native AIC fixture covers pad configuration/restoration and
deadline refusal; its published-firmware fixture independently checks the
500µs hold at the last fractional phase of a 100µs provider tick.

The radio positive fixture explicitly constructs a BLDO5 1.8V supply. An
independent negative case preserves the fitted-topology blocker: DCDC8
selector 100 is undecoded and fails during rail admission, with its raw
snapshot retained and no radio MMIO or pin acquisition. Constructed supply
evidence does not qualify the fitted board's DCDC8 route.

One execution of the same four focused host closures measured the following
wall times, including process and sanitizer startup. The corrected fixtures
include the additional completion cases; these times are check costs, with no
inference about device throughput.

| Closure | Baseline milliseconds | Corrected milliseconds |
| --- | ---: | ---: |
| SPI | 4.754 | 4.742 |
| UART | 4.243 | 4.669 |
| PWM | 4.268 | 4.774 |
| I2S platform | 4.318 | 4.542 |

Raw logs, baseline source snapshots, executable hashes and timing records are
under `~/.cache/sunstoneos/build/model-fidelity-1031/io/`. Test binaries used
the same compiler and sanitizer flags before and after.

The final minimum guards add fractional-time and callback-cost cases to the
SPI and I2S closures. Their final necessary rerun updates the table above;
unchanged UART/PWM timings are reused. The combined host cost is 18.727ms,
against 17.583ms for the baseline. Process startup and the added cases are
included; these small host timings do not measure hardware performance.
All six changed driver translation units compile for AArch64. The RTC,
HDMI audio, radio and AIC transport sanitizer checks pass; the AIC firmware
check also preserves its independent source transcript and published bytes.

No board was accessed; physical transition latency and peripheral
qualification remain open.
