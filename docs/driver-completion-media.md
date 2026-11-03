# Media register completion audit

This audit covers every file in `drivers/camera`, `drivers/display`,
`drivers/video`, `drivers/deinterlace` and `drivers/g2d`: 45 files, comprising
14 camera, 17 display, ten video, two deinterlace and two G2D files. It separates a stored
configuration readback from an observed hardware transition. The reader is
assumed to understand MMIO, DMA and deadline-based driver state machines.
All conclusions are source inspections or host models; physical completion and
output remain unqualified.

## Minimum waits account for rounded-down timestamps

The native counter adapter divides 24 MHz ticks by 24 in
`platform/a733/counter_clock.c:a733_counter_clock_now`. Its timestamps round down.
Waiting until `now_us >= sampled_us + delay_us` can therefore finish almost
1 µs before the requested interval has elapsed. Integer-time fixtures alone do not expose this phase within a microsecond.
Confirmed minimum waits now require one additional reported microsecond while
keeping their original absolute deadlines. The table identifies each corrected
minimum and the unchanged intervals whose meaning differs.

| Site | Required or source-selected interval | Current treatment |
| --- | --- | --- |
| `a733_display_pll.c:delay` | 5 µs lock sample spacing; 20 µs after N write | Filter targets use 6 µs and the N-write target uses 21 µs. The reported 25 µs settle target guarantees the source's 20 µs physical minimum despite rounding. |
| `a733_dsi_phy.c:wait_for` | 5 µs lock sample spacing, 20 µs lock settle, 1 µs analog settle | The targets use 6, 21 and 2 reported microseconds respectively. |
| `a733_hdmi_phy.c:a733_hdmi_phy_poll` | 20 µs TOP lock settle; 5 µs SVSRET reset pulse and TX lock sample spacing | The targets use 21, 6 and 6 reported microseconds respectively. The existing lock timeout remains separate and unchanged. |
| `a733_hdmi_phy.c:phy_transaction` | 10 µs source-selected status poll spacing | The target uses 11 reported microseconds, clipped to the original transaction deadline. Completion still requires DONE; expiry refuses further status reads. |
| `a733_hdmi.c:delay_set` | 50 ms TCON stabilization before output start | The existing 50 ms reported target is unchanged. An explicit source minimum remains unestablished; this interval is not claimed as a qualified physical hold. |
| `a733_display_framebuffer.c:cold_step`; `a733_video_platform.c:prep_step` | 10 µs external reset hold | Both targets use a checked 11 reported microseconds. Video reset follows pinned `ccu_reset.c:ccu_reset_reset`, which specifies 10 µs. The framebuffer interval remains an existing native minimum-hold policy; a vendor minimum is unestablished. |
| `ov5640.c:delay` | 5 ms power, 1 ms PWDN, 20 ms reset, table delays in milliseconds, 500 µs final timing control | The helper checks overflow and adds one reported microsecond after each triggering callback completes. Extra callback or scheduler latency supplies no assumed margin. |
| `a733_dp_cadence.c` waits through `apps/displayport/native.c:wait_io` | Source-selected 5 µs, 10 µs, 1 ms and 10 ms intervals; controller AUX/training intervals | The native callback requires the reported difference to exceed the requested interval and refuses admission without room before the original deadline. All current callers request positive intervals. |
| `a733_camera_capture.c:a733_camera_capture_poll` | Quiet interval of at least three frame periods, rounded up | The reported elapsed interval must exceed the rounded-up duration. This guarantees the actual three-frame minimum. The separate `status_us` interval subtracts 2 µs and remains conservatively early for its delivery deadline. |

The quiet duration is `Q = max(ceil(3 × htotal × vtotal × 1000000 / pixel_hz),
100000)` microseconds. It uses the exact derived frame period before rounding.
A reported difference greater than Q means at least Q + 1 whole reported units;
the actual elapsed interval therefore exceeds Q, including the unknown fraction
of the starting microsecond. The quiet origin remains the receiver's completed
sample timestamp.

The focused fixture keeps true time in 24 MHz ticks and rounds only the driver's
clock observations. It tests starting phases of 0, 1 and 23 ticks, hardware
callbacks that take 37 ticks, refusal at the original deadline and physical
minimum intervals before the next modeled action. The reset helpers execute
verbatim production functions with explicit admission and MMIO models. These
checks supply software evidence; they do not measure physical timing.

## References identify the completion contract

