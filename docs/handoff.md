# Current work

Continue in `/home/sunil/git/sunstoneos-worktrees/bringup` toward the
[accepted goal](goal.md). Read `AGENTS.md` and the relevant device skill before
new work. The [hardware matrix](hardware.md) remains the complete support
inventory; component documents hold APIs, source provenance and detailed evidence.

The user cannot reconnect either board until further notice. No board access or
new hardware launch is authorized while physical recovery is unavailable. Do not
request another reconnect. Both queues were idle at the last observation; no live
query has been made since the announced second-board disconnection. Continue
offline implementation and integration. Earlier identities and admission records
supply no current authority.

Neither board has populated eMMC or UFS. Do not probe or require qualification of
these absent devices. Preserve both microSD cards and their native support.
No MIPI camera, display or microphone is attached. OV5640 is the first sensor
target; the DSI panel is undecided. Use recorded audio for mimus meanwhile.

GPU firmware cleanup, optional standalone child disposal/reopen, healthy same-AP
fixed-address/DHCP reconnect and HDMI copied USB observation are implemented within
the separate model/build scopes below. Physical operation remains unqualified.
Mixed-device disposal and Wi-Fi unexpected-loss recovery remain unsupported.
TNR native/host implementation remains active and unaccepted.
Compute-only lifecycle disposal and paged application/host evidence are now
accepted within the model scope below.

Camera and thermal-monitor adapters now check actual mutex ownership after
entry failure, independently of health. Four focused host cases pass under
`mutex-enter-20261020/root-integration.json`; all 103 frozen source files match
integration. Existing native images predate this correction; no hardware result
or new native build is claimed.

The GPU [cache-ordering source audit](gpu-compiled-programs.md) confirms the existing per-job
firmware/completion contract; it identifies no missing host cache command.
`gpu-cache-visibility-20261020/assessment.json` adds no physical visibility proof.

NPU and DI successful-completion source audits identify no extra reset-before-read
requirement. DI's retained polling finish acknowledgment is intentional for its
single-job lifetime. `npu-normal-completion-20261020/audit.json` and
`deinterlace-motion-20261020/completion-source-audit.json` add no native execution
or physical visibility qualification.

## Board state and recovery

| Board | Power and access facts | Continuation requirement |
| --- | --- | --- |
| Original, host port `3-1` | Separate 5 V/3 A supply; removing the USB data cable may leave power applied. Accepted eight-core first-task result is below. | Refresh identity, recovery and admission before any eventual bounded test. Full workload execution is deferred. |
| Second, host port `3-3` | USB was its only power source. Last confirmed removal recovered `003:073`, identity `[7,3858,1097032,0,0]`; ROM/access passed, retained processes stopped and the failed claim closed. | That recovery predates the planned disconnection. Require fresh cold DDR and platform admission when available. Evidence: `usb-first-fault-20261018/root-physical-064/recovery-073.json`. |

Per-port permissions, locks, queues and failure records remain independent.
Reconnect rules last covered `3-1` and `3-3` until host reboot; future access must
verify permissions and explicitly select the port. The separate ESP32 mimus
keyboard is unrelated: do not use it as the Orange Pi console or send desktop
keyboard events.

The preceding second-board USB takeover returned to ROM with native status 1
before controller writes. Its guard rejected the inherited device-session bit at
`0x04100040`. The integrated correction permits that bit while requiring device
role, excluding host request/mode and retaining DMA checks. Software checks and
review pass under `usb-rom-session-20261018/`; physical takeover is unqualified.
Failure evidence: `usb-first-fault-20261018/physical-064/runtime/`.

## CPU evidence and next physical decision

The primary agent owns all multicore implementation, review and board testing.
[Arm/Linux source audit](a733-multicore-reference-audit.md) and
[startup records](a733-smp-supervised.md) preserve the architectural decisions.

All eight CPUs physically reach their initial FreeRTOS task context and return
safely. Actual construction and restoration record each core's idle-task pointer,
stack, argument and exception state. The production barrier and 256 atomic
increments also pass. All writers finish before cleaning the 145-page writable
interval, or 9,280 cache lines per CPU. CPU0 restores controls; all seven
secondaries acknowledge shutdown and reach independently observed OFF. Saved
DRAM restoration and responsive ROM return pass. Reuse
`all-core-first-task-20261018/physical-010/runtime/session.json`.
This physical diagnostic excludes task bodies, timer interrupts and application
SGIs; it does not qualify sustained multicore scheduling.

The following extensions pass software checks only:

| CPU workload | Accepted scope and required preservation | Evidence |
| --- | --- | --- |
| Context switching | Actual idle yield, context save/selection/restoration and pinned-worker entry on all eight cores; wrong arguments retain cores. USB observer is suppressed. Worker bodies and timer/SGI work remain excluded. | `all-core-task-switch-20261018/build-no-observer/`, `host/software.json`, `host-root-review.json` in that evidence directory |
| Full returning workload | Each core completes 32 floating-point rounds, background work and SGIs; CPU0 services the production timer. Final-writer stop, interrupt cleanup, Nonsecure memory publication and matched shutdown are reviewed. Timing/coherence remain modeled. | `all-core-task-workload-20261019/frozen/`, `host-frozen/`, `host-root-review.json` |

