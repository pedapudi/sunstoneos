# Accelerator register completion audit

Status: source audit and focused software validation of `drivers/gpu`, `drivers/npu`, `drivers/dma`,
`drivers/iommu` and `drivers/crypto`. Completion and timing fixes below are
implemented and software-verified. Hardware qualification
remains open. The intended reader understands MMIO, DMA and device ownership.
An ordinary control-word readback proves the sampled value; it proves a
transition complete only when the cited reference assigns that meaning.

## Primary references

- **Manual:** A733 User Manual V1.00, source-lock entry
  `a733-user-manual-1.00` in [sources.lock.json](../sources.lock.json).
  PDF SHA-256: `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
  Cached text: `~/.cache/sunstoneos/sources/camera-research/A733_User_Manual_V1.00.txt`.
- **CCU:** Linux Orange Pi revision
  `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`,
  `drivers/clk/sunxi-ng/ccu_common.c:47–64`, `ccu_gate.c:40–69,157–203`
  and `ccu_nm.c:250–345`. Cached under `sources/gpu-power/`, with hashes
  in that directory's `manifest.json`. Paths beginning `sources/` below
  are relative to `~/.cache/sunstoneos/`.
- **GPU host:** the same Linux revision's
  `bsp/drivers/gpu/imagination/pvr_fw_startstop.c:105–191,205–320`,
  `pvr_ccb.c`, `pvr_fw_mips.c`, and `pvr_power.c`; pins are in the
  source lock. The selected BXM DDK's
  `services/server/devices/rogue/rgxstartstop.c:775–860,992–1241` and
  `rgxinit.c:420–462` supply the A733 reset and stop sequence.
  The [start](designs/gpu-firmware-start.md) and
  [stop](designs/gpu-firmware-stop.md) designs give exact source selection
  and the DDK manifest at `sources/gpu-fw-start-ddk-2ac08e8c/manifest.json`.
- **NPU:** the same Linux revision's
  `bsp/drivers/npu/aw_nna_vip/vip2/vip_drv_hardware.c`, functions
  `reset_vip:104–233`, `vipdrv_hw_submit` and `vipdrv_hw_cancel:1119–1200`.
  Platform clock setup is in
  `vip2/os/linux/platform/allwinner/vip_drv_device_platform.c:265–359,1386–1387`.
  Files are cached under `sources/npu/board-vip2-2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/`.
  The [reference audit](reference-backsolve-npu.md) identifies effective
  build branches; its linked `npu/sources.json` records hashes and URLs.
- **DMA:** source-lock entry `dma-platform-sunxi-dma-c`, same Linux revision,
  `bsp/drivers/dma/sunxi-dma.c:652–653,1268–1300,2272–2282`.
- **IOMMU:** source-lock entry `a733-iommu-v2-c`, same Linux revision,
  `bsp/drivers/iommu/sunxi-iommu-v2.c:516–540` (`sunxi_tlb_flush`) and
  `sunxi_iommu_hw_init`; SHA-256
  `2cfa66d29a1223d259486c29ef6722fc6eb3679990f39592c8c4d4b11442e31d`.
- **CE:** [security sources](../apps/crypto/security-sources.json) and
  [clock sources](../apps/crypto/clock-sources.json), same Linux revision,
  `bsp/drivers/ce/v5/sunxi_ce_reg.c:393–474` and `sunxi_ce_proc.c`.
- **GIC:** Arm IHI 0069G §12.9.1, GICD_CTLR.RWP, and §12.9.7,
  GICD_ICENABLER. Cached text and hash record are indexed in
  [the reference index](reference-index.md#cached-primary-sources).

## GPU completion-sensitive writes

Offsets in this table are relative to the named register bank.

| File and function | Write and required observation | Reference and assessment |
| --- | --- | --- |
| `a733_gpu_power.c`, `on_steps`, `a733_gpu_power_poll` | PLL_GPU0, CCU `+0xe0`: output closed, LDO/PLL/lock enable, then three LOCK samples separated by more than 3 µs; more than 20 µs before output opens. Same wait after factor change. | Manual §4.1.4.1–2, §4.1.6.16; CCU lock helper. Correct spacing, reset on lock loss, 100-read bound and global deadline. Ordinary PLL readbacks exclude status; LOCK is observed separately. |
| Same | GPU0_CLK, CCU `+0xb20`, bit 27 after core gate enable and repeated update request. | Manual §4.1.6.121, R/WAC GPU0_UPD; both ON requests poll clear before the next step. A permanently set bit fails after 100 reads. |
| Same, `off_steps` | GPU0_CLK gate disable writes bit 27, waits for UPD clear, checks bit 31, then disables PLL_GPU0. | Manual §4.1.6.121: bit 27 clears after configuration becomes valid. OFF now polls clear with the existing 100-read/global deadline bounds before PLL/reset work. The vendor KEY_FIELD_MOD path supplies no demonstrated wait; this check follows the manual's completion condition. |
| Same | CORE6 PWCR `+0x20`, policy `+0`, status `+8`; TOP5 power row. | Selected `sunxi_platform.c` and Arm DEN0051E §5.2.8, as pinned in [GPU power](gpu-power.md). PWCR requires settled policy except the documented cold state. ON/OFF wait for CORE6 status; TOP5 completion comes from the power owner. Status polls retain resources on timeout. |
| Same | CCU `+0xb24` reset and bus gate; PLL disable control fields. | Manual §4.1.6.16,122 and CCU disable source. Each control write has its own readback; no separate local reset-done field is documented. CORE/TOP retirement precedes clock removal. |
| `a733_gpu_performance.c`, `a733_gpu_platform_performance_poll`, `a733_gpu_platform_performance_off` | CCU `+0xb20` source/divider update. | Manual §4.1.6.121. Both paths poll UPD clear, then verify the resulting selector/divider/rate under a deadline. Correct completion condition. |
| `a733_gpu_platform.c`, `supply_raise`; `a733_gpu_performance.c`, voltage stages | PMIC DCDC4 request, completed snapshot, supply settle. | [GPU operating-point design](designs/gpu-performance.md), [GPU power](gpu-power.md), PMIC provider pins. Both wait strictly beyond the rounded timestamp plus `A733_GPU_SUPPLY_SETTLE_US`. No direct PMIC MMIO occurs in these files. Rail sequencing is protected review scope. |
| `a733_gpu_resources.c`, `gate`; native adapter | Shared PCK/fabric gates at RCCU `+0x1ac`, CCU `+0x5c0/+0x5e0`; parent verification. | Manual §4.1.6.82–83 and [shared resource source comparison](../checks/a733-gpu-resources-reference.py). Control readbacks verify retained gate fields; selected PLL branches also check effective gate status. No new PLL transition is started here. |
| `pvr_gpu_start.c`, `pvr_gpu_start_poll` | SYS_BUS_SECURE `+0xa100`, SOFT_RESET `+0x100`: write then read fence. | GPU host start source. Read fences drain writes; their return values are intentionally not equality checks. Reset is followed by documented 3 µs delays, implemented with rounding allowance, then a fresh firmware-started word. |
| Same | SLC_CTRL_MISC `+0x3800`, MIPS remaps `+0x810…+0x8c8`, SLC coherency `+0x38c0`; GARTEN release. | GPU host start source; `pvr_mips.c` constructs the remaps. No documented per-write busy flag. Publication and ordered register writes precede release; firmware readiness is the completion condition. |
| `pvr_gpu_stop.c`, `pvr_gpu_stop_poll` | IRQ clear `+0x8b0`, thread associations `+0xb40/+0xb30`, optional unacknowledged SOFT_RESET `+0x100`. | DDK `RGXStop:992–1241`, `RGXAckIrqMIPS:420–462`. Stop polls SIDEKICK `+0x3c8`, SLC idle `+0x3898`, MMU/read status `+0x1358/+0x1350/+0x1320/+0x1338/+0x3870` before platform isolation. Bounded polls preserve ownership on failure. Unacknowledged reset has no separate read fence; subsequent platform isolation must remain ordered. |
| `pvr_gpu_control.c`, `pvr_gpu_control_poll` | MIPS IRQ clear `+0x8b0`; later status `+0x8a8`. | GPU host MIPS interrupt source. Subsequent reads and queue service observe remaining work; an asserted event keeps service pending. |
| `pvr_ccb.c`, `pvr_ccb_submit` | MTS_SCHEDULE `+0xb00` kick after queue publication. | GPU host CCB source and DDK `__MTSScheduleWrite`. The concrete kick adapter supplies register readback; command completion comes from a cleared-and-reused return slot or consumed cursor with command-specific acknowledgement. |
| `pvr_gpu_flush.c`, `pvr_gpu_clock.c`, `pvr_gpu_stop.c` | Firmware MMU-cache, clock and idle/OFF commands, through CCB. | GPU host command interfaces. MMU flush correctly waits for command return rather than its initially zero sync word. Clock/stop clear the sync word before sending and wait for a new acknowledgement. These are firmware shared-memory completions rather than new MMIO mechanisms. |
| `gicv3_spi.c`, `gicv3_spi_mask` | GICD_ICENABLER, then DSB. | GIC §12.9.1: RWP tracks disable propagation. The header explicitly defines success as an issued command. Callers retain a software mask; rearm checks RWP before enabling. The NPU mask callback separately checks RWP, enable and active state. This asynchronous API adds no claim that DSB completed propagation. ISENABLER is not tracked by RWP. |

## NPU completion-sensitive writes

| File and function | Write and required observation | Reference and assessment |
| --- | --- | --- |
| `a733_npu_startup.c`, `apply_factors`, `lock_wait`, retune | PLL_NPU CCU `+0x2a0`, gate config `+0x1920`; lock then settle before output. | Manual §4.1.4.1–2, §4.1.6.56,255; CCU source. Shared `a733_pll_lock.h` requires three spaced samples and more than 20 µs settle. Exact control checks ignore LOCK and preserve the inherited reserved bit. Correct. |
| Same, local open/stop tables | CCU `+0xb00/+0xb04` resets and gates; SYSCTRL `0x03000008` SRAM route. | Manual §4.1.6.119–120; NPU platform `npu_clk_init/uninit`; U-Boot `board_sun60iw2.c:119–133`, revision `b791be842935b27268ae3d00e943a9075495f30a`. Each write has a separate readback; no module update bit is documented. Domain ON precedes local open. Local stop retains the domain and PLL. |
| Same, `A733_NPU_STARTUP_SETTLE` | Wait after local clocks before controller access: 2,000 µs. | NPU platform source `:1386–1387`. The endpoint now includes 1 µs of rounding allowance. The raw-counter tail returns PENDING after 1,024 unchanged samples; the original absolute deadline remains in force. |
| `a733_npu_controller.c`, `reset` | NPU `+0` FSCALE pulse, isolation; `+0x3a8` reset assert/deassert; delays 5/20/20/40/50 µs. | NPU `reset_vip:104–233`. Two successful passes require idle bit 0, control `0x30000` and disabled MMU; final full idle is mandatory. Delays delegate to the native adapter. `platform/a733/npu_native.c` now requires the rounded elapsed value to exceed the request. Its existing 1,048,576-loop bound and fatal retention remain. |
| Same, `a733_npu_open`, `trigger` | MMU directory `+0x3b4`, MMU control `+0x388`, IRQ enable `+0x14`, command address `+0x654`, trigger `+0x3a4`. | NPU hardware initialization and submit functions. Exact directory/control/IRQ readbacks and clear events/full idle precede bootstrap. Trigger orders address before fetch with a barrier. Job completion requires event bit 0 and full idle before output acquire; idle alone is insufficient. |
| Same, `a733_npu_cancel`, `a733_npu_advance` | Cancel bit 3 at `+0x3a8`, 100 µs assert, release, completion bit 4 and full idle. | NPU `vipdrv_hw_cancel:1119–1200`. Completion and ownership checks are present. The minimum pulse now starts at a post-write timestamp and waits strictly beyond 100 rounded µs. The separate original timeout origin remains unchanged, including time consumed by the assertion callback. |
| Same, `a733_npu_close` | IRQ enable `+0x14=0`, MMU `+0x388=0`. | NPU hardware source plus native closed-state contract. Readback of IRQ mask and MMU enable, barrier and full idle precede CLOSED. No separate apply field is documented. |
| `npu_session.c`, `a733_npu_runtime.c`, `npu_convolution.c`, native platform adapter | Forwarded controller writes and delay callbacks. | These retain the same controller sequence and native timing dependency. Native platform allowlists admit only the startup fields above and issue DSB after each write. |

## DMA, IOMMU and crypto completion-sensitive writes

| File and function | Write and required observation | Reference and assessment |
| --- | --- | --- |
| `a733_dma_platform.c`, `a733_dma_platform_poll` | CCU `+0x704` reset/bus gate, `+0x5e4` MBUS gate; 20 µs reset-held baseline. | Manual §4.1.6.84–85; DMA platform source. Ordinary writes have exact readback. The pre-release wait starts after a baseline observation rather than asserting reset. Its rounded 20 µs interval can be short by less than 1 µs; no exact minimum was established from a primary reference, so this remains a conservative-budget issue. |
| Same, `controller` | DMA global auto gate and CPU/MCU routes. | DMA source `:2272–2282`; each write has readback and global quiet checks before and after. No update/busy handshake is documented for these route fields. |
| `a733_dma_controller.c`, `submit` | Channel descriptor `+8`, IRQ enable `+0x34`, EN `+0`. | Manual §13.1.7.6,8 and per-channel IRQ definitions; DMA submit source. Descriptor and IRQ readbacks precede EN. Completion requires finite-chain event, then teardown; an ignored start times out. |
| Same, `begin_stop`, `a733_dma_poll` | IRQ disable, pause, EN=0, unpause; pending W1C `+0x38`; descriptor restoration. | DMA terminate source `:1268–1300`; manual §13.1.7.3,6,7. Teardown polls EN=0 **and** channel BUSY=0, checks pending clear and restored descriptor before cache acquire/release. This is stronger than the reference's write-only terminate. |
| Same, cyclic receive helpers | Package count/cursor reads after cyclic submission. | Manual package/count definitions and source residue handling. The code explicitly calls this a consistency check, not a completion fence. A physical bound on data arrival versus counter/cursor progress remains unqualified. |
| `a733_iommu.c`, `operation`, `program` | Both banks' TLB flush `+0x30=3`, accessible micro-TLB `+0x18=1`, 10 µs wait, then bank flush clear. | IOMMU `sunxi_tlb_flush:516–540` requires `udelay(10)`; no completion bit is documented in this reference. The endpoint is now the rounded timestamp plus 11 µs, preserving the full 10 µs minimum. The wait begins after the final flush request; overflow and the original operation deadline still fail with ownership retained. |
| Same | Directory `+0x20`, prefetch `+0x2c`, invalidate modes `+0x40/+0x60`, IRQ masks, bypass, authority `+0x1c`, enable `+0x14`. | IOMMU `sunxi_iommu_hw_init`. Each ordinary write has its own readback. Unavailable microsets are skipped explicitly; no synthetic observation is supplied. Enable follows flush and clear. |
| `a733_ce.c`, `poll` SUBMIT/WAIT | Descriptor `+0/+4`, IRQ enable `+8`, load `+0x10`; status `+0xc`, error `+0x18`. | Manual §19.1.7.1–7; CE `ss_ctrl_start/ss_ctrl_hash_start`. Status value 1 means completed successfully, 2 means failure. Pending starts clear; output acquire follows success. TLR readback is not a substitute for task completion. |
| Same, completion cleanup | ISR W1C `+0xc`, IRQ disable `+8`. | Manual §19.1.7.4. IRQ readback follows W1C; the next PREPARE requires ISR clear. No same-call ISR-clear wait exists. Delayed W1C can cause conservative refusal on reuse, but cannot create a fresh completion because submission first checks zero. |
| Same, `prepare`, `provision` | CE LPC `+0xd0` bit 1; secure health `+0x90…+0x9c`. | Manual §19.1.7.8 and §19.1.8.34–37. Each field is read back; low-power config bit 0 must already be clear. The source does not prescribe a separate apply/ready wait for health programming. |
| `a733_ce_clock.c`, `a733_ce_clock_provision` | CCU `+0xac0/+0xac4/+0x5e4` local clocks/reset. | CE clock manifest: `sunxi_ce.c:1148–1215`; manual §4.1.6.84,117–118. Each RMW gets exact readback, with parent/effective branch checks before and after. No PLL is retuned, and no completion bit is documented for these local fields. |

The MMIO adapters `a733_gpu_resources_native.c`,
`a733_gpu_platform_native.c`, `a733_npu_platform_native.c`,
`a733_dma_platform_native.c` and `a733_ce_native.c` forward these writes.
Their barriers order transactions; controller status and timing checks supply
completion.

## Focused models expose the original completion gaps

- **GPU OFF update:** the original `checks/a733-gpu-power.c` cleared
  GPU_UPD during writes, so its gate readback could not distinguish a
  pending update. Its `--completion-only` mode now schedules delayed or
  permanently pending hardware completion through `register_latency.h`.
  Any PLL shutdown while that request remains pending fails independently.
- **IOMMU flush:** the original `checks/a733-iommu.c` advanced only whole
  microseconds. Its `>=10` assertion could not expose rounding, and the
  source-comparison fixture only checked the delay argument. The focused
  mode now uses a 24 MHz counter and rejects flush clear before 240 ticks
  have elapsed after the final micro-TLB request.
- **NPU cancel:** `checks/npu-controller.c --completion-only` separates a
  costly assertion write, fractional time, the minimum pulse and delayed
  acknowledgement. It also checks expiry during the write and while the
  minimum pulse remains incomplete. Expiry issues no deassertion or output
  acquire and retains the job.
- **NPU settle:** `checks/npu-startup-host.c --completion-only` checks the
  endpoint against the actual local-enable timestamp. A stopped raw clock
  returns PENDING; subsequent original-deadline expiry retains ownership.
- **NPU native delays:** `checks/npu-native-delay.py` compiles the production
  C delay function with a host clock, replacing only the AArch64 yield
  instruction with a compiler barrier. Six requested intervals run at all
  24 fractional phases of a 24 MHz counter. Stopped-clock and excessive-delay
  cases retain the original fatal bounds. Address and undefined-behavior
  sanitizers cover this extracted-function check.
- **DMA:** `checks/dma-controller.c` already separates queue completion
  from explicit bus drain and checks that output invalidation follows
  EN=0 and BUSY=0. That independent model remains in place.
- **CE:** `checks/a733-ce.c --completion-only` now schedules success after
  task load and keeps device output separate until completion. It rejects
  early acquire and checks a permanently pending task against the original
  deadline. Ordinary CE task waits required no production change.

The fixes preserve operating points, supply settings and hard timeouts.
The design panel approved the GPU shutdown change and NPU timing changes
before implementation; the same reviewers approved their code and focused
failure evidence. Physical timing and recovery remain unqualified.

## Executed checks and remaining qualification

Focused GPU OFF, IOMMU flush, NPU cancellation, NPU settle and CE completion
cases pass. The native-delay extracted-function check passes with address and
undefined-behavior sanitizers. Rebuilt `npu-native-model`, `npu-startup-model`
and `a733-npu-controller` include the changed code and controller layout.
Both linked FreeRTOS models pass in QEMU with GICv3. An initial native-model
invocation selected QEMU's default GICv2 and failed before NPU execution;
its corrected invocation is preserved beside that failed setup.

Removing the GPU OFF wait fails before PLL shutdown. Restoring the original
IOMMU interval fails the 240-tick flush check. Removing cancellation rounding
allowance fails the 2,400-tick pulse check. The original native-delay loop
fails the fractional-time minimum check. These negative controls establish
that the focused checks detect the corrected behavior.

For identical inherited GPU ON/OFF inputs, ON uses 190 callbacks before and
after the change. OFF increases from 48 to 50 callbacks, with 24 writes
unchanged across the complete ON/OFF pair. The added work is one register
sample and its deadline sample. The complete original and current host closures
took 1.227 and 1.286 ms in one process invocation each; those wall times do not
measure board performance. The current closure passes its complete fault sweep.

Commands, logs and mutation results are under
`~/.cache/sunstoneos/build/model-fidelity-1031/accelerators/`, especially
`results.json`, `mutations/results.json` and `native-delay-sanitized/`.
No board was accessed. GPU OFF update latency, NPU pulse and settle timing,
IOMMU flush duration, DMA cyclic ordering and physical fault recovery remain
qualification requirements.

## Files without additional completion-sensitive writes

The files below contain memory formats, queue/service orchestration, resource
ownership or adapters to the audited sequencers. Their reads and writes were
checked for independent MMIO trigger/completion protocols. Memory publication
through the CCB and NPU controller remains covered in the tables above.

- **GPU:** `LICENSE.pvr`, `a733_gpu_clock_parent.c`, `pvr_cccb.c`, `pvr_compute_context.c`, `pvr_compute_job.c`, `pvr_compute_mimus.c`, `pvr_compute_mimus_usc.c`, `pvr_compute_mode.c`, `pvr_compute_package.c`, `pvr_compute_probe.inc`, `pvr_compute_programs.c`, `pvr_compute_service.c`, `pvr_compute_usc.inc`, `pvr_firmware.c`, `pvr_free_list.c`, `pvr_fw_state.c`, `pvr_gpu_events.c`, `pvr_gpu_service.c`, `pvr_gpu_vm.c`, `pvr_mimus_service.c`, `pvr_mips.c`, `pvr_render_context.c`, `pvr_render_draw.c`, `pvr_render_job.c`, `pvr_render_programs.c`, `pvr_render_service.c`, `pvr_render_target.c`, `pvr_render_texture.c`.
- **NPU:** `a733_npu_memory.c`, `a733_npu_mmu.c`, `a733_npu_platform.c`, `a733_npu_runtime_interim.c`, `nbg_assembly.c`, `nbg_checksum.c`, `nbg_command.h`, `nbg_model.c`, `nbg_relocation.c`, `nbg_sh_nt.c`, `npu_job.c`, `npu_models.inc`, `npu_service.c`.
- **DMA:** `a733_dma.c`, `dma_memory.c`, `dma_service.c`, `dma_service_i2s.c`, `dma_service_i2s_stream.c`.
- **IOMMU:** every file has an entry in the tables.
- **CRYPTO:** `a733_ce_cipher.c`, `a733_ce_entropy.c`, `a733_ce_hash.c`, `a733_ce_session.c`.