The A733 User Manual V1.00 is pinned in [sources.lock.json](../sources.lock.json).
Its cached text is `~/.cache/sunstoneos/sources/camera-research/A733_User_Manual_V1.00.txt`.
The following short source names identify cached source roots; component
manifests supply their revision, URL, hash and license.

| Name | Cached root under `~/.cache/sunstoneos/sources/` | Pin |
| --- | --- | --- |
| Camera | `camera/bsp/drivers/vin/` | [Camera source record](camera.md), [ISP0 manifest](../drivers/camera/isp0-sources.json) |
| Display | `display/bsp/drivers/drm/` | [Display source pins](../sources.lock.json) |
| HDMI | `hdmi-research/kernel/bsp/drivers/drm/sunxi_device/hardware/lowlevel_hdmi20/` | [HDMI source pins](../sources.lock.json) |
| DP | `displayport-native/bsp/drivers/` | [DisplayPort manifest](../apps/displayport/sources.json) |
| Cedrus | `hevc-decode/cedrus_regs.h`, `hevc-decode/cedrus_h265.c`, `h264-native/cedrus_h264.c` | [HEVC manifest](../apps/hevc/sources.json), [H.264 manifest](../apps/h264/sources.json) |
| Encoder | Binary-derived register inventories and source comparison | [JPEG encoder manifest](../apps/jpeg-encode/sources.json), [H.264 encoder manifest](../apps/h264-encode/sources.json) |
| DE | Display `sunxi_device/hardware/lowlevel_de/de_top.c` | [Display source pins](../sources.lock.json) |
| G2D | `g2d-assessment-20261020/g2d_rcq/` | [G2D manifest](../apps/g2d/sources.json) |
| DI | `deinterlace-native/bsp/drivers/di/drv_div3x/lowlevel_v3x/di301/` | [DI manifest](../apps/deinterlace/sources.json) |

## Native providers complete writes before dependent operations

A Data Synchronization Barrier (`DSB SY`) establishes architectural completion
of preceding explicit memory accesses before execution continues. A volatile access or Data Memory Barrier
(`DMB`) alone does not establish write completion. Completion of a CPU write
also does not establish a device's internal transition, DMA drain or output
visibility; those still require the status and ownership conditions below.

The following paths reach actual native providers, including application-owned
adapters. `both` means `DSB SY; access; DSB SY`; `after` means
`access; DSB SY`, as implemented in `include/a733_mmio.h`. Each listed native
callback reaches architectural write completion before it returns. Completion
follows the admitted memory type: Device-nGnRE permits early acknowledgment
before endpoint arrival. Models must represent any remaining propagation
separately. A timer sample after the required device visibility point can
anchor an inter-write minimum. Host replacements of these providers need
separate ordering tests.