The full-workload damaged-round-count refusal reuses its saved completed
checkpoint. Host checks validate workload results, epochs, task contexts,
timer-priority restoration, saved RAM and retained transport failure. Do not
repeat the earlier unobserved full launch unchanged. When recovery becomes
available, refresh board admission and select one bounded test for a concrete
remaining question; no CPU test is currently queued.

Preserve the accepted returning CPU diagnostic ELF
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`.
Rebuilding requires new symbol and patch bindings; documentation changes do not
justify replacing the frozen artifact.

## Integrated component status

All rows below describe implemented, software-checked paths. They do not grant
physical admission. Modeled hardware, scheduler, cache, transport and numerical
boundaries remain those of each referenced record. Evidence paths are relative
to `~/.cache/sunstoneos/evidence/`. The hardware matrix covers remaining onboard
functions and modes beyond these bounded applications.

| Component and canonical specification | Current scope and remaining limit | Accepted evidence |
| --- | --- | --- |
| [PCIe endpoint COPY](pcie-test-application.md) | One trusted direct `pci-epf-test` endpoint: selected BAR/iATU, 4,096-byte DMA COPY, one-shot MSI and immutable copied USB output. Hardware/scheduling are modeled. Requires endpoint-specific DMA-before-unmap and data-before-status/MSI authority; none exists yet. All resources remain retained; physical DMA/MSI, all-core startup and recovery remain unqualified. | `pcie-endpoint-20261019/root-integration.json`; `root/review.json` in that directory |
| [GPU→HDMI copied USB observations](gpu-display.md#copied-usb-observations) | Two immutable 720p converted frames and ordered 0→1→0 replay; GPU CLOSE precedes HDMI start. Typed USB authority and full-session digest bind copied hashes/16 samples per frame, three presentations and healthy GPU/HDMI/DE/PMIC terminal state. Final `70041393…`: fifteen framing cases; earlier components/eight root cases reused via 102 unchanged objects, seven SRAM cases separately. Durable host capture uses 19 s/native 20 s. Effects remain modeled; no full-pixel, visible-output, physical or concurrent GPU/HDMI qualification. All resources retained. | `gpu-display-capture-20261020/root-integration.json`; host: `gpu-display-observation-host-20261020/`, [capture scope](gpu-display-preparation.md#bundle-and-evidence-limits) |
| [Finite HDMI PCM](hdmi-audio.md), [copied USB observation](hdmi-audio-preparation.md#optional-copied-usb-observation) | Stereo S16LE/48 kHz, 1–48,000 frames via DMA0→I2S3→HDMI. Original `432b7c10…` models finite playback. Observation `0c661c29…` adds identity-bound 512-byte USB replies, with actual TinyUSB/publisher cases and host refusal/durability/readiness checks; prior audio disposition seeded, no PCM/DMA replay. Four boot assertions passed before reporting-only failure; repaired source unexecuted. Resources retained; common health failure can prevent fault retrieval. No physical USB/sound, serializer drain or SMP qualification. | `hdmi-audio-usb-20261020/root-integration.json`; earlier finite scope: `hdmi-audio-nc-20261020/root-integration.json` |
| [DisplayPort implementation](displayport-native.md) | Four-lane 1.62 Gb/s, 720p60 RGB8/C-or-E profile. Final `91646247…` passes linked constructor/PD/controller/PHY/AUX/DE lifecycle and retained shutdown, twelve host cases and one linked HS USB packet-completion case with modeled effects. Nominal schedule defers ticks under the register mutex; no contention/SMP guarantee. AUX APB source/rate authority remains required. COMB0 excludes USB3/PCIe; resources retained. No trained physical link or visible-output qualification. | `displayport-native-20261020/root-integration.json`; earlier admission/boot/launcher checks retain their image scopes; [preparation limits](displayport-preparation.md) |
| [DSI immutable-frame replay](display-scanout.md#caller-uploaded-immutable-frame-replay) | Two authenticated XRGB8888 uploads selected 0→1→0 through the register command queue (RCQ) at modeled 720p/1080p. Fresh RCQ completion plus later DE frame-end bind copied results; full USB delivery precedes next request. Frames/command pages stay immutable: no retirement, recycling or arbitrary new-frame streaming. No attached panel, physical pixels, electrical qualification or shared startup execution. | `display-frame-present-20261019/root-integration.json`; `root/layout-review.json` in that directory |
| [SPI3 polling/deferred IRQ](spi-application.md#deferred-interrupt-software-evidence) | One authenticated FIFO-sized WRITE_READ on PE0–3; version 2 uses CPU0 INTID59, ISR mask/notify and owner-only completion. Final `149f7635…`: five linked native cases and separate host/root checks. Fourteen clock-parent cases remain separate. Frozen workload-2 image excludes the differing Type-C task. Register/device/scheduler effects modeled; resources retained, physical exchange/IRQ/timing/recovery unqualified. | `spi-irq-20261020/root-integration.json`, `root-frozen/manifest.json` |
| [UART2 FIFO/IRQ/DMA](uart-application.md#finite-dma-transfers) | Finite 1–1,024-byte DMA echo: both channels active before requests; success requires stopped DMA, requests off, true TEMT/RX-empty/idle, copied echo, REAP/CLOSE and UART release. Known-live faults cancel boundedly; uncertainty retains. DMA0 parent/allocation stay owned for board lifetime. Device/cache/RTOS effects are modeled; physical wiring, DMA visibility and inherited startup remain unqualified. | `uart-dma-20261019/root-integration.json`, `root/layout-review.json`; earlier FIFO/IRQ evidence remains separate |
| [I2S duplex/repeated service](i2s-application.md), [cyclic DMA](i2s-stream.md) | Separate TX/RX channels; copied slots survive cancellation until delivery/ACK. Fixed cyclic TX and checked-copy RX/duplex are integrated. Continuity, cache visibility and wire drain remain unqualified; RX ordering blocker is below. | `i2s-duplex-20261018/integration.json`; `i2s-repeated-20261018/root-integration.json`; `i2s-hardware-cyclic-20261019/root-integration.json` |
| [Runtime I2S PCM/file input](i2s-application.md#complete-file-input-retains-unknown-serial-completion) | Finite TX leases accept BEGIN_FILE/END_INPUT with a short final period. Normal EOF requires exact DMA consumption and FIFO empty; wire completion is unknown. TX/GEN, clocks, pins, catalogue and buffers remain retained; REAP/CLOSE refuse. Explicit CANCEL may truncate; prefix-plus-guards mode remains separate. Native/provider and client/host checks model hardware; physical playback/drain remain unqualified. | `i2s-file-playback-20261019/root-integration.json`; earlier refills: `i2s-runtime-pcm-20261019/` |
| [Repeated keyboard/mimus/storage](../apps/mimus-board/README.md) | Workload 4 rearms only after transcript/sector/trigger delivery and matching ACK-response completion. Each generation needs fresh neutral→Enter and generation-bound storage overlap during unchanged EL0 recognition. Nonwrapping `UINT32_MAX` limit and original session deadline bound reuse; workload 0 remains version-1 one-shot. Lifecycle/transport/hardware/scheduler effects modeled; earlier arithmetic parity reused. Coordinator captures 1–32 generations/default 1 within 59 s/native 60 s; durable results precede ACK, failures preserve earlier output without retry/relaunch. Host fixtures/readiness only. Final `05c4b4d9…` unchanged; physical concurrency/capture/recovery/performance unqualified. | `mimus-repeated-board-20261020/root/integration.json`; host: `mimus-board-supervised-host-20261020/root-integration.json`, `checks-fixed/result.json`, `caller-entry.json` |
| [Healthy Wi-Fi reconnect](radio-session.md#explicit-healthy-reconnect-preserves-the-existing-service) | Same AP, fixed address or DHCP after socket/IP-task retirement. Fixed OPEN emits PASS; WPA2 owner/session completion is verified from terminal RAM despite reporting assertion/no PASS, corrected fixture unexecuted. Separate actual-IP-task DHCP new-lease/timeout cases pass with association/peer modeled; native board consumer build only. No board-poll/startup/physical claim; unexpected-loss recovery unsupported. | `radio-reconnect-20261020/root-integration.json`; DHCP: `radio-dhcp-reconnect-20261020/root-integration.json` |

| [IPv4 TCP listener](board-radio.md#an-admitted-listener-verifies-one-copied-application-exchange) | One admitted peer and copied 1–256-byte exchange each way; backlog one, fenced child/listener disposal. Seven actual-stack model, four app and five host cases pass; native image linked, not executed. Pinned MIT stack uses generated IPv4 reply-checksum correction. Radio-only workload 3 retains inactive older USB host inputs; physical networking, entropy and wire drain remain unqualified. | `net-inbound-20261020/root-integration.json`, `closure.json`, `bringup-impact.json`; stack: [radio-network](radio-network.md) |
| [Repeated Bluetooth recognition](bluetooth-native.md#repeated-headset-capture-and-recognition) | CVSD copied input and delivery-owned transcripts; unchanged recognizer remains EL0. Optional mSBC decodes 16 kHz speech through the same path. Transparent eSCO, receive bounds, acoustic quality and session timing remain physically unqualified. | `bluetooth-repeated-recognition-20261019/root-integration.json`; `bluetooth-msbc-20261019/root-integration.json` |
| [Bluetooth caller PCM](bluetooth-native.md#caller-pcm-playback-and-duplex) | mSBC playback-only and recognition duplex; two immutable 2,400-byte periods, full USB delivery and ticket ACK before reuse. END seals admission; CANCEL can truncate audio. Credits/disconnect account for buffers, not rendering or wire drain. Image-specific case boundary is below. | `bluetooth-pcm-duplex-20261019/root-integration.json`, `profile-gate-delta.json` |
| [Read-only SD](storage-read-application.md) | Legacy one-bit 400 kHz, four-bit default up to CSD/25 MHz, or required high speed at 50 MHz. USB-layout reconciliation was accepted at `894051d`; later headers do not invalidate that frozen image. No writes or physical mode qualification. | `sd-bus-modes-20261019/`; `sd-shared-usb-20261019/root-integration.json` |
| [Optional eMMC reads](emmc-read-application.md) | SDC2/SDC3 identification, EXT_CSD and copied reads pass instruction/host checks. Neither current board has a qualification target. The single-core image retains resources and has no qualified ROM return. | `emmc-native-20261018/` |
| [High/Baseline H.264](h264-native.md#high-profile-runtime-clips-and-presentation-ordering) | High profile 100, progressive 8-bit 4:2:0: CABAC/scaling/weights, I/P/B/reference-B; two full-HD or four HD short-term references, four active list entries, five reordered copied-output slots. Twelve full-HD + twelve refB-HD + twelve four-ref-HD pictures complete in models; normal mode decodes three/copies two. Physical decode/cache/confinement/drain remain unqualified; POC wrap/MMCO5 are unexecuted; long-term refs, unrestricted streaming and broader encoding remain required. Separate Baseline scopes retained. | `h264-high-20261019/root-integration.json`, `closure.json`, `root/layout-review.json`; Baseline: `h264-fullhd-20261019/` |
| [VGA/legacy H.264](h264-native.md) | VGA admits 256 prepared 640×480 pictures, 512 KiB/AU; legacy 48×32 IDR/P remains separate. Input refill, reference retirement and copied-output ACK are distinct. Corrected launcher and supervisor-policy evidence are bounded as below. Physical pixels, reference visibility, confinement and drain remain unqualified; no reclamation. | `h264-vga-20261019/root-integration.json`; `h264-launcher-20261019/root-integration.json`; `h264-supervisor-entry-20261019/result.json` |
| [DI301 offline WEAVE/initial motion](deinterlace-native.md), [host](deinterlace-preparation.md) | WEAVE uses one frame; mode 2 uses two caller 640×480 NV12 frames with zero history for one copied output. Motion requires reset-held/gates-off entry. Final motion `2cff56ab…` passes scoped native/refusal/serializer/host and separate SRAM checks; synthetic device writes establish no motion arithmetic or physical reset/pixels. Earlier WEAVE/USB/boot evidence retains its original scope. ACK permits neither reuse nor another job; resources retained. Steady-state history/feedback and broader modes remain required. | `deinterlace-motion-20261020/root-integration.json`; WEAVE: `deinterlace-native-20261020/root-integration.json` |
| [Recorded Bayer ISP0](camera-isp0.md), [host](isp0-preparation.md) | One 640×480 BGGR12→NV16 frame via TOP/ISP600/VIPP0/camera DMA0, with guarded masters 1/2 and immutable copied output. Final `32eb5b93…`: bounded-call nominal, classified failures, serializer/host/allocation and separate boot/SRAM checks. Pixels/device effects modeled; no numerical, full TinyUSB/DCD, scheduler or physical proof. GPL defaults/setters, no opaque tuning/runtime blob; all resources retained after ACK. Sensor-dependent ISP1 path stays separate. | `isp0-native-20261020/`; `root-integration.json` binds the final closure |
| [HEVC source-only](hevc-native.md) | Typed foundations/host sources are in bringup; runtime preparation refuses absent native allocation/page-table proof. Partial native code remains separate at `hevc-decode`/`69c4eac`, unreviewed/unbuilt. GPL Cedrus/LGPL GStreamer are planned native dependencies; opaque CedarC remains offline reference with metadata-ABI limits. Foundation evidence is limited to UBSan session validation; no host-runtime/native image or physical decode result. | `hevc-host-source-20261020/root-integration.json`; `hevc-decode-20261020/root-session/result.json`; [source pins](../apps/hevc/sources.json) |
| [H.264 encoding](h264-encode-native.md) | Two caller 640×480 NV12 frames, Baseline level 3.1/CAVLC/QP 26 IDR→P; immutable copied outputs with delivery-gated ACK. All input/reference/output resources retained. Actual provider/IOMMU/copy/USB and 140 SDK-reference register-write checks pass with entropy/completion modeled; no opaque CPU library linked. Final image `2e4a93b0…`; real entropy, reconstruction, numerical encoding, visibility and drain unqualified. Broader encoding remains required. | `h264-encode-20261020/root/integration.json`; [source provenance](../apps/h264-encode/sources.json) |
| [JPEG encoding](jpeg-encode-native.md) | One retained caller 640×480 NV12 job, stride 640/quality 75; copied JPEG ≤1 MiB. Inherited PCK3, exclusive encoder lease and IOMMU master 3. Provider/IOMMU/copy checks model completion and entropy; native image linked, not executed. No numerical, burst, visibility, drain or physical claim; all resources retained. Frozen workload 2 uses private USB0, with eight inactive radio/USB-host input deltas. Broader encoding remains required. | `jpeg-encode-native-20261020/root/integration.json`, `closure-summary.json`, `commit-binding.json` |
| [JPEG](jpeg-application.md) | Authenticated supplied baseline YCbCr420 image and copied pixels with dimensions; independent oracle optional. Pixel correctness and confinement remain physically unqualified. | `jpeg-runtime-input-20261018/` |
| [Crypto](crypto-application.md#fixed-cold-clock-provisioning-for-caller-requests) | Caller SHA-256/AES-128 ECB; version 3 adds fixed cold CE400 clock/reset provisioning after authenticated reset-held/gated admission. Eighteen owner cases and two separate SRAM cases pass; SHA device output/RTOS effects modeled. One build, prior arithmetic/version-1/2 evidence reused. All resources retained; physical exclusion, secure access, operation and drain remain unqualified. | `ce-clock-20261020/root-integration.json`; caller inputs: `crypto-runtime-input-20261018/` |
| [USB HID/CPU IRQ](usb2-host.md) | LS/FS/HS HID copies four reports without desktop events. Supervised board option routes CPU0 INTID187 to the existing owner; other profiles poll. IRQ delivery, DMA/drain, power and attachment reuse remain unqualified. | `usb-host-interrupt-20261018/root-integration.json`; `usb-host-cpu-irq-20261019/root-integration.json` |
| [Gen1/companion leaf](usb2-host.md#one-gen1-hub-or-the-usb2-companion-root-serves-one-leaf) | One selected root, direct or one self-powered hub/leaf: workloads 6/8 Gen1 MSC/boot keyboard, 10/12 USB2 companion keyboard/HS MSC. Typed topology binds the unchanged 592-byte session. Actual stack/copied-client/host plus six identity/layout and two route cases model hardware/IRQ. Native linked, not physically executed. Root/topology/class, ≥900 mA and hub-power admission remain required; no physical bundle. Nested/simultaneous roots and wider USB remain required. Mimus remains USB2. | `usb-ss-hub-20261020/root/integration.json`, `handoff.md`; earlier direct HID: `usb-ss-hid-20261020/` |
| [USB hub/dual leaf](usb2-host.md) | Workload 4: one self-powered HS USB2 hub, one LS/FS/HS HID plus one HS MSC. Joint immutable output requires both results and fresh attachment. Existing IRQ187 route retained; derived profiles keep workloads 0–3. Hub/TT, power, DMA and drain remain unqualified. | `usb-host-dual-leaf-20261019/root-integration.json`; earlier single leaf: `usb-host-hub-20261019/` |
| [USB MSC/Gen1 BOT](usb2-host.md) | GET_MAX_LUN endpoint reset/dequeue precedes LUN0 fallback and copied READ10; eleven HS cases model hardware, SS recovery is source-reviewed only. Other stalls/uncertainty retain. Workload 6 Gen1 read-only LBA0 refuses USB2 fallback and requires authenticated ≥900 mA plus image-bound COMB0/rail/clock/route/source-switch admission; none exists. Wider modes and safe detach/drain remain required. Frozen recovery image retains the older clock policy. | `usb-msc-stall-20261020/root-integration.json`; Gen1: `usb-host-superspeed-20261019/` |
| [LRADC0 raw observations](adc-application.md#lradc0-raw-observations-require-separate-electrical-authority) | ADC version 2 copies two fresh channel-0 raw codes, then configured disable with permanent retention. Final `7f390582…` passes composed/driver/host checks with analog/RTOS/USB effects modeled; raw 0/63 establish no measured value or sample rate. Physical use is blocked by nominal 1.8 V R23 pull-up versus documented 0–1.35 V range, without overrange authority. No key mapping or voltage conversion. | `lradc-20261020/root-integration.json`, `root-frozen/manifest.json`; original and corrected fixtures retain their scopes |
| [Thermal](thermal-application.md), [ADC](adc-application.md), [finite USB](usb.md) | Finite applications and return adapters integrated. Thermal/ADC retain configuration and leases; ADC needs fitted-divider electrical admission. Physical operations/return remain unqualified. | `thermal-adc-returning-source-20261018/integration.json`; `usb-returning-20261018/integration.json` |
| [RTC source/alarm](rtc-application.md) | Separate source initialization and external-source alarm paths. Source admission precedes alarm use; success requires verified Secure interrupt-state restoration, not only a completion message. | `rtc-source-returning-20261018/integration.json`; `rtc-alarm-returning-20261018/integration.json` |
| [NPU runtime/IRQ](npu-application.md), [service](npu-service.md) | Retained same-model SUBMIT/READ/copied output/REAP/CLOSE; IRQ97 owner alone acknowledges. Bootstrap plus two finite inferences require three notified completions and stopped receipt. Source-off CLOSE/GIC stop precede release; retained runtime keeps OS/USB live without ROM return. | `npu-interrupt-service-20261019/root-integration.json`; `npu-runtime-input-20261019/dma-reconcile/root-integration.json` |
| [NPU two-stage graph](npu-two-stage.md) | Fixed two fused convolution/ReLU stages, 64 input/eight output bytes, internal SRAM and one graph completion. CPU/SDK references pass; native output/service are modeled. No mimus replacement or physical inference claim. | `npu-two-stage-20261019/root-integration.json` |
| [NPU SH/NT graph](npu-relative-shift.md) | Fixed FP32 four-head/128-step relative shift, external pool cleared before each job, chunked copied input/output and healthy CLOSE. Two reused-network SDK outputs match FP32 bits; native replies/IRQ/cache/transport are modeled. No unchanged-encoder acceleration. | `npu-relative-shift-20261019/root-integration.json` |
| [Complete NPU NN graph](npu-complete-nn.md) | Published 125-operation ShuffleNetV2 graph with coefficients/pool; two copied jobs and CLOSE. Both SDK input patterns saturate to `[0,255]`, with undocumented classes: no input-sensitive accuracy, physical inference or mimus acceleration claim. | `npu-complete-nn-20261019/root-integration.json` |
| [Combined GPU/NPU](accelerator-composition.md) | Trusted GPU compute and complete NPU graph share providers and one copied USB owner. Interleaved results/independent CLOSE retain the session deadline; shared faults revoke both. Native and host preparation/capture checks model admission, CPU/RTOS, arithmetic and hardware; NPU outputs remain saturated. Positive bundle substitutes physical NPU validation; production refuses synthetic proofs. Supervisor stops at readiness before queue/USB. Native linked; physical execution unqualified. | `gpu-npu-composition-20261020/root/integration.json`; host: `accelerator-host-20261020/root-integration.json`, `results.json`, `supervisor-entry.json` |
| [G2D fill/copy/blend/2× enlargement](g2d.md), [host](g2d-preparation.md) | Exact 2× ARGB8888 enlargement uses a source crop at half the even destination dimensions. Current `f77be8b2…`: three pinned geometries, five native refusals, one job/copy, checkpointed HS TinyUSB/ACK and separate SRAM/host checks. Before ACK, authenticated output must keep outside-rectangle/padding bytes zero; interpolation remains unchecked. One build/launcher, no prior matrix replay. Earlier success/blend scopes remain separate. All resources retained; physical pixels/DMA/cache/reuse and arbitrary ratios/downscaling/rotation/conversion remain unqualified or required. | `g2d-scale-20261020/root-integration.json`; prior: `g2d-success-20261020/`, `g2d-blend-20261020/` |
| [GPU programs/live input](gpu-compiled-programs.md) | Trusted sealed program, same-shape copied input, exact-sequence READ and CLOSE. Separate multi-dispatch and true 1D/2D grid profiles admit up to 31 workgroups/992 elements with 32 lanes. Numerical execution and device-cache visibility remain modeled/unqualified. | `gpu-dispatch-geometry-20261019/`; `gpu-workgroup-grid-20261019/root-integration.json`; `gpu-runtime-input-20261019/dma-header-integration/root-integration.json` |
| [GPU indexed textures/depth/blend](gpu-render.md#depth-testing-and-source-over-blending) | Through 1080p with trusted shaders; per-frame depth OFF/LESS/LEQUAL/ALWAYS plus writes and straight-alpha source-over. Shared GEOM+FRAG_PR then final FRAG retains both rings through final completion. Old single-final-fragment ownership results are historical; legacy-profile-2 sequencing is separately checked. GPU/firmware and selected app/USB boundaries are modeled. Physical graphics, stencil/perspective, arbitrary shaders/blends, wider modes, presentation and disposal remain required. | `gpu-depth-blend-20261019/root-integration.json`, `closure.json`, `root/layout-review.json`, `reuse.json` |
| [GPU compute cleanup](gpu-compute-service.md), [render cleanup](gpu-render.md#healthy-close-cleans-firmware-objects-before-stopping) | Exact successful context/render-target/free-list receipts precede healthy stop; partial progress stays retained and late reconstruction faults through shutdown. Seven compute/eight render composition cases model firmware/MMIO/isolation. Separate full native builds `dae5a387…`/`6c45bdc3…` compile/link their recorded layouts only; no runtime-result transfer. Default backing/VM stay retained and reopen is refused; optional standalone disposal has separate scope below. Physical cleanup remains unqualified. | `gpu-compute-cleanup-20261020/root-integration.json`, `gpu-render-cleanup-20261020/root-integration.json`; builds: `gpu-compute-cleanup-native-20261020/`, `gpu-render-cleanup-native-20261020/` |
| [Standalone GPU disposal/reopen](gpu-render.md#standalone-sessions-can-dispose-and-reconstruct-child-resources) | Optional sole-client single-core profile releases/rebuilds seven child parents with fresh generations/epochs; two global firmware parents retained. Original `e26b9632…` native leaf checks model stopped admission; final `780d9544…` depth app/TinyUSB protocol models service completion, with thirteen host cases. Two builds, 87/89 objects unchanged after app budget fix. Separate 15 s DISPOSE/rebuild budgets; triangle/version-8 texture runtime and pending texture OPEN remain unchecked; compute-only has separate evidence below. Physical reuse/visibility/SMP and mixed-device reclamation remain unqualified. | `gpu-resource-disposal-20261020/root-integration.json`, `closeout.json`, `final-frozen/manifest.json` |
| [Compute-only disposal/reopen](gpu-compute-service.md#compute-only-disposal-and-reopening) | Image `06315568…` executes actual three-parent release/reconstruction with package/global parents retained, plus seventeen app/TinyUSB exchanges with GPU/service completion modeled. Pending OPEN holds old identity; READY publishes coherent new epoch/result for immediate READ. Two 65-word outputs use pages of 32, 32 and 1 word; healthy host replay and four tail faults retain exact 256-byte prefixes. Physical reuse/cache/SMP and mixed-device disposal remain unqualified. | `gpu-compute-lifecycle-20261020/root-integration.json`, `final-frozen/manifest.json` |
| [GPU returning application](gpu-application.md), [NPU returning application](npu-application.md) | Workers stop and PMIC parks before cleanup. Reviewed Nonsecure publication and matched host restoration are preserved separately from retained live-input profiles. Physical inference and ROM return remain unqualified; critical frozen scopes are below. | `gpu-returning-repeat-fix-20261019/root-review.json`; `gpu-returning-host-20261019/root-host-source-review.json`; `npu-finite-return-20261018/frozen/`; `npu-finite-host-20261018/root-host-source-review.json` |

## Blockers and image-specific evidence boundaries

- **I2S cyclic RX/duplex:** physical admission requires
  `cyclic-rx-completion-and-reuse-ordering-established`; no qualifying record
  exists. The completed-package counter does not explicitly order destination
  writes and cursor updates. CPU-cache review alone is insufficient. Preserve
  `i2s-hardware-cyclic-20261019/root-cache-review.json` and
  `identity-reuse-clarification.json`; standalone-client cases model service calls.
- **Bluetooth:** seven composed/eight native operation cases retain ELF
  `36bfa257…`; final `388f9f93…` changes five objects for exact profile guards.
  Its final-image case covers version admission and copied PCM commit only.
  Earlier repeated-recognition USB cases also retain their earlier image;
  `identity-reuse.json` binds the changed finished/status case. Pinned libsbc 2.0
  is CPU code under LGPL-2.1-or-later; preserve source/notices/relinkable objects
  via [the manifest](../apps/bluetooth/msbc-source.json). Default CVSD omits it.
- **H.264:** the historical `862fae444a481eb9505d8ba7ef3835760e208764663c1e7aef8459f8b9be43ac`
  omits VE counter-read permission and cannot publish completed output. Its
  substituted-read compositions are historical only. VGA `24144b2a…` includes the
  correction and real lower IOMMU policy checks; normal-mode evidence stops after
  frame one with terminal status pending. Use the corrected AArch32 launcher in
  new VGA bundles; old frozen bundles reject version 2. Launcher tests stop at
  modeled reset handoff; the 59-second supervisor-policy sentinel stops before
  queues, evidence creation or USB. Full-HD `b48123e3…` has separate private
  contract/sealing evidence; neither path executes shared startup or proves pixels.
- **GPU:** the corrected fixed-return candidate is
  `2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d`.
  Publish through Nonsecure EL1 mappings before SMC; earlier EL3-only publication
  results are superseded. Root review passes; physical admission remains false.
  The historical two-job
  image has the NEW-only callback defect and is superseded for repeated submit.
  Keep `gpu-returning-application-20261019/ns-publication/` and
  `gpu-returning-host-20261019/` within their publication/host scopes. Native
  compute/render cache ordering implements the pinned firmware's documented
  per-job flushing contract and matched completion UPDATEs. No missing host-side
  cache command is identified. Physical device-cache visibility remains
  unqualified; CPU maintenance alone does not establish it. Source comparison:
  `gpu-cache-visibility-20261020/assessment.json`.
  Corrected shared rendering uses FRAG_PR (204) at 2n−1, then final FRAG (203)
  at 2n; geometry completion n and final fragment 2n gate output/reuse. Older
  single-final-fragment ownership cases are superseded. Compute evidence retains
  its separate scope.
- **NPU returns:** preserve `npu-finite-quiescence-20261018/integration.json`,
  `npu-supply-returning-20261018/integration.json` and
  `npu-finite-host-20261018/transport-frozen/`. SRE tasks run at EL1t; cached SMP
  task/exit rules do not transfer. Stop communication after transport failure;
  retain ownership. Root retains architectural return review.
- **Supervisor entry:** host-only corrections admit DSI and UART result keys.
  The actual caller reaches a readiness sentinel before queue or device access:
  `dsi-supervisor-entry-20261019/result.json`, `frozen/manifest.json`. This
  establishes neither native startup nor hardware operation; H.264 was not rerun.
  UART entry passes the same boundary under `uart-supervisor-entry-20261019/`.
  Its `caller-audit.json` confirms admitted keys for all 21 existing production
  callers, including both conditional GPU profiles; device cases were not repeated.
- **Physical device preparation:** DSI needs a selected panel and image-bound
  electrical/PLL, rail/bank-voltage, wiring, isolation and fetch-padding facts;
  model-only authority is refused. USB Gen1 has no authenticated physical source
  record. Type-C cutoff cannot remove an external hub's supply. Combined mimus
  needs fresh DDR/model-memory, power, DMA and recovery admission; its injected
  native transcript does not establish physical concurrent recognition/storage.

## Continuation and evidence reuse

The [microSD container packager](microsd-boot-package.md) creates regular files
only; one actual packer invocation and seventeen host checks pass. Root audit:
`microsd-boot-package-20261020/wrapper-root-integration.json`. The
[cold-entry recorder](cold-entry.md) now supplies a guarded 4,164-byte native
monitor that captures header/exception state and parks. Actual boot0 parser,
cleanup/main-tail and eight AArch64 entry cases pass with independent reset-state
models; one ordinary-file container contains the actual monitor. Evidence:
`cold-entry-20261020/frozen/manifest.json`, `instructions/result.json`.
The actual parser cache wrappers separately execute twenty synthetic set/way
operations before disabling the data cache under `cold-boot-cache-20261020/audit.json`.
Earlier parser checks substituted these wrappers; no old cases were replayed.
This establishes neither full boot0 nor physical publication or remote/DMA exclusion.
No FreeRTOS entry, remote-writer exclusion, memory ownership, physical boot,
media write or recovery is established. Standalone OS boot remains incomplete;
packer license/source equivalence remains unresolved. The primary agent owns
cold-entry/component authority and CPU integration. Hardware access remains unavailable.

The shared launcher correction `6407e26` preserves exact legacy preprocessed
templates and the combined binary; it adds no physical launch evidence.
GPU→HDMI copied USB observation is accepted within its modeled boundaries;
physical startup/output remain unqualified. G2D fill/copy native/host software
integration and DP composed native/host observation are accepted within their
recorded model scopes. DI301 WEAVE/initial motion are accepted within the modeled scope
above. Finite HDMI audio and copied USB observation are accepted within the
modeled scopes above; physical USB capture and playback remain unqualified.
CE local clock/reset provisioning is accepted within the model scope above.
G2D DE_SYS power initialization and recorded-image ISP0 integration are accepted
within the model scopes
above. The sensor-dependent camera path still uses ISP1 pass-through; neither
path has physical or numerical image-processing qualification.
DMA fabric admission now accepts correctly normalized 24 MHz references from
supported raw oscillators and retains REF in every admitted profile. Ten focused
platform/clock-registry sanitizer cases pass under
`dma-clock-reference-20261020/frozen/manifest.json`; no native image or physical
DMA qualification is added. The overall goal remains incomplete.

LRADC0 software integration is accepted within the modeled scope above;
physical electrical authority remains unresolved. [DMA1 applicability](dma.md#dma1-request-routing-remains-unresolved)
is source-blocked after the bounded primary-source search: the CPUX request table
does not establish CPUS DRAM encoding. No DMA1 implementation, build, descriptor
or launch is enabled.
DMA-copy, UART DMA, I2S and HDMI audio host admission require external session/
configuration-bound channel, controller and descriptor/data Nonsecure access
authority. Thirty additional consumer host cases and twenty source matches are
accepted under `dma-consumer-security-20261020/root-integration.json`, without
native/runtime changes. Production rejects model authority; no safe DMA security
register probe or physical permission is established. Earlier images retain
their runtime scopes; HDMI audio USB observation has its separate scope above.

The [provider clock record](provider-clock-reference.md) covers normalized REF24
admission in TWI11/USB2/radio/CE/I2S/PWM. [SPI](spi-platform.md) has separate
parent checks. Raw S_TWI1, LED source 0 and S_SPI source 0 retain their recorded
restrictions. [HDMI](provider-clock-reference.md#hdmi-native-evidence) now binds
raw 24/26 MHz TOP input separately from normalized 24 MHz SFR and retains REF.
Actual compiled-object raw-26 720p prepare/start/retained-stop and saved-state
refusals pass under `hdmi-ref26-20261020/root-integration.json`. Calculated pixel
rates are 73.9375/147.875 MHz; 1080p has source/host evidence only. Physical timing
and output remain unqualified; the earlier GPU image retains its frozen scope.
Other function checks do not qualify images or physical clocks.

1. Continue complete native device paths and missing modes in the hardware matrix;
   integrate reviewed work promptly. Keep component owners' shared drivers and
   startup untouched unless a concrete dependency requires coordination. Update
   canonical hardware/component records and existing issues for accepted changes.
2. Preserve each accepted ELF, full source/object closure, generated inputs and
   evidence scope. Shared USB headers change embedded layouts even in consumers
   whose USB host path is inactive. Audits match their named integration revision,
   not every future HEAD. Future builds compile affected consumers consistently;
   do not rebuild old images merely to catch up. SD `141078f4…` matched `894051d`.
3. Reuse accepted checks while relevant inputs match; a commit, documentation edit
   or handoff is not a rerun reason. Follow [selective validation](validation.md)
   and the [reference index](reference-index.md). Tables above select continuation
   records; their linked component documents preserve full evidence inventories.
4. When physical recovery is available, the primary agent refreshes identity,
   clock/power/memory/DMA facts and board-specific admission before one justified
   CPU or independent device test. Preserve the final-writer barrier, Nonsecure
   publication and matched shutdown. Fixtures supply no physical authority.
5. Require established recovery before persistent writes. Keep intentional
   watchdog expiry and the failed historical UFS probe deferred; absent eMMC/UFS
   are not current qualification blockers. Preserve both cards.
6. Qualify unchanged-model mimus and representative concurrent OS workloads.
   After every required hardware function works, optimize complete recognition
   to at most 0.05 times audio duration; investigate 0.01 times as a bounded stretch.
   Report loading and live latency separately. Reproducible boot images,
   recovery/update procedures and physical qualification remain goal requirements.

The [historical handoff](handoff-history-20261019.md) preserves older integration,
recovery and validation context; it supplies no current launch authority.
