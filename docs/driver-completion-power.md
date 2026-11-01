# Completion checks in power, memory, storage, PCIe and USB drivers

This source audit covers every tracked file in `drivers/clock`, `drivers/dram`,
`drivers/power`, `drivers/thermal`, `drivers/storage`, `drivers/pci` and
`drivers/usb`, including the USB host subdirectory. It concerns hardware
completion after a write, including delayed register readback. It does not
qualify hardware. A barrier completes the processor's access; a separate status
bit or specified delay establishes the peripheral operation's completion.

The intended reader maintains A733 native drivers and understands MMIO,
interrupts, DMA and deadlines. MMIO means memory-mapped device registers.
The tables group repeated writes that have the same completion contract.
Registers are offsets from the base stated in each row.

## Primary references

References below identify cached primary inputs already pinned in
[`sources.lock.json`](../sources.lock.json), unless a separate manifest is
named. Cached paths are relative to `~/.cache/sunstoneos/sources/`.

| Reference | Identity and relevant location |
| --- | --- |
| Manual | A733 User Manual 1.00, SHA-256 `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`; `camera-research/A733_User_Manual_V1.00.txt`. Sections 4.1.4.4–5, 4.1.6, 4.2, 7.1.5–7, 11.3.6–8, 18.2–4 and 18.11 describe the registers below. |
| Kernel | Orange Pi kernel `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Source filenames and function names below identify the effective implementation. |
| CPU PLL | Kernel `board-kernel/ccu-sun60iw2-cpupll.c`, `ccu_nkmp.c`; common helpers in `gpu-power/drivers/clk/sunxi-ng/ccu_common.c`. Manual §4.2 describes manual PLL updates and the subsequent readback. |
| PCK | Kernel `gpu-power/drivers/pm_domain/pck600_domains.c`, `pck600_power_on`, `pck600_power_off`, and delay programming; SHA-256 `2d9c2b60b3b264d1732538815d12570e45386d30b280a1d0b4f5b9780bdc49ee`. PCK-600 TRM in `gpu-isolation/pck600-trm.pdf` describes policy versus status. |
| PMIC | U-Boot `b791be842935b27268ae3d00e943a9075495f30a`, `drivers/sunxi_power/pmu_axp8191.c`; TWI control/status in Manual §18.11.10.4–5. Regulator settling references are in [power domains](power-domains.md) and [CPU clocks](cpu-clock.md). |
| NSI | Kernel `gpu-isolation/drivers/bus/sunxi-nsi.c`: `nsi_port_setmode`, `nsi_port_setpri`, `nsi_port_setio`, `nsi_pmu_disable`, `nsi_pmu_clear`, `nsi_pmu_enable`, `__nsi_pmu_timer_store_v2`. |
| MMC | Kernel `radio-sdio/bsp/drivers/mmc/sunxi-mmc.c` and `sunxi-mmc-v5p3x.c`: reset, clock update, IRQ completion, voltage switch and DMA teardown. Manual §11.3.8 describes the SMHC registers by offset. SD protocol references are pinned in [UHS design](designs/sd-uhs.md); write references in [MMC](mmc.md). |
| PCIe | Kernel `pcie/kernel/bsp/drivers/pci/controller/allwinner/pcie-sunxi-rc.c`: `sunxi_pcie_prog_outbound_atu`, `sunxi_pcie_host_speed_change`, `__sunxi_pcie_host_init`. Header `pcie-sunxi.h` defines `WAIT_ATU=1` ms and `LINK_WAIT_MAX_RETRIE=20`. Further protocol pins are in [`drivers/pci/source.json`](../drivers/pci/source.json). |
| Combo PHY | Same kernel revision, `pcie/kernel/bsp/drivers/phy/sunxi-cadence-combophy.c`, SHA-256 `84b5471d3b7bbb2ae247b75459135dff0a1d40eef9a2bb994df14eb799b5c082`: `combo0_usb_phy_reset`, `sunxi_cadence_phy_pcie_internal_100m_init`. |
| USB0 | Kernel `usb-platform/bsp/drivers/usb/sunxi_usb/manager/usbc0_platform.c`, `usbc/usbc_phy.c`, `udc/sunxi_udc_board.c`; precise routing and sequence in [USB platform](usb-platform.md). |
| USB host | xHCI 1.2, `usb-host/xhci-1.2.txt`, §§4.2–4.3, 4.6, 4.9 and 5.4–5.5: reset, run, commands, transfer events and ports. DWC3 kernel `usb2-research/drivers/usb/dwc3/core.c`. |
| Type-C | FUSB302 and USB Type-C source manifests linked from [Type-C](typec.md), plus [PD sources](typec-pd-sources.json). Cached `typec-pd/drivers/usb/typec/tcpm/fusb302.c`, `fusb302_get_src_cc_status`, waits 50–100 µs after each comparator change. Reset, comparator, toggle and VBUS status contracts are distinct from I2C transaction completion. |
| Thermal | Kernel `thermal/sunxi_thermal.c`, `sun60iw2_ths_init`, calibration and data-ready handling; SHA-256 `e0b67fa2bbda72d9f226dd910560d9a31169f671b07383c95831edfbe2be2c02`. |

## Clock, power and thermal writes

| File and function | Register or operation | Completion assessment |
| --- | --- | --- |
| `clock/a733_cpu_clock.c`: `S_PARK`, `S_UPDATE`, `S_LOCK`, `S_UNPARK` | CPU PLL base `0x08870000` plus per-domain page; UPDATE bit 26, LOCK bit 28, multiplier N bits 15:8; mux/divider words | **Reported to its owner:** this worktree's initial `S_UPDATE` required target N before testing UPDATE. Manual §4.2 says N readback changes after update. Target N must be checked after UPDATE clears. The 10 ms update, 70 ms lock, three spaced lock samples and 20 µs settling have independent purpose. This audit does not modify that file or its fixtures. |
| Same file: voltage phases | AXP8191 selector write through PMIC provider | Provider requires exact selector readback. CPU owner separately waits the voltage ramp before raising frequency or releasing a lowered rail. Readback alone does not measure analog voltage. |
| `clock/a733_media_clock.c`: `update`, `changes` | CCU `0x02002000`: reset/gates `0x1884,1844,a74,a04,16e4,16c4,150c,158c,58c,5b4,5e4`; module clocks `0x1840,1860,1804,1808,a00,1588` | Generic module programming under Manual §4.1.4.4: reset release, disabled divider/source setup, gate enable. Exact readback verifies configuration. These writes do not target the NSI/MBUS/GPU/DRAM update registers of §4.1.4.5. Parent PLL LOCK and effective gate status are read-only admission checks. No missing update wait found. |
| `power/a733_pmic.c`: `phase`, `await_status`, `transfer` | S_TWI0 `0x07083000`: DATA `+8`, CNTR `+c`, STATUS `+10` | START, address, register, restart, receive and selector-data phases wait INT_FLAG and the expected bus status. STOP waits M_STP clear, idle control and status `0xf8`. Deadline checks surround completed phases and publication. Status readback is independent of command bytes. |
| Same file: `P_WAIT_STOP`, `P_PUBLISH` | PMIC selector readback after write | Publishes only after STOP/idle and exact readback of the selector byte. Voltage settling belongs to the consumer; this provider promises a programmed selector. No missing bus-completion wait found. |
| `power/a733_power_domains.c`: `command` | PCK domain page `0x07060000 + id*0x1000`: delays `+170,+174,+c00,+c04,+c10` | Programs the vendor delay row before a transition, including first OFF. Compares documented delay fields; `+c10` ignores board-observed read-only high bits. Transition acknowledgment comes from the separate status word. |
| Same file: `command`, `advance`, `status` | Policy `+0`, status `+8`, low nibble OFF=0/ON=8 | Waits independent status under a transition deadline; short spin then service polls. Never releases the rail on pending or late OFF. A stale starting status remains pending. Existing model delays status by `ack_us` and exercises stuck status. |
| Same file: supply `verify` | PMIC supply snapshots | Waits startup settling unless separately established, then independent PMIC requests. Publishing verified rail state requires matching identity, enabled output and selector. No domain command can use a pending snapshot. |
| `thermal/a733_thermal.c`: `startup`, `update` | CCU `+fc0,+fe4`; THS `0x02522000`: control, period, filter, calibration, threshold and enable registers | Generic clock configuration and source-defined THS setup use exact readback. THS enable does not promise a conversion result. A later `a733_thermal_read` requires data-ready flags before reading channel data. Manual §7.1 and the effective A733 source supply no additional apply/busy handshake for configuration. |
| Same file: `clear`, `a733_thermal_read`, `a733_thermal_stop` | THS flags `+20,+24,+28,+2c`; sensor/interrupt enables `+4,+14,+18` | Flags are write-one-to-clear; writing zero would not acknowledge them. Stop reads enable state after clearing it. No analog drain or automatic completion is inferred from flag acknowledgment. |

## Memory and storage writes

| File and function | Register or operation | Completion assessment |
| --- | --- | --- |
| `dram/a733_dram.c`: `a733_dram_priority` | NSI port mode `+10`, priority `+14`, selection `+18` | Source-defined direct configuration fields, read back after each changed write. NSI clock registers are read-only here. No clock UPDATE transaction occurs. |
| `dram/a733_dram_load.c`: `start`, `collect` | NSI PMU period `+c8`, enable `+c0`, clear `+c4` | Vendor sequence programs period, disables, clears, enables; collection waits the configured counter interval. No source-defined clear-busy bit was found. Counter-stability comparison detects continued counting and selects disable-before-collect. This diagnostic does not establish an undocumented hardware latch guarantee. |
| `storage/a733_mmc_platform.c`: `registers` | SDC module/bus clocks, GIC disable, reset assertion/release | Waits masked/inactive interrupt state. Reset release requires elapsed floor-counter time greater than 10 µs from the completed writes/readback. The extra counter unit protects the native minimum in [MMC platform](mmc-platform.md). Module writes use disabled-source setup and exact readback. |
| Same file: `core_sd_module` | SDC CCU source/divider/gate | Requires card clock stopped, CMD_LOAD clear and controller idle before gating/reprogramming. Generic clock configuration has no extra apply bit. Controller `sd_clock` waits both card-clock updates. |
| Same file: `core_sd_signal` | PIO `0x02000000 +70` PF selector; voltage status `+48` | Selector readback starts the switch; repeated POLL reads independent bank-voltage status until 1.8 V. The MMC core enforces the switch interval and deadline. |
| Same file: close states | SMHC CLKDIV and CMD update, then gate/reset and pins | Close uses CMD_LOAD completion before dropping clocks or pins. Failure retains ownership. |
| `storage/sunxi_mmc.c`: `setup` | SMHC GCTRL `+0` soft/FIFO/DMA reset bits 0–2 | Repeated polls wait all reset bits clear before setup. Manual §11.3.8.1 defines automatic clear on completion. |
| Same file: `setup`, `sd_clock`, `sd_signal`, tuning recovery | CMD `+18`: CMD_LOAD bit 31 and clock-update bit 21; CLKDIV `+4` | Every update submission has a subsequent CMD_LOAD-clear wait before dependent configuration. Readback then verifies the active card-clock state. Primary protocol tests defer reset and UPDATE by 125 µs; missing-wait mutations fail before dependent writes. |
| Same file: `command_step` | CMD, RINTSTS `+38`, STATUS `+3c`, FIFO | Waits command completion, data completion and automatic-stop completion as applicable; exact byte count and idle/busy/FIFO checks precede success. CMD11 uses the source-defined voltage-switch interrupt. Stale interrupt flags are acknowledged before submission. |
| Same file: DMA command setup/teardown | GCTRL DMA/FIFO reset, DMAC `+80`, IDST `+88`, descriptor OWN bits | Waits both reset engines before submission. Completion requires hardware done flags and released descriptors. Teardown waits IDMAC reset, idle state, DMA-interface reset and FIFO reset before returning CPU buffer ownership. |
| Same file: `sd_signal`, `defer_hold` | CMD11 voltage switch, DAT[3:0], PF voltage; CMD0 pacing | Source/contract holds use a fresh post-operation sample plus the requested delay and one counter unit. This covers 1 ms before/2 ms after CMD0, 1 ms after CMD11, 10 ms clock-off and 1 ms after restart. The 100 µs polling cadence remains unchanged. |
| Same file: write protocol | CMD24 and CMD13 card status | Write completion includes data completion, busy release and subsequent transfer-ready status. The update service additionally reads back and compares the sector. This audit changes no write authority or media operation. |
| `storage/sunxi_sdio.c`: `prepare_step`, `widen_step` | GCTRL reset; CMD clock update | Polls reset and CMD_LOAD clear under the operation deadline. Source/divider changes occur only with card clock stopped. Final readback verifies width, phase and clock. |
| Same file: `issue_step`, `transfer_step` | FIFO reset, CMD, RINTSTS, STATUS | Waits FIFO reset, command response, required data interrupt, exact bytes and write/select busy release. Late error sample precedes acknowledgment and DONE. |
| `storage/sdio_card.c`: command/result state machine | CMD5 OCR ready; CMD52 configuration and readback | Card readiness comes from the response ready bit with a bounded retry interval. Controller completion comes from `sunxi_sdio`; CMD52 readback verifies card configuration. |
| `storage/a733_storage_update.c`: `a733_storage_update_poll` | Granted single-sector write, verification read | Waits platform COMPLETE and error-free result, then compares owned readback bytes before publishing a receipt. No direct MMIO or independent apply register. |
| `storage/fatfs_port.c`: `mmc_read` | Read-only MMC consumer | Waits core DONE/FAULT and verifies result size before copying. It inherits the controller's hardware completion proof. |

## PCIe and USB writes

| File and function | Register or operation | Completion assessment |
| --- | --- | --- |
| `pci/a733_pcie_phy.c`: `a733_pcie_phy_step` | Combo1 PHY reset/configuration; status `0x06c02900` bit 0 | Waits PMA ready at 100 µs intervals before final PHY writes. Enclosing session bounds the wait; vendor internal-100-MHz loop is unbounded. This status is separate from written reset bits. |
| `pci/a733_pcie.c`: `ENABLE`, `RESET_WAIT`, `RESET_RELEASE` | Local clocks, PHY reset, PD22 PERST | Generic clock readbacks verify configuration. PERST hold and post-release configuration access require elapsed floor-counter time greater than 100 ms. The release timestamp follows the synchronous pin callback. Fractional-time and costly-GPIO cases exercise both boundaries. |
| Same file: `TRAIN`, `LINK`, `speed`, `ready` | APP `0x06400000 +c00,+e0c`; DBI `0x06000000 +80c` bit 17; CFG vendor ID | Waits independent link-up status and directed-speed-change self-clear. Endpoint readiness accepts delayed vendor-ID/CRS responses only within the one-second window. |
| Same file: `a733_pcie_atu_program`, `a733_pcie_atu_disable`; `config_read`, `claim`, `stop` | Outbound ATU `0x06300000 + region*0x200 +4`, ENABLE bit 31 | Polls clear before window fields and enable before returning. A program shares one 20 ms budget across both edges; standalone disable has a 20 ms cap. Both preserve the caller deadline and a finite read bound. Origin register, target, start, last observation and failure cause survive retention. |
| `pci/a733_pcie_function.c`: `open`, `quiesce` | ATU region 1 enable/disable, BAR/decode/MSI configuration | Uses the acknowledged shared ATU helpers. A stuck region-1 disable cannot publish client stop or continue root decode/link teardown. Ordinary PCI configuration writes use readback to flush posted access; no independent apply bit is specified for these fields. |
| Same file: SUSPEND/RESUME | PCI power-management control: D3hot/D0 | The native 10 ms contracts in [PCIe](pcie.md) use checked targets 10,001 µs after the successful write's fresh timestamp. Costly fractional PMCSR writes, both exact-boundary refusals and expired-lifetime retention have dedicated cases. |
| `pci/a733_pcie_epf_test.c`: `window`, `WAIT` | ATU region 1; endpoint COPY command/status; MSI status | Window access follows shared enable acknowledgment. COPY waits one interrupt, exact success status, command clear and copied bytes before publication. Completion retains the DMA allocation. |
| `pci/a733_pcie_nvme.c`: `configure`, `ENABLING`, `complete`, `SHUTDOWN_WAIT` | NVMe CC, CSTS, SQ/CQ doorbells and phase tags | Enable waits CSTS.RDY using CAP.TO; queues wait matching completion phase/CID/status; normal shutdown waits CSTS.SHST complete before clearing bus mastering. Doorbell readback is not substituted for command completion. |
| `usb/a733_usb_platform.c`: reference/device states | USB0 CCU `+1340,+1304,+1300` | Reference enable waits 10 µs; device gate/reset path waits 40 µs before core access. A fresh timestamp plus the delay and one counter unit protects these minima against floor quantization. Exact readbacks verify configuration only. |
| Same file: ROM takeover states | SoftConn byte at USB0 `0x04100000 +40`, CCU device reset | Reads SoftConn clear before starting 150 ms detach delay; reset assertion waits 1 ms, release 40 µs. These existing native contracts also receive the floor guard. Detach delay does not prove DMA drain; PIO-only predecessor authority supplies that requirement. |
| `usb/a733_usb.c`: `a733_usb_prepare` | Legacy USB0 reset and route | Uses 150 ms detach and 1 ms reset hold, then barrier. It has no independent soft-disconnect observation or reset-release settling check. Its inherited-ROM assumptions are narrower than the provider's handoff contract; it is not evidence of arbitrary controller drain. |
| `usb/a733_usb2_platform.c`: `CLOCKS`, `PHY_SETUP`, `globals` | USB2/DWC3 local clocks; PHY and DWC3 static fields | Exact readback verifies generic configuration. Controller reset/run completion belongs to xHCI below. No additional apply/busy field found in the selected DWC3 setup. |
| `usb/a733_usb3_phy.c`: `a733_usb3_phy_poll`, `wait_after` | Combo0 reset `0x06c01000` bit 0, PLL request `+c` bit 0, PMA ready `+900` bit 0 | Uses floor-guarded 1 ms intervals before reset/PLL-request readback. If PMA is absent, waits 10 ms, applies the fallback and samples ready after the native 5 ms hold. The vendor instead polls readiness at 5 µs intervals; it specifies no mandatory 5 ms hold. |
| `usb/a733_xhci.c`: reset/run phases | Operational USBCMD.HCRST and USBSTS.CNR/HCHalted | Polls HCRST clear, CNR clear and halted status before DMA setup; Run waits HCHalted clear. Host cases independently defer HCRST by 200 µs, CNR by 800 µs and Run by 250 µs. Missing-wait mutations reject early status, DMA publication or port setup. |
| Same file: `port` | PORTSC.PR/PED/speed | USB2 reset waits PR clear, then enabled port and valid speed. A modeled 50 ms reset and stuck case exercise this wait; removing it prevents nominal enumeration. SuperSpeed admission requires an already active enabled link. |
| Same file: `command_event`, `transfer_event`, `event_step` | Command/transfer ring doorbells and event-cycle bit | Hardware event, completion code, slot, endpoint and command/TD pointer establish completion. Stop/reset/dequeue recovery waits command completion before releasing or retrying requests. Repeated event reads cannot substitute for the hardware cycle transition. |
| `usb/a733_xhci_tinyusb.c`: hub/BOT state machines | Hub port-power/reset status and mass-storage reset recovery | Waits asynchronous control completions, power-good delay, reset status and endpoint recovery. All controller MMIO reaches xHCI. Read-sector publication requires matching transfer result and length. |
| Same file: final PORT_POWER completion | Hub descriptor `bPwrOn2PwrGood`, in 2 ms units | A fresh completion timestamp starts the advertised positive delay plus one counter unit. The first hub status transfer remains deferred until that target. Zero delay stays immediate. The original mount/lifetime deadlines still refuse a late transfer. |
| `usb/a733_typec.c`: RESET, LOOK, `measured`, DEBOUNCE, RISE, DISCHARGE | FUSB302 software reset, toggle status, comparator and VBUSOK | Polls reset clear and toggle completion. The 100 µs comparator interval exceeds the pinned `fusb302.c:1268,1284` minimum of 50 µs, so floor quantization does not violate that minimum. The 120 ms debounce also has margin. Discharge requires comparator-safe voltage and VBUSOK clear before OFF. |
| `usb/a733_typec_pd.c`: send/receive/policy | FUSB302 FIFO and USB PD messages | FIFO submission starts a bounded GoodCRC wait; accepted request and VDM replies have independent deadlines. PS_RDY follows the 35 ms transition interval. These responses acknowledge transmitted protocol messages. |

## Files without independent hardware-completion sites

These files were included in the audit. Their role explains why they add no
separate apply, busy, lock, ready or settling requirement.

| Directory | Files and role |
| --- | --- |
| `drivers/clock` | `a733_clock_leases.c`: software ownership; `a733_media_clock_native.c`: checked MMIO transport for the media provider; `a733_cpu_cycles_native.c`: architectural PMU enable/readback with ISB, no asynchronous peripheral completion. |
| `drivers/dram` | `a733_dram_copy.S`: CPU memory copy; `a733_dram_decode.c`: refuses unsupported decode without accesses. |
| `drivers/power` | `a733_pmic_native.c`, `a733_power_domains_native.c`: checked transport and barriers for the provider sites above; `a733_pmic_consumer.c`: snapshot/lease client; `a733_rail_leases.c`: software ownership; `a733_perf_mode.c`: delegates complete operating-point operations to the CPU/GPU/NPU owners. |
| `drivers/thermal` | `a733_thermal_native.c`: checked MMIO transport; conversion readiness remains in the provider. |
| `drivers/storage` | `a733_mmc_platform_native.c`: access policy and MMIO/FIFO transport; `storage_digest.c`: hashing; `storage_update_usb.c`: CPU channel framing. |
| `drivers/pci` | `a733_pcie_native.c`: access policy/transport; `source.json`: reference manifest. |
| `drivers/usb` | `a733_usb_platform_native.c`, `a733_usb2_native.c`: checked transport, including analog configuration readback; `diagnostic.c`: TinyUSB protocol consumer; `a733_usb.h`, `a733_usb3_phy.h`, `usb_service.h`, `tusb_config.h`: declarations/configuration. |
| `drivers/usb/host` | `hid.c`, `serial.c`, `serial-native.c`, `serial-control.inc`, `uvc.c`: class/protocol consumers of asynchronous xHCI transfers; `hub.patch`, `superspeed.py`, `serial-source.py`, `cadence-source.py`: source adaptation; `tusb_config.h`: configuration. Hub power/reset completion is covered by the host state-machine row above. |

## Model evidence and remaining gaps

- CPU PLL fixture changes belong to the CPU owner. This audit reports the
  initial stale-N gap and supplies no independent claim about its final model.
- PCIe models defer ATU0/1 transitions, PHY readiness and link-up. The complete
  focused batch passes 43 cases in `power/pcie-latency-3/`. Added failure-input
  checks pass in `power/pcie-diagnostics/`. A 4 ms disable plus 15 ms enable
  succeeds in 19,035 modeled µs; two 12 ms edges fail after 20,008 µs. A stopped
  clock ends at 1,048,576 reads without subsequent address writes.
- The primary MMC protocol model defers reset and clock UPDATE and covers both
  stuck conditions. Removing either wait fails its dependent-write assertion.
  Extracted card-model consumers retain their existing timing and need separate
  delayed completion work if they claim that coverage.
- USB reset/run/port model cases pass in `power/usb-latency-final/`. Removing
  each of the four waits fails an ordering assertion or nominal enumeration.
  The final SuperSpeed model starts with PMA readiness clear and schedules its
  assertion 200 µs after the PHY trigger. Nominal cases observe pending status;
  the stuck case retains before controller publication.
- `checks/power-domains-model.h` already separates policy from delayed status.
  `checks/a733-pmic.c` already supports timed wire events. Preserve these
  meaningful models while adding a shared deferred-register mechanism.

Evidence paths above are relative to
`~/.cache/sunstoneos/build/model-fidelity-1031/`. PCIe Gen1 traces before and after
the correction are identical: 157 writes for root configuration, 43 for the
endpoint-copy path and 45 for its 64-bit BAR variant. A declared single host
execution of each closure takes 6.4–7.5 ms; these are fixture execution times,
not board performance measurements. Inputs, traces and timings are in
`power/pcie-final/`; final-source trace comparisons are in
`power/pcie-final-traces/`.

The [minimum-hold check](../checks/power-hold-boundaries.py) executes the native
USB0, USB3 and MMC target helpers after a modeled 625.023 µs trigger callback
with a fractional counter origin. PCIe and MMC platform composition checks
exercise their actual phase boundaries and costly GPIO/register callbacks.
These checks verify software sequencing; physical delay and peripheral
qualification remain outstanding.

The guarded-delay evidence is in `power/hold-boundaries-2/`,
`power/pcie-hold-1/` and `power/mmc-hold-1/`. The UHS-enabled core and platform
models and AArch64 builds pass in `power/mmc-uhs-hold-1/`. USB0 source comparison,
host composition and native MMIO checks pass in `power/usb0-hold-2/`.
SuperSpeed read and PMA-timeout cases and six native objects pass in
`power/usb3-hold-1/`. These checks reuse earlier unaffected protocol evidence.
The final hub/PMA batch passes seven cases and six native objects in
`power/hub-pma-final/`. Both PCIe PM transition modes, nominal NVMe and stuck
ATU1 shutdown pass in `power/pcie-pm-hold-2/`.