| Completion-sensitive sites | Effective native provider and barrier | Pinned source order and delay |
| --- | --- | --- |
| DISPLL enable/UPDATE, N, dividers and second UPDATE | `apps/dsi-scanout/native.c:pll_write` → `platform/a733/display_native_io.c:a733_display_native_write`, `both`. The driver also calls the native `barrier` after every write before readback. | Display `phy/sunxi_dsi_combophy_reg.c:displl_clk_enable/displl_clk_set`: enable fields → UPDATE → lock samples; N → 20 µs → dividers → UPDATE. Native reported targets are 6 µs between lock samples, 21 µs after N and 25 µs after lock. The N delay begins after write completion and readback. |
| DSI PHY analog enable, host enable, instruction packet/graph, low→high instruction start and video exit graph | `display_scanout.c` → `a733_display_native.c` → `platform/a733/display_native_io.c`, `both`; panel command callbacks in `apps/dsi-scanout/native.c` reach the same provider. Explicit driver barriers also precede host enable and instruction start. | Display `sunxi_dsi_io_open`, `dsi_v1.c:dsi_start/dsi_close`: preserve field and graph order, source 1 µs analog minimum, 5 µs lock spacing and 20 µs settle. Reported targets are 2/6/21 µs. PHY analog shutdown, LVDS and SSC source delays belong to unimplemented paths; they are not implied by this start sequence. |
| DE enable, double-buffer READY, RCQ address/length and submit | Native scanout uses `display_native_io.c`, `both`; framebuffer and DP setup use `a733_display_framebuffer.c:raw_write`, `both`. Framebuffer replay delegates through `apps/gpu-display/display.c` to `a733_display_native_rcq_submit`. That helper uses plain busy/control reads and one update write within a task critical section, then `DSB SY` before leaving. | DE `de_top.c:de_top_set_double_buffer_ready/de_top_set_rcq_update`: publish configuration/header before update; no selected inter-write delay. RCQ busy eligibility and fresh finish/frame-end observations remain necessary. |
| LCD/TV TCON timing and channel enable; HDMI TOP/TX reset and PLL; PHY I2C and DDC requests | LCD uses the display provider above. TV/HDMI use `platform/a733/hdmi_native.c:write8/write32`, `both`; `a733_hdmi_phy.c:a733_hdmi_write` additionally invokes the native `barrier` after every write. | HDMI `phy_top.c:top_phy_config` requires 20 µs after lock; `dw_phy.c` requires 5 µs SVSRET pulse, 10 µs I2C poll spacing and 5 µs TX lock spacing. Native guards are 21/6/11/6 µs. The separate 50 ms TCON warmup has no established source minimum. |
| DP reset release, clock/power requests, voltage/pre-emphasis and AUX/training requests | `a733_dp_cadence.c` → `a733_dp.c` → `apps/displayport/native.c:raw_write`, 16- or 32-bit `both`; explicit `barrier` issues `DSB SY`. | DP `phy/sunxi-cadence-combophy.c`: reset stages and selected requests retain 1 ms holds, status polling 5 µs, level changes 10 µs and AUX analog setup 10 ms. `wait_io` starts after the completed operation and requires elapsed reported time greater than its argument. Controller AUX/training spacing remains explicit. |
| Camera analog reset/reference/LDO/calibration; CSI parser capture; VIPP parameter address→READY; DMA address/enable/acknowledgment | `a733_camera_native.c:analog_write/csi_write/route_write/dma_write` → `write_word` → `store`, 32-bit `both`. Model-only load/store hooks bypass this assembly and cannot prove it. | Camera `combo_csi_reg.c`, parser setters, `vipp200_reg.c` and `dma140_reg.c` preserve their selected setter order. Calibration FLAG/result, VIPP load and DMA events supply separate completion; these low-level selected setters establish no additional fixed inter-write delay. |
| Sensor power/reset/PWDN lines and sensor register/table writes | `a733_camera_native.c:sensor_line` → `a733_pins_write_outputs` → `a733_pins_native.c`, `both`. `sensor_bus` drives `a733_twi_poll` through `a733_twi_native.c:store`, `both`, and returns only after transaction completion. | Upstream `ov5640.c` power/mode helpers retain 5 ms power, 1 ms PWDN, 20 ms reset, table delays and 500 µs mode settle. `ov5640.c:delay` anchors its guarded target after each completed callback. DSB completes controller writes; TWI completion establishes the sensor transaction boundary. |
| ISP0 prepare and retained rearm: load addresses→ISP/VIPP READY→TOP GEN→START | `apps/isp0/native.c:engine_write/dma_write` → `store`, `after`; reads use `both`. Consecutive GEN and START callbacks each complete their write. Rearm clears observed status, refreshes the load address and READY before a separate trigger. | Camera `top_reg.c:csic_ptn_generation_en`, ISP600 `sunxi_isp.c` PARA_LOAD handling and `isp600_reg_cfg.c` address/update setters, pinned by `isp0-replay-sources.json`. The selected setters contain no fixed inter-write delay. ISP/VIPP load and TOP START clear remain independent completion conditions. |
| ISP0 device interrupt enables→GIC rearm and interrupt mask→owner notification | `apps/isp0/irq.h` uses the preceding native `store`, `after`, before `gicv3_spi_rearm`; `drivers/gpu/gicv3_spi.c` fences distributor writes with `DSB SY`. | ISP600 SAVE/LOAD, VIPP load and DMA capture bits are pinned by the replay manifest. The adapter waits for GIC RWP/inactive state, clears owned pending state and programs device enables before rearm. These barriers do not replace deferred GIC status checks. |
| VE PLL factors/enable/lock-enable→LOCK→output gate; module clocks and external/local reset | `a733_video_platform.c` and `a733_video_encode_platform.c` native operations → `a733_video_unit.c:a733_video_unit_native_write`, explicit `DSB SY` before access and after write; reads use `both`. | Pinned `ccu_gate.c/ccu_nm.c`, manual §§4.1.4.1–2 and `ccu_common.c:ccu_helper_wait_for_lock`: three LOCK samples at 3 µs spacing, then 20 µs. Shared `a733_pll_lock.h` guards these with 4/21 reported µs. `ccu_reset.c:ccu_reset_reset` holds external reset 10 µs; native target is 11 µs. |
| H.264/HEVC VLD flush→busy check, decode trigger→status; JPEG decode/encode and H.264 encode reset/trigger/header command→status | Decoder platform adapters and encoder platform adapters reach the preceding video-unit native writer for byte or word accesses. Each reset pulse, flush and trigger write reaches `DSB SY` before a dependent write or poll. | Cedrus flush/trigger helpers and the pinned encoder register inventories define command order. Busy/success/error observations below remain required. No additional inter-write minimum is established by the selected codec references; binary-derived register order alone cannot establish an omitted delay's absence. |
| DE_SYS power request, framebuffer reset, shared media clocks and IOMMU publication | Framebuffer `raw_write`, `both`; shared `a733_media_clock_native_write` uses `DSB SY` before its ownership read and after its write. G2D domain-row callbacks use its native `store`, `after`. | Pinned PCK600 delay-register programming precedes ON request and status polling. Delay-register values are hardware configuration, not elapsed CPU holds. Framebuffer's external reset target is 11 reported µs under its existing native 10 µs policy; no vendor minimum is established. |
| G2D RCQ address/length→enable; task-end acknowledgment→mixer reset low→high→output copy | `apps/g2d/native.c:store`, `after`; readback uses `both`. The application fences each bounded copy as well. `drivers/g2d` builds immutable RAM commands and performs no MMIO. | Pinned `g2d_rcq/g2d_top.c` submit/reset helpers and selected successful IRQ path preserve acknowledgment and reset order before completion delivery. No selected reset hold is specified. CPU barriers and the success-path reset supply no independent fabric-drain proof. |
| DI stale finish acknowledgment→masked configuration→START; following-job acknowledgment before new configuration | `apps/deinterlace/native.c:store`, `after`; readback uses `both`. `drivers/deinterlace/di-port.h` volatile accessors address RAM images through `di_dev_set_reg_base`, not physical registers. | DI301 configuration and `di_driver.c:894–920` successful completion preserve job ordering. Native code retains the finish latch through copy and acknowledges it before the next job. The selected RAM setters require no delay; the unused source lifecycle delay is refused by `di_port_violation`. |
| Page tables/parameter RAM/input publication before media triggers | Camera uses `aarch64_device_cache`; display/framebuffer, codec, ISP0, DI and G2D publication adapters use `DSB SY` after preparing their admitted RAM. Subsequent trigger writes use the providers above. | Existing DMA/cache and IOMMU contracts remain required. A barrier completes CPU publication; it cannot establish ownership, mapping validity or an accelerator's completed writes. |

No missing native completion barrier or lost source inter-write delay was
confirmed in these selected implemented paths. This is source inspection,
without a physical ordering claim. Unsupported shutdown, LVDS and SSC paths,
codec binary timing and the existing drain gaps remain outside that conclusion.

## Display completion sites

Offsets below are relative to the named block. Repeated ordinary configuration
writes in a table share its listed contract; they have no independent documented
completion indication.

| File and function | Register or operation | Reference and required observation | Finding |
| --- | --- | --- | --- |
| `a733_display_pll.c:a733_display_pll_poll` | DPHY PLL `0x104` UPDATE; `0xe0` LOCK bit26 | Display `phy/sunxi_dsi_combophy_reg.c:displl_clk_enable`, `displl_clk_set`; manual §15.5.5.2 p791 | Three consecutive lock samples use a 6 µs reported interval followed by a 25 µs target. N programming has a separate 21 µs target. These guards preserve the source's 5/20 µs minima. Whole-word UPDATE readback assumes a persistent bit. The available source explicitly clears it at disable but supplies no self-clear contract; do not import CPU-PLL UPDATE semantics. |
| `a733_dsi_phy.c:a733_dsi_phy_poll` | Inherited PLL LOCK `0xe0[26]`; analog table | Same source `displl_clk_enable`, `sunxi_dsi_io_open` | Three consecutive samples use 6 µs reported spacing and 21 µs settle. Analog `0x118` programming precedes a 2 µs target, guaranteeing its 1 µs physical minimum. Other fields receive stable readback only. |
| `a733_dsi.c:a733_dsi_poll` | DSI `0x10[0]` instruction enable | Display `sunxi_device/hardware/lowlevel_lcd/dsi_v1.c:dsi_start`, `dsi_inst_busy` | Writes low then high, then polls the self-clearing bit until zero under the request deadline. Packet length and instruction graph have separate readbacks. |
| `a733_dsi_video.c:a733_dsi_video_poll` | DSI `0x4c[28]` conditional jump; `0x10[0]` | Same source `dsi_close`, `dsi_inst_busy` | Stop requests the exit graph and polls instruction enable clear. Start requires enable set and checks debug errors. A stored jump readback alone does not prove the exit. |
| `a733_display_engine.c:a733_de_poll` | DE output `0x814c[0]` double-buffer request; `0x8140[0]` enable | DE `de_top.c:de_top_set_double_buffer_ready`, `de_top_set_rtmx_enable` | The source writes double-buffer ready but its completion helper says that bit is unreadable and requires vblank. Native start now excludes only READY bit0 from stable verification; other fields remain checked. ARMED permits downstream TCON/DSI/DP startup; it establishes configuration, without fresh frame or drain evidence. |
| `a733_display_engine.c:replay_poll` | DE `0x8150` RCQ submit; `0x8144[2]` RCQ finish, `[0]` frame end | DE `de_top.c:de_top_set_rcq_update`, `de_top_check_display_rcq_update_finish_with_clear`, `de_top_query_state_with_clear` | Clears stale RCQ finish, waits for new finish, checks the selected address, clears frame end until a clear sample, then waits for a later frame end. The native submit callback separately handles RCQ busy. This does not authorize old-slot reuse. |
| `a733_display_framebuffer.c:cold_step`, `a733_display_framebuffer_prepare` | CCU reset assertion, DE domain ON, DE local clocks/resets | [G2D resource sequence](g2d.md#resources-remain-owned-through-every-outcome); DE `de_top.c` | Uses a guarded 11 µs reported target after reset assertion. Domain service polls ON status. Local stable bits use readback. Domain and clock changes remain under their existing reviewed contracts. |
| `a733_hdmi_phy.c:a733_hdmi_phy_poll` | HDMI TOP PLL `0x20`; `0x40[0]` lock | HDMI `phy_top.c:top_phy_config` | Polls lock within 20 ms, confirms twice, uses a guarded 21 µs target before gate opening. TX reset low uses a guarded 6 µs target. |
| `a733_hdmi_phy.c:phy_transaction` | PHY I2C command `0x3026`; W1C status `0x108` | HDMI `dw_phy.c:_dw_phy_i2cm_write`, `_dw_phy_i2cm_read` | Clears stale flags, issues each command, polls DONE/error with a guarded 11 µs reported spacing and the existing 5 ms budget, then reads and compares internal data. Error takes precedence over DONE. |
| `a733_hdmi_phy.c:a733_hdmi_phy_poll` | TX PHY enable `0x3000[3]`, lock `0x3004[0]` | HDMI `phy_snps.c`, `dw_phy.c` lock wait | Uses guarded 6 µs reported sample spacing to preserve the 5 µs minimum. Tick-paced confirmation can extend beyond the source's 2.5 ms interval; the outer deadline still bounds the operation. Existing fixtures exercise 1 ms tick phases and missing lock. |
| `a733_hdmi.c:edid_read` | DDC command `0x7e04`; W1C status `0x105` | HDMI `dw_i2cm.c:_dw_i2cm_read` byte transactions | Clears flags before every byte; waits for DONE/error with a 10 ms transaction limit, then reads data and checks HPD. |
| `a733_hdmi.c:a733_hdmi_poll` | Output mute/start/stop and HPD | HDMI `dw_avp.c`, `dw_fc.c`; manual §15.3 | Source delays and TCON sequencing are explicit. Stored mute/enable fields and HPD do not establish serializer drain; resources remain retained. |
| `a733_tcon_lcd.c:a733_tcon_lcd_poll` | LCD CPU IF `0x60`; global/channel enable | Manual §§15.1.10.15, 15.1.10.29–33; Display `lowlevel_lcd/tcon_lcd.c` | Excludes read-only FIFO counters and write-one trigger bit from RMW verification. Checks underflow after start/stop. No implemented trigger-start write requires a completion poll here. Stop preserves the external drain requirement. |
| `a733_tcon_tv.c:a733_tcon_tv_start/stop` | TV `0x90[31]` channel enable | Manual §15.2.10; HDMI TCON source | Stable enable readback is implemented; no independent drain status is established. Health checks preserve underflow/lost-data flags. |
| `a733_dp_cadence.c:combo0_dp_phy_reset` | PHY reset release, READY `0x900[0]`, clock ACK `[4]`, power ACK `[13:8]` | DP `phy/sunxi-cadence-combophy.c:combo0_dp_phy_reset`; manual §§18.2.6.9–10 pp1159–1160 | Reset stages wait 1 ms. READY/clock ACK poll every 5 µs for 1000 attempts. A0 acknowledgment is polled for up to 100000 attempts and must equal 1. The corrected predicate rejects stale A2=4; other request writes and bounds are preserved. |
| `a733_dp_cadence.c:combo0_dp_set_sw_pre`, `a733_dp_cadence_prepare` | PHY level changes; AUX analog setup | Same DP source functions | Preserves 10 µs source waits and the final 10 ms analog settle. Register constants do not establish electrical level or frequency. |
| `a733_dp.c:aux` | AUX command; status, reply counter and byte count | Manual §15.4.7 AUX registers; DP `drm/sunxi_device/hardware/lowlevel_edp/trilinear_dp14/trilinear_dp14.c` AUX routines | Requires an incremented transaction counter, reply received and both busy bits clear. Polls at 20 µs with 2 ms limit; only DEFER retries, at 400 µs, at most eight attempts. |
| `a733_dp.c:training` | Receiver training status after source/receiver changes | Manual §15.4.5.1; DP `sunxi_device/sunxi_edp.c` training | Honors DPCD interval or 100/400 µs minimum; checks every lane and alignment. Source start/stop enable writes have no independent drain proof. |

## Camera and codec completion sites

| File and function | Register or operation | Reference and required observation | Finding |
| --- | --- | --- | --- |
| `a733_camera_platform.c:a733_camera_platform_poll` | COMBO TOP calibration `+4` FLAG/result | Camera `vin-mipi/combo_csi/combo_csi_reg.c`; manual §§16.1.10.1–2 pp885–887 | Polls FLAG=1 and result=16 after software calibration; preserves automatic RESETN and checks writable fields independently. Deadline bounds a missing result. Analog power fields receive stable readback only. |
| `a733_csi_receiver.c:program`, `a733_csi_poll` | Parser capture `0x0c` SCAP/VCAP; status `0x44`; MIPI status `0x118` | Manual §§16.1.12–13; Camera parser and combo CSI setters | Configuration and image rearm require capture readback set. SCAP can clear on image completion; a frame completing before readback would be refused. No late-readback acceptance is defined here. Stop producer/drain remains a callback contract. |
| `a733_camera_route.c:program`, `a733_camera_route_poll` | VIPP READY/capture `0x40`; load interrupt `0x30[8]` | Camera `vin-vipp/vipp200/vipp200_reg.c:__vipp_set_para_ready`, capture setters | READY is excluded from stable verification. Capture disable polls until clear without repeating its write. Final status requires the parameter-load indication; no physical frame or drain is inferred from it. |
| `a733_camera_dma.c:a733_camera_dma_poll`, `stream_poll` | BK `0x254` capture/frame flags, `0x24c[0]` capture busy; next addresses | Manual §§16.1.14.27, 29, 35, 37, §16.1.6.7 | Single capture waits for capture done; final status rejects active capture. Streaming acknowledges observed events, updates next-slot addresses after frame start and checks racing frame starts. Source-defined frame boundaries govern slot ownership. |
| `a733_camera_capture.c:a733_camera_capture_poll` | Parser fence `0x0c`, VIPP load status, quiet interval | Same parser/VIPP sources; OV5640 stop sequence | Coordinates leaf completion and requires capture bits clear before readout. Timing comes from selected sensor mode. Native drain callbacks are admission-backed placeholders, so this supplies no physical independent drain evidence. |
| `a733_isp0.c:a733_isp0_poll/trigger/complete/rearm` | ISP `0x20` READY, VIPP READY, TOP pattern `+4[4]` START; ISP/VIPP status | Camera `vin-isp/isp600/isp600_reg_cfg.c`, `top_reg.c` pattern setters; manual §16.1.9.2 | Excludes auto-clearing READY from stable readback. START is written once. Completion requires ISP flags `0xc`, VIPP flag `0x100`, START clear and no unexpected flags under the outer deadline. Retained rearm acknowledges those observed flags, refreshes the load address and ISP/VIPP READY, then permits a separate trigger. It retains the same input, mappings and output backing. |
| `ov5640.c:ov5640_poll`, `configure` | Sensor reset/power lines, `0x3008` reset/standby, `0x4202` stream control | Camera `modules/sensor/ov5640.c`; upstream `drivers/media/i2c/ov5640.c` power/reset and mode tables | Preserves 5 ms power, 1 ms PWDN, 20 ms reset, table delays and 500 µs final timing control with one additional reported microsecond per minimum. Stream writes have explicit readbacks. Stored stream control alone does not prove that an in-flight frame has left the receiver. |
| `a733_video_unit.c:a733_video_unit_pll_start/wait/open` | CCU PLL_VE0 `0x220`, LOCK bit28 | Manual §§4.1.4.1–2, 4.1.6.36; pinned `ccu_gate.c`, `ccu_nm.c` | Masks read-only LOCK from writes and readback. Shared lock helper qualifies lock before output gate opens; inherited rate admission remains separate. No new operating point is proposed. |
| `a733_video_platform.c:cold`, `prep_step` | PCK request/status; local/CCU reset pulse | Manual power controller; pinned `cedar_ve.c`, package reset policy | ON acknowledgment is polled under the deadline. External reset uses a guarded 11 µs reported target for the source-defined 10 µs hold; local reset follows source writes. Existing reset authority requires prior drain. |
| `a733_video_h264.c:a733_h264_poll` | Trigger `0x224`, status `0x228` VLD busy and completion | Cedrus `VE_H264_STATUS_VLD_BUSY`, `cedrus_h264_skip_bits`, decoder trigger/IRQ routines | Each flush waits for VLD idle; decode requires success and every declared busy bit clear, then macroblock count before copy. Error/data request wins over success. |
| `a733_video_hevc.c:a733_hevc_poll` | Trigger `0x534`, status `0x538` VLD and engine busy | Cedrus `VE_DEC_H265_STATUS_*`, `cedrus_h265_skip_bits`, setup/trigger routines | Flush waits for bit8 clear. Decode requires success with all declared busy bits clear and CTB count 80 before copy. Stale flags are cleared before setup. |
| `a733_video_jpeg_run.c:a733_video_jpeg_poll` | Trigger byte `0x118`, MPEG status `0x11c` | Pinned JPEG package register trace; Cedrus MPEG status definitions | Refuses stale status before trigger and waits for success, rejecting error/data-request flags. It does not require all initial busy fields clear at success. Retained output therefore still depends on admitted completion/drain authority; a decoder-specific documented busy contract is a remaining gap. |
| `a733_video_jpeg_encode.c:a733_jpeg_encode_poll` | ENC reset `0x804[24]`, trigger `0xb18`, status `0xb1c`, bit count `0xb90` | Encoder `JpegEncEncode`/ENCPP register inventory | Pulses local reset, enables engine, clears and rereads stale flags, triggers once, waits for status low nibble exactly 1, validates byte count before copy. No independent writeback-drain bit is established. |
| `a733_video_h264_encode.c:a733_h264_encode_poll` | Same reset/status; header-ready `0xb1c[9]`, header command `0xb18=0x601` | Encoder `h264EncFrame` inventory | Later pictures wait for header-ready before header command. All pictures wait for success and valid ring cursor before copy. No separate command-complete wait follows `0x601`; the pinned effective command sequence is the current contract. |

## Files without a separate hardware completion site

| Directory | Files and boundary |
| --- | --- |
| `drivers/camera` | `a733_camera_native.c` validates admission, locks, mappings and allowed accesses; it delegates completion to the leaf drivers. `isp0-sources.json` and `isp0-replay-sources.json` record native source provenance. `isp0-reference-sources.json` identifies the optional host FFmpeg comparison and establishes no ISP completion or pixel-equivalence contract. `isp0-replay.h` declares retained rearm and its exclusion/lifetime conditions. `isp600_default.h` and `ov5640_tables.h` contain parameter data. |
| `drivers/display` | `a733_display_native.c` delegates DE RCQ busy/submission to its native helper. `a733_display_handoff.c` transfers ownership and polls leaf services. `display_scanout.c` orders leaf operations and refuses unsupported drain proof. `a733_dp_internal.h`, `a733_dp_regs.h` define interfaces and offsets. |
| `drivers/video` | `a733_video_jpeg.c` parses JPEG and creates table data. `a733_video_jpeg_encode_tables.c` contains format tables. `a733_video_encode_platform.c` checks retained ownership and access policies; engine completion belongs to encoder state machines. |
| `drivers/g2d` | `a733_g2d.c` builds immutable RCQ command memory; `g2d_mixer_type.h` defines its layout. They perform no device MMIO. Actual task completion and reset are in the application owner, outside this directory: see [G2D completion](g2d.md). |
| `drivers/deinterlace` | `a733_deinterlace.c` runs the adapted DI301 setters against RAM, builds masked register images and consumes copied statistics. `di-port.h` marks any source delay as a violation. Actual start/busy/finish/reset MMIO belongs to the application owner: see [DI completion](deinterlace-native.md). |

## Host models must expose delayed and stale status

Native barriers also need a model that delays write visibility. Current media
host callbacks commonly update register dictionaries immediately; for example,
the DE fixture's barrier only accounts an operation. The minimum-hold checks charge callback cost and
fractional time but still publish writes immediately. Their passing results
establish neither posted-write completion nor the presence of native `DSB SY`.

Useful consumers for a model that queues writes until a barrier include the
DISPLL N→20 µs→divider/UPDATE sequence, ISP0 load-address/READY→GEN→START and
retained rearm, DE RCQ header/length→submit, codec flush/trigger→status, and G2D
acknowledgment→reset→copy. The model should keep stale visible registers while
writes are pending and start hardware latency only when the write reaches the
modeled endpoint. Architectural write completion and endpoint arrival need
separate events wherever early acknowledgment is permitted.
For minimum holds, the next operation must remain refused until the full
source interval after that visibility point. An omitted barrier must fail even
when a later status read would otherwise return success. Linked instruction
checks must observe the actual barrier; generic driver callbacks alone cannot
prove native adapter behavior.

Models that return a constant PHY status can compare register writes. They
cannot detect a missing wait, a wrong nonzero acknowledgment, or a request issued
before its prerequisite. No host check models the DisplayPort controller with
write-armed transitions: reset release should arm READY, the PLL clock request
its acknowledgment, and an A0 request should leave any prior A2 acknowledgment
visible until the A0 transition completes. The controller polls A0 equality
under its request sequence and timeout budgets.

The DSI command fixture already clears instruction enable after several reads
and tests a stuck engine. Camera calibration already delays FLAG/result and
tests a stuck result. H.264/HEVC fixtures inject decoder busy and errors before
copy. Those checks are meaningful but read-count timing cannot establish elapsed
hardware latency. A shared latency model should arm from the write and advance
from the model's monotonic time. Initial/stale status, normal delay and permanent
failure must remain independent inputs.

Ordinary configuration fixtures store writes immediately. The DISPLL fixture
drops LOCK after each UPDATE and restores it only after modeled time elapses.
The DISPLL source does not establish UPDATE auto-clear; adding an invented
self-clear event would create an unsupported hardware contract. Permanent lock
failure at either update is exercised without changing the driver. ISP/VIPP models should independently clear READY and delay their load-complete flags;
neither a copied parameter word nor a READY readback is a load completion oracle.

The DE fixture exercises read-as-zero READY and self-clear after 1 or 30 µs.
Each path reaches configuration-only ARMED without claiming a frame completion.
Corruption of an unrelated bit still fails before producer enable. Existing replay
cases require fresh RCQ finish and a later frame end, including stale flags,
failed clears and frame boundaries racing status acknowledgment.

## Validation remains host-only

The write-ordering inspection records all 45 driver files, their native provider
inputs and inspected cached source hashes in
`~/.cache/sunstoneos/build/model-fidelity-1031/ordering-followup/media/source-inspection.json`.
Every inventoried filename appears in this audit.

Focused sanitizer, pinned-source comparison and native compilation records are
under `~/.cache/sunstoneos/build/model-fidelity-1031/media/`: `pll/results.json`
for delayed and stuck DISPLL lock and `de/results.json` for unreadable READY and
replay status. No host check remains for the DisplayPort PHY and controller
transitions; the recorded results are in
[the evidence record](evidence/driver-completion-media.jsonl).

`holds-final3/results.json` records all eight fractional-counter sequencer cases.
Every compiled translation unit has a dependency file; the result hashes its
consumed headers, sources, generated fixtures and compiler commands. The check
uses the shared assertion guard, configured compiler and non-recovering address
and undefined-behavior sanitizers. Original PLL source/native checks pass under
`pll-holds/`, and both-port camera composition and retained rearm checks pass
under `capture-holds/`.
