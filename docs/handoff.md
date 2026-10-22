# Current work

Continue in `/home/sunil/git/sunstoneos-worktrees/bringup` toward the
[accepted goal](goal.md). Read `AGENTS.md` and the relevant device skill before
new work. The [hardware matrix](hardware.md) remains the complete support
inventory; component documents hold APIs, source provenance and detailed evidence.

`bringup` contains the [implementation audit](audit/README.md) and six rounds
of fixes and features based on it. The audit record lists each package, the
findings it addressed and the findings that remain deferred with their
reasons. Every change was checked with host builds, emulators and host models,
and all 67 central build targets build. No board test ran during the audit or
any round, so nothing they added is physically tested or qualified. The merged
work is committed locally and has not been pushed.

## Board state and recovery

One board is attached, on host USB port `3-1`, in FEL mode: the host's USB
device list shows it as the boot ROM's FEL device, an observation that involved
no communication with the board. The user cannot reconnect it if its USB
connection drops, so one lost connection ends board access until further
notice. No board test may run, and no board command may be sent, until the user
says physical recovery is available. Do not request a reconnect. Nothing in
rounds four to six ran on the board. The second board is disconnected. Earlier
identities and admission records supply no current authority. When recovery is
available, the physical steps follow the
[ranked RAM-only board test plan](audit/board-test-plan.md);
[continuation](#continuation-and-evidence-reuse) gives the complete order.
Until then, continue the [offline work](#next-offline-work).

Neither board has populated eMMC or UFS. Do not probe or require qualification of
these absent devices. Preserve both microSD cards and their native support.
No MIPI camera, display or microphone is attached. OV5640 is the first sensor
target; the DSI panel is undecided. Use recorded audio for mimus meanwhile.

| Board | Power and access facts | Continuation requirement |
| --- | --- | --- |
| Original, host port `3-1` | Attached; the host lists the boot ROM's FEL device (`1f3a:efe8`). Separate 5 V/3 A supply; removing the USB data cable may leave power applied. The user cannot reconnect it if the connection drops. Accepted eight-core first-task result is below. | No access until the user says physical recovery is available. Then bind its queue to the port and refresh identity, recovery and admission in the order of the board test plan. Full workload execution is deferred until those ranks pass. |
| Second, host port `3-3` | Disconnected. USB was its only power source. Last confirmed removal recovered `003:073`, identity `[7,3858,1097032,0,0]`; ROM/access passed, retained processes stopped and the failed claim closed. | That recovery predates its disconnection. Require reconnection by the user, then fresh cold DDR and platform admission. Evidence: `usb-first-fault-20261018/root-physical-064/recovery-073.json`. |

Per-port permissions, locks, queues and failure records remain independent.
Neither queue has a running test; the original board's queue has four blocked
entries. Neither queue database records
its board port, and the runners that check the binding refuse claims from an
unbound queue before any device request. The coordinator binds each queue with
`tools/board-test-queue.py ... bind-port` before its next launch, as described
in the [queue procedure](board-test-queue.md). Reconnect rules last covered
`3-1` and `3-3` until host reboot; future access must verify permissions and
explicitly select the port. The separate ESP32 mimus keyboard is unrelated: do
not use it as the Orange Pi console or send desktop keyboard events.

The second board's last USB takeover attempt returned to ROM with native status 1
before controller writes. Its guard rejected the inherited device-session bit at
`0x04100040`. The integrated correction permits that bit while requiring device
role, excluding host request/mode and retaining DMA checks. Software checks and
review pass under `usb-rom-session-20261018/`; physical takeover is unqualified.
Failure evidence: `usb-first-fault-20261018/physical-064/runtime/`.

## Integrated OS image

The [integrated OS image](os-image.md), `sunstone-a733-os`, runs the mimus
board application together with a continuous thermal service. The board
application already combines the USB0 device service, the USB2 host with
keyboard and read-only storage, the Type-C and PMIC owners and EL0 recognition.
The image routes its interrupts through one fixed table and starts its
applications from one fixed list. It builds in two lifetimes:

- **Supervised.** The image accepts the bounded FEL contract, keeps WDT0
  disabled and ends with a terminal halt that needs manual power removal. A
  FEL launcher and session preparation exist. Preparation requires a reviewed
  entry record built from physical evidence, which does not exist yet.
- **Resident.** The [stage-2 loader](boot-slots.md) enters the image after a
  cold boot from microSD with WDT0 armed in its four-second mode. A supervising
  task renews the watchdog only while every listed service reports progress
  within its window, so a stalled service lets the watchdog reset the board.
  The loader admits the SD controller state that boot0 leaves, but its three
  entry prerequisites refuse: remote-writer exclusion, watchdog recovery after
  a cold boot and a comparison of boot0's entry state. This lifetime cannot
  run on a board until all three exist.

Optional builds add I2S microphone capture with the DMA0 interrupt in the
table, the board's Wi-Fi station with crypto engine entropy for WPA2, or
Bluetooth headset recognition. The image builds in six checked variants:

| Variant | Options added to `SUNSTONE_A733_OS=ON` | Needed before a board run |
| --- | --- | --- |
| Supervised | None | The reviewed entry record, and a passed USB0 channel test |
| Resident | `SUNSTONE_A733_OS_RESIDENT` | The stage-2 entry prerequisites, a qualified watchdog-expiry recovery and measured service windows |
| Resident with I2S audio | `SUNSTONE_A733_OS_RESIDENT`, `SUNSTONE_A733_OS_AUDIO` | As resident, plus a connected microphone and a reviewed audio entry record with the resident DMA facts |
| Radio | `SUNSTONE_A733_OS_RADIO` | Preparation that stages the radio profile and firmware, a reachable access point, and a qualified faster SDIO mode, because firmware loading needs at least 46.7 s at the admitted clock and the bootstrap bound is 40 s |
| Resident with radio | `SUNSTONE_A733_OS_RESIDENT`, `SUNSTONE_A733_OS_RADIO` | As resident, plus the radio profile and firmware as a slot asset at `0x43000000`; any radio failure, including an absent access point, resets the board |
| Supervised Bluetooth headset | `SUNSTONE_A733_OS_HEADSET`, with `SUNSTONE_BT_SECURITY_DIR` naming the pinned Zephyr security sources | Preparation that stages the Bluetooth profile and firmware, and a paired, reviewed headset |

The headset and radio builds exclude each other, because both own the AIC8800
module. The headset build also excludes the microphone, and it is supervised
only, because the Bluetooth application ends 60 s after it starts. A link check
and host models of interrupt delivery, supervision and admission pass for
every variant. No board has run any of them. Display, camera, GPU and NPU
remain separate images.

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
| [Eight-core emulator soak](smp.md#eight-emulated-cores-pass-a-scheduling-soak) | Ten QEMU trials pass a cross-core queue ring, shared-mutex exclusion, migration through all cores with FP context and preemption of a non-yielding task on every core. QEMU models identical Cortex-A55 cores and uses the QEMU platform, not the A733 cached platform. | `smp8-soak.json` |
| [Resident contract](a733-smp-cached.md#resident-contract) and [interrupt table](a733-smp-cached.md#a-fixed-interrupt-table-serves-images-with-several-device-owners) | Cold-boot contract with an armed four-second watchdog, no FEL return and no diagnostic deadline; EL3 route programming and per-core dispatch for listed shared peripheral interrupts. Host checks cover admission and every refusal. The integrated OS image uses both. | Not frozen; rerun `checks/a733-resident-contract.c` and `checks/a733-irq-table.c` |

The full-workload damaged-round-count refusal reuses its saved completed
checkpoint. Host checks validate workload results, epochs, task contexts,
timer-priority restoration, saved RAM and retained transport failure. Do not
repeat the earlier unobserved full launch unchanged. When recovery becomes
available, ranks 7 and 8 of the [board test plan](audit/board-test-plan.md)
are the CPU tests: the accepted first-task diagnostic, then the full returning
workload. Each needs refreshed board admission first; no CPU test is queued.

Preserve the accepted returning CPU diagnostic ELF
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`.
Rebuilding requires new symbol and patch bindings; documentation changes do not
justify replacing the frozen artifact.

The [CPU clock and voltage driver](cpu-clock.md) and its thermal coupling
exist and pass host and instruction models. The driver moves CPU-L and CPU-B
among the pinned operating points, raises a cluster only at the thermal
policy's NORMAL level after a reading from that cluster's sensor, and lowers
raised clusters to 1014 MHz at THROTTLE. Every transition needs a session
grant record, and no image grants a transition: no image carries a grant or
calls the driver. The user reviewed the CPU clock plan and agreed with the
design on 2026-10-21. Any physical change still needs the user's approval for
each change, and the board cannot be reconnected yet. CPU-L therefore stays at
the 494 MHz that boot0 leaves.

## Integrated component status

All rows below describe implemented, software-checked paths. They do not grant
physical admission. Modeled hardware, scheduler, cache, transport and numerical
boundaries remain those of each referenced record. Evidence paths are relative
to `~/.cache/sunstoneos/evidence/`. The hardware matrix covers remaining onboard
functions and modes beyond these bounded applications.

| Component and canonical specification | Current scope and remaining limit | Accepted evidence |
| --- | --- | --- |
| [Integrated OS image](os-image.md), [stage-2 loader](boot-slots.md) | Mimus board application plus a continuous thermal service, one interrupt table and a fixed start list, in the six variants [above](#integrated-os-image). Resident renewal requires progress from every listed service within 1 s windows (3 s for recognition). Link check and host models pass for every variant, including each service stalling or never reporting. The loader admits the SD controller state that boot0 leaves; its entry prerequisites refuse on a board. Service windows are unmeasured. | Not frozen; rerun `checks/os-image-link.py`, `checks/os-image-model.c`, `checks/os-image-stage2.py`, `checks/os-image-audio.py`, `checks/os-image-radio.py`, `checks/os-image-headset.py`, `checks/os-image-identity.py` and `checks/os-image-launch.py` |
| [PCIe endpoint COPY](pcie-test-application.md) | One trusted direct `pci-epf-test` endpoint: selected BAR/iATU, 4,096-byte DMA COPY, one-shot MSI and immutable copied USB output. Hardware/scheduling are modeled. Requires endpoint-specific DMA-before-unmap and data-before-status/MSI authority; none exists yet. All resources remain retained; physical DMA/MSI, all-core startup and recovery remain unqualified. | `pcie-endpoint-20261019/root-integration.json`; `root/review.json` in that directory |
| [GPU→HDMI copied USB observations](gpu-display.md#copied-usb-observations) | Two immutable 720p converted frames and ordered 0→1→0 replay; GPU CLOSE precedes HDMI start. Typed USB authority and full-session digest bind copied hashes/16 samples per frame, three presentations and healthy GPU/HDMI/DE/PMIC terminal state. Final `70041393…`: fifteen framing cases; earlier components/eight root cases reused via 102 unchanged objects, seven SRAM cases separately. Durable host capture uses 19 s/native 20 s. Effects remain modeled; no full-pixel, visible-output, physical or concurrent GPU/HDMI qualification. All resources retained. | `gpu-display-capture-20261020/root-integration.json`; host: `gpu-display-observation-host-20261020/`, [capture scope](gpu-display-preparation.md#bundle-and-evidence-limits) |
| [Finite HDMI PCM](hdmi-audio.md), [copied USB observation](hdmi-audio-preparation.md#optional-copied-usb-observation) | Stereo S16LE/48 kHz, 1–48,000 frames via DMA0→I2S3→HDMI. Original `432b7c10…` models finite playback. Observation `0c661c29…` adds identity-bound 512-byte USB replies, with actual TinyUSB/publisher cases and host refusal/durability/readiness checks; prior audio disposition seeded, no PCM/DMA replay. Four boot assertions passed before reporting-only failure; repaired source unexecuted. Resources retained; common health failure can prevent fault retrieval. No physical USB/sound, serializer drain or SMP qualification. | `hdmi-audio-usb-20261020/root-integration.json`; earlier finite scope: `hdmi-audio-nc-20261020/root-integration.json` |
| [DisplayPort implementation](displayport-native.md) | Four-lane 1.62 Gb/s, 720p60 RGB8/C-or-E profile. Final `91646247…` passes linked constructor/PD/controller/PHY/AUX/DE lifecycle and retained shutdown, twelve host cases and one linked HS USB packet-completion case with modeled effects. Nominal schedule defers ticks under the register mutex; no contention/SMP guarantee. AUX APB source/rate authority remains required. COMB0 excludes USB3/PCIe; resources retained. No trained physical link or visible-output qualification. | `displayport-native-20261020/root-integration.json`; earlier admission/boot/launcher checks retain their image scopes; [preparation limits](displayport-preparation.md) |
| [DSI immutable-frame replay](display-scanout.md#caller-uploaded-immutable-frame-replay) | Two authenticated XRGB8888 uploads selected 0→1→0 through the register command queue (RCQ) at modeled 720p/1080p. Fresh RCQ completion plus later DE frame-end bind copied results; full USB delivery precedes next request. Frames/command pages stay immutable: no retirement, recycling or arbitrary new-frame streaming. No attached panel, physical pixels, electrical qualification or shared startup execution. | `display-frame-present-20261019/root-integration.json`; `root/layout-review.json` in that directory |
| [SPI3 polling/deferred IRQ](spi-application.md#deferred-interrupt-software-evidence) | One authenticated FIFO-sized WRITE_READ on PE0–3; version 2 uses CPU0 INTID59, ISR mask/notify and owner-only completion. Final `149f7635…`: five linked native cases and separate host/root checks. Fourteen clock-parent cases remain separate. Frozen workload-2 image excludes the differing Type-C task. Register/device/scheduler effects modeled; resources retained, physical exchange/IRQ/timing/recovery unqualified. | `spi-irq-20261020/root-integration.json`, `root-frozen/manifest.json` |
| [UART2 FIFO/IRQ/DMA](uart-application.md#finite-dma-transfers) | Finite 1–1,024-byte DMA echo: both channels active before requests; success requires stopped DMA, requests off, true TEMT/RX-empty/idle, copied echo, REAP/CLOSE and UART release. Known-live faults cancel boundedly; uncertainty retains. DMA0 parent/allocation stay owned for board lifetime. Device/cache/RTOS effects are modeled; physical wiring, DMA visibility and inherited startup remain unqualified. | `uart-dma-20261019/root-integration.json`, `root/layout-review.json`; earlier FIFO/IRQ evidence remains separate |
| [I2S duplex/repeated service](i2s-application.md), [cyclic DMA](i2s-stream.md) | Separate TX/RX channels; copied slots survive cancellation until delivery/ACK. Fixed cyclic TX and checked-copy RX/duplex are integrated. Current source copies a cyclic RX period only while one whole period separates it from the DMA writer; that guard postdates the evidence, so rerun `checks/i2s-cyclic.py`. Continuity, cache visibility and wire drain remain unqualified; RX ordering blocker is below. | `i2s-duplex-20261018/integration.json`; `i2s-repeated-20261018/root-integration.json`; `i2s-hardware-cyclic-20261019/root-integration.json` |
| [Runtime I2S PCM/file input](i2s-application.md#complete-file-input-retains-unknown-serial-completion) | Finite TX leases accept BEGIN_FILE/END_INPUT with a short final period. Normal EOF requires exact DMA consumption and FIFO empty; wire completion is unknown. TX/GEN, clocks, pins, catalogue and buffers remain retained; REAP/CLOSE refuse. Explicit CANCEL may truncate; prefix-plus-guards mode remains separate. Native/provider and client/host checks model hardware; physical playback/drain remain unqualified. | `i2s-file-playback-20261019/root-integration.json`; earlier refills: `i2s-runtime-pcm-20261019/` |
| [Repeated keyboard/mimus/storage](../apps/mimus-board/README.md) | Workload 4 rearms only after transcript/sector/trigger delivery and matching ACK-response completion. Each generation needs fresh neutral→Enter and generation-bound storage overlap during unchanged EL0 recognition. Nonwrapping `UINT32_MAX` limit and original session deadline bound reuse; workload 0 remains version-1 one-shot. Lifecycle/transport/hardware/scheduler effects modeled; earlier arithmetic parity reused. Coordinator captures 1–32 generations/default 1 within 59 s/native 60 s; durable results precede ACK, failures preserve earlier output without retry/relaunch. Host fixtures/readiness only. Final `05c4b4d9…` unchanged; physical concurrency/capture/recovery/performance unqualified. | `mimus-repeated-board-20261020/root/integration.json`; host: `mimus-board-supervised-host-20261020/root-integration.json`, `checks-fixed/result.json`, `caller-entry.json` |
| [Healthy Wi-Fi reconnect](radio-session.md#explicit-healthy-reconnect-preserves-the-existing-service), [link-loss recovery](radio-session.md#clean-link-loss-is-recovered-without-disposing-sockets) | Same AP in the frozen evidence; current source also recovers onto another AP of the same network, and board transfers repeat after a timeout during link loss (not frozen). Fixed address or DHCP after socket/IP-task retirement. Fixed OPEN emits PASS; WPA2 owner/session completion is verified from terminal RAM despite reporting assertion/no PASS, corrected fixture unexecuted. Separate actual-IP-task DHCP new-lease/timeout cases pass with association/peer modeled; native board consumer build only. A clean unsolicited link loss now takes the interface offline with sockets kept open and reassociates until the recovery deadline; one host composition covers it. Loss with outstanding work, or a second loss during recovery, retains the radio. No board-poll/startup/physical claim. | `radio-reconnect-20261020/root-integration.json`; DHCP: `radio-dhcp-reconnect-20261020/root-integration.json`; link loss: not frozen; rerun `checks/radio-reconnect.py --fixture loss` |
| [IPv4 TCP listener](board-radio.md#an-admitted-listener-verifies-one-copied-application-exchange) | One admitted peer and copied 1–256-byte exchange each way; backlog one, fenced child/listener disposal. Seven actual-stack model, four app and five host cases pass; native image linked, not executed. Pinned MIT stack uses generated IPv4 reply-checksum correction. Radio-only workload 3 retains inactive older USB host inputs; physical networking, entropy and wire drain remain unqualified. | `net-inbound-20261020/root-integration.json`, `closure.json`, `bringup-impact.json`; stack: [radio-network](radio-network.md) |
| [Repeated Bluetooth recognition](bluetooth-native.md#repeated-headset-capture-and-recognition) | CVSD copied input and delivery-owned transcripts; unchanged recognizer remains EL0. Optional mSBC decodes 16 kHz speech through the same path. Transparent eSCO, receive bounds, acoustic quality and session timing remain physically unqualified. | `bluetooth-repeated-recognition-20261019/root-integration.json`; `bluetooth-msbc-20261019/root-integration.json` |
| [Bluetooth caller PCM](bluetooth-native.md#caller-pcm-playback-and-duplex) | mSBC playback-only and recognition duplex; two immutable 2,400-byte periods, full USB delivery and ticket ACK before reuse. END seals admission; CANCEL can truncate audio. Credits/disconnect account for buffers, not rendering or wire drain. Image-specific case boundary is below. | `bluetooth-pcm-duplex-20261019/root-integration.json`, `profile-gate-delta.json` |
| [Read-only SD](storage-read-application.md) | Legacy one-bit 400 kHz, four-bit default up to CSD/25 MHz, or required high speed at 50 MHz. USB-layout reconciliation was accepted at `894051d`; later headers do not invalidate that frozen image. No writes or physical mode qualification. | `sd-bus-modes-20261019/`; `sd-shared-usb-20261019/root-integration.json` |
| [Optional eMMC reads](emmc-read-application.md) | SDC2/SDC3 identification, EXT_CSD and copied reads pass instruction/host checks. Neither current board has a qualification target. The single-core image retains resources and has no qualified ROM return. | `emmc-native-20261018/` |
| [High/Baseline H.264](h264-native.md#high-profile-runtime-clips-and-presentation-ordering) | High profile 100, progressive 8-bit 4:2:0: CABAC/scaling/weights, I/P/B/reference-B; the frozen image holds two full-HD or four HD short-term references, four active list entries, five reordered copied-output slots. Current source admits three full-HD references in host models; it needs a new freeze. Twelve full-HD + twelve refB-HD + twelve four-ref-HD pictures complete in models; normal mode decodes three/copies two. Physical decode/cache/confinement/drain remain unqualified. Current source adds long-term references in P pictures, memory-management operations, POC and frame_num wrap and Baseline/Main streams, checked with header-rewritten clips in models (not frozen; rerun `checks/h264-high-modes.py`). Long-term B references, unrestricted streaming and broader encoding remain required. Separate Baseline scopes retained. | `h264-high-20261019/root-integration.json`, `closure.json`, `root/layout-review.json`; Baseline: `h264-fullhd-20261019/` |
| [VGA/legacy H.264](h264-native.md) | VGA admits 256 prepared 640×480 pictures, 512 KiB/AU; legacy 48×32 IDR/P remains separate. Input refill, reference retirement and copied-output ACK are distinct. Corrected launcher and supervisor-policy evidence are bounded as below. Physical pixels, reference visibility, confinement and drain remain unqualified; no reclamation. | `h264-vga-20261019/root-integration.json`; `h264-launcher-20261019/root-integration.json`; `h264-supervisor-entry-20261019/result.json` |
| [DI301 WEAVE/motion/fixed TNR](deinterlace-native.md), [host](deinterlace-preparation.md) | Modes 1/2 retain their original scopes. Mode 3 requires reset-held entry and uses two 640×480 NV12 frames with zero history, copying planar YUV420; each guarded flag buffer is 307,200 bytes. Native plan/allocation/resumed completion/serializer and host/SRAM cases pass with hardware/pixels/timing modeled. One allocation replay repaired an omitted checkpoint and reproduced proof/table bytes; completion then resumed without replay. One image/launcher. Image `9a4c1c38…` adds a selectable TNR strength (not frozen). Resources retained; numerical/physical filtering, gain feedback, film detection, repeated jobs and drain remain unqualified or required. | `deinterlace-tnr-20261020/root-integration.json`; prior: `deinterlace-motion-20261020/`, `deinterlace-native-20261020/` |
| [Recorded Bayer ISP0](camera-isp0.md), [host](isp0-preparation.md) | One 640×480 BGGR12→NV16 frame via TOP/ISP600/VIPP0/camera DMA0, with guarded masters 1/2 and immutable copied output. Final `32eb5b93…`: bounded-call nominal, classified failures, serializer/host/allocation and separate boot/SRAM checks. Pixels/device effects modeled; no numerical, full TinyUSB/DCD, scheduler or physical proof. GPL defaults/setters, no opaque tuning/runtime blob; all resources retained after ACK. Sensor-dependent ISP1 path stays separate. | `isp0-native-20261020/`; `root-integration.json` binds the final closure |
| [HEVC decode](hevc-native.md) | One uploaded two-picture clip, Main profile 8-bit 4:2:0 640×480, IDR then P, for decode on the VE with copied NV12 output. `sunstone-a733-hevc` builds; host models check parsing, storage and register programming, and the IDR picture's register writes match a vendor-library trace. Preparation refuses a launch bundle until physical allocation and page-table evidence exists. No numerical or physical decode result. | Not frozen; rerun `checks/hevc-decode-model.py`, `checks/hevc-native-access.py` and `checks/hevc-image.py`; earlier source-only scope: `hevc-host-source-20261020/root-integration.json` |
| [H.264 encoding](h264-encode-native.md) | Two caller 640×480 NV12 frames, Baseline level 3.1/CAVLC/QP 26 IDR→P; immutable copied outputs with delivery-gated ACK. All input/reference/output resources retained. Actual provider/IOMMU/copy/USB and 140 SDK-reference register-write checks pass with entropy/completion modeled; no opaque CPU library linked. Final image `2e4a93b0…`; real entropy, reconstruction, numerical encoding, visibility and drain unqualified. Broader encoding remains required. | `h264-encode-20261020/root/integration.json`; [source provenance](../apps/h264-encode/sources.json) |
| [JPEG encoding](jpeg-encode-native.md) | One retained caller 640×480 NV12 job, stride 640/quality 75; copied JPEG ≤1 MiB. Inherited PCK3, exclusive encoder lease and IOMMU master 3. Provider/IOMMU/copy checks model completion and entropy; native image linked, not executed. No numerical, burst, visibility, drain or physical claim; all resources retained. Frozen workload 2 uses private USB0, with eight inactive radio/USB-host input deltas. Broader encoding remains required. | `jpeg-encode-native-20261020/root/integration.json`, `closure-summary.json`, `commit-binding.json` |
| [JPEG](jpeg-application.md) | Authenticated supplied baseline YCbCr420 image and copied pixels with dimensions; independent oracle optional. Current source admits up to 512 KiB of input and 640×480 planes, and version 3 sessions decode up to eight images in order; the supervised tool still sends one image, and neither change is frozen. Pixel correctness and confinement remain physically unqualified. | `jpeg-runtime-input-20261018/` |
| [Crypto](crypto-application.md#fixed-cold-clock-provisioning-for-caller-requests) | Caller SHA-256/AES-128 ECB; version 3 adds fixed cold CE400 clock/reset provisioning after authenticated reset-held/gated admission. Eighteen owner cases and two separate SRAM cases pass; SHA device output/RTOS effects modeled. One build, prior arithmetic/version-1/2 evidence reused. Current source adds AES-192 and AES-256 keys with CBC, CTR and GCM, SHA-1, SHA-224, SHA-384, SHA-512 and HMAC in the services (not frozen; rerun `checks/a733-ce-modes.py`); the application's request format still offers only SHA-256 and AES-128 ECB. All resources retained; physical exclusion, secure access, operation and drain remain unqualified. | `ce-clock-20261020/root-integration.json`; caller inputs: `crypto-runtime-input-20261018/` |
| [USB HID/CPU IRQ](usb2-host.md) | LS/FS/HS HID copies four reports without desktop events. Supervised board option routes CPU0 INTID187 to the existing owner; other profiles poll. IRQ delivery, DMA/drain, power and attachment reuse remain unqualified. | `usb-host-interrupt-20261018/root-integration.json`; `usb-host-cpu-irq-20261019/root-integration.json` |
| [Gen1/companion leaf](usb2-host.md#one-gen1-hub-or-the-usb2-companion-root-serves-one-leaf) | One selected root, direct or one self-powered hub/leaf: workloads 6/8 Gen1 MSC/boot keyboard, 10/12 USB2 companion keyboard/HS MSC. Typed topology binds the unchanged 592-byte session. Actual stack/copied-client/host plus six identity/layout and two route cases model hardware/IRQ. Native linked, not physically executed. Root/topology/class, ≥900 mA and hub-power admission remain required; no physical bundle. Simultaneous roots and wider USB remain required. Current USB2 source also admits two hub tiers, bus-powered hubs and USB Audio Class 1 capture, which no workload selects. Mimus remains USB2. | `usb-ss-hub-20261020/root/integration.json`, `handoff.md`; earlier direct HID: `usb-ss-hid-20261020/` |
| [USB hub/dual leaf](usb2-host.md) | Workload 4: one self-powered HS USB2 hub, one LS/FS/HS HID plus one HS MSC. Joint immutable output requires both results and fresh attachment. Existing IRQ187 route retained; derived profiles keep workloads 0–3. Hub/TT, power, DMA and drain remain unqualified. | `usb-host-dual-leaf-20261019/root-integration.json`; earlier single leaf: `usb-host-hub-20261019/` |
| [USB MSC/Gen1 BOT](usb2-host.md) | GET_MAX_LUN endpoint reset/dequeue precedes LUN0 fallback and copied READ10; eleven HS cases model hardware, SS recovery is source-reviewed only. Other stalls/uncertainty retain. Workload 6 Gen1 read-only LBA0 refuses USB2 fallback and requires authenticated ≥900 mA plus image-bound COMB0/rail/clock/route/source-switch admission; none exists. The TinyUSB adapter now tears down a disconnected hub child, or a device behind an adapter, through the xHCI detach path (not frozen); a Type-C detach or bulk stall still ends the session. Wider modes and drain remain required. Frozen recovery image retains the older clock policy. | `usb-msc-stall-20261020/root-integration.json`; Gen1: `usb-host-superspeed-20261019/` |
| [LRADC0 raw observations](adc-application.md#lradc0-raw-observations-require-separate-electrical-authority) | ADC version 2 copies two fresh channel-0 raw codes, then configured disable with permanent retention. Final `7f390582…` passes composed/driver/host checks with analog/RTOS/USB effects modeled; raw 0/63 establish no measured value or sample rate. Physical use is blocked by nominal 1.8 V R23 pull-up versus documented 0–1.35 V range, without overrange authority. No key mapping or voltage conversion. | `lradc-20261020/root-integration.json`, `root-frozen/manifest.json`; original and corrected fixtures retain their scopes |
| [Thermal](thermal-application.md), [ADC](adc-application.md), [finite USB](usb.md) | Finite applications and return adapters integrated. Thermal/ADC retain configuration and leases; ADC needs fitted-divider electrical admission. The fixed [thermal policy](thermal.md#a-fixed-board-policy-throttles-and-stops-work) ends a thermal session at the critical level. The CPU clock driver lowers raised clusters at the throttle level, but no image runs it, so no image reduces load there. Physical operations/return remain unqualified. | `thermal-adc-returning-source-20261018/integration.json`; `usb-returning-20261018/integration.json` |
| [RTC source/alarm](rtc-application.md) | Separate source initialization and external-source alarm paths. Source admission precedes alarm use; success requires verified Secure interrupt-state restoration, not only a completion message. | `rtc-source-returning-20261018/integration.json`; `rtc-alarm-returning-20261018/integration.json` |
| [NPU runtime/IRQ](npu-application.md), [service](npu-service.md) | Retained same-model SUBMIT/READ/copied output/REAP/CLOSE; IRQ97 owner alone acknowledges. Bootstrap plus two finite inferences require three notified completions and stopped receipt. Source-off CLOSE/GIC stop precede release; retained runtime keeps OS/USB live without ROM return. | `npu-interrupt-service-20261019/root-integration.json`; `npu-runtime-input-20261019/dma-reconcile/root-integration.json` |
| [NPU two-stage graph](npu-two-stage.md) | Fixed two fused convolution/ReLU stages, 64 input/eight output bytes, internal SRAM and one graph completion. CPU/SDK references pass; native output/service are modeled. No mimus replacement or physical inference claim. | `npu-two-stage-20261019/root-integration.json` |
| [NPU SH/NT graph](npu-relative-shift.md) | Fixed FP32 four-head/128-step relative shift, external pool cleared before each job, chunked copied input/output and healthy CLOSE. Two reused-network SDK outputs match FP32 bits; native replies/IRQ/cache/transport are modeled. No unchanged-encoder acceleration. | `npu-relative-shift-20261019/root-integration.json` |
| [Complete NPU NN graph](npu-complete-nn.md) | Published 125-operation ShuffleNetV2 graph with coefficients/pool; two copied jobs and CLOSE. Both SDK input patterns saturate to `[0,255]`, with undocumented classes: no input-sensitive accuracy, physical inference or mimus acceleration claim. | `npu-complete-nn-20261019/root-integration.json` |
| [Combined GPU/NPU](accelerator-composition.md) | Trusted GPU compute and complete NPU graph share providers and one copied USB owner. Interleaved results/independent CLOSE retain the session deadline; shared faults revoke both. Native and host preparation/capture checks model admission, CPU/RTOS, arithmetic and hardware; NPU outputs remain saturated. Positive bundle substitutes physical NPU validation; production refuses synthetic proofs. Supervisor stops at readiness before queue/USB. Native linked; physical execution unqualified. | `gpu-npu-composition-20261020/root/integration.json`; host: `accelerator-host-20261020/root-integration.json`, `results.json`, `supervisor-entry.json` |
| [G2D fill/copy/blend/2× enlargement](g2d.md), [host](g2d-preparation.md) | Exact 2× ARGB8888 enlargement uses a source crop at half the even destination dimensions. Current `f77be8b2…`: three pinned geometries, five native refusals, one job/copy, checkpointed HS TinyUSB/ACK and separate SRAM/host checks. Before ACK, authenticated output must keep outside-rectangle/padding bytes zero; interpolation remains unchecked. One build/launcher, no prior matrix replay. Earlier success/blend scopes remain separate. Image `08e56fc5…` adds general scaling, image-over-image blending and RGB order conversion with model checks (not frozen). All resources retained; physical pixels/DMA/cache/reuse remain unqualified; rotation and YUV conversion remain required. | `g2d-scale-20261020/root-integration.json`; prior: `g2d-success-20261020/`, `g2d-blend-20261020/` |
| [GPU programs/live input](gpu-compiled-programs.md) | Trusted sealed program, same-shape copied input, exact-sequence READ and CLOSE. Separate multi-dispatch and true 1D/2D grid profiles admit up to 31 workgroups/992 elements with 32 lanes. Numerical execution and device-cache visibility remain modeled/unqualified. | `gpu-dispatch-geometry-20261019/`; `gpu-workgroup-grid-20261019/root-integration.json`; `gpu-runtime-input-20261019/dma-header-integration/root-integration.json` |
| [GPU indexed textures/depth/blend](gpu-render.md#depth-testing-and-source-over-blending) | Through 1080p with trusted shaders; per-frame depth OFF/LESS/LEQUAL/ALWAYS plus writes and straight-alpha source-over. Shared GEOM+FRAG_PR then final FRAG retains both rings through final completion. Old single-final-fragment ownership results are historical; legacy-profile-2 sequencing is separately checked. GPU/firmware and selected app/USB boundaries are modeled. Current source adds graphics profile 4: state batches, stencil, perspective texturing, five blend equations, sampler modes and lending a finished frame to a display consumer (not frozen). Physical graphics, arbitrary shaders and blend factors, multisampling and display use of a lent frame remain required. | `gpu-depth-blend-20261019/root-integration.json`, `closure.json`, `root/layout-review.json`, `reuse.json` |
| [GPU compute cleanup](gpu-compute-service.md), [render cleanup](gpu-render.md#healthy-close-cleans-firmware-objects-before-stopping) | Exact successful context/render-target/free-list receipts precede healthy stop; partial progress stays retained and late reconstruction faults through shutdown. Seven compute/eight render composition cases model firmware/MMIO/isolation. Separate full native builds `dae5a387…`/`6c45bdc3…` compile/link their recorded layouts only; no runtime-result transfer. Default backing/VM stay retained and reopen is refused; optional standalone disposal has separate scope below. Physical cleanup remains unqualified. | `gpu-compute-cleanup-20261020/root-integration.json`, `gpu-render-cleanup-20261020/root-integration.json`; builds: `gpu-compute-cleanup-native-20261020/`, `gpu-render-cleanup-native-20261020/` |
| [Standalone GPU disposal/reopen](gpu-render.md#standalone-sessions-can-dispose-and-reconstruct-child-resources) | Optional sole-client single-core profile releases/rebuilds seven child parents with fresh generations/epochs; two global firmware parents retained. Original `e26b9632…` native leaf checks model stopped admission; final `780d9544…` depth app/TinyUSB protocol models service completion, with thirteen host cases. Two builds, 87/89 objects unchanged after app budget fix. Separate 15 s DISPOSE/rebuild budgets; triangle/version-8 texture runtime and pending texture OPEN remain unchecked; compute-only has separate evidence below. Physical reuse/visibility/SMP and mixed-device reclamation remain unqualified. | `gpu-resource-disposal-20261020/root-integration.json`, `closeout.json`, `final-frozen/manifest.json` |
| [Compute-only disposal/reopen](gpu-compute-service.md#compute-only-disposal-and-reopening) | Image `06315568…` executes actual three-parent release/reconstruction with package/global parents retained, plus seventeen app/TinyUSB exchanges with GPU/service completion modeled. Pending OPEN holds old identity; READY publishes coherent new epoch/result for immediate READ. Two 65-word outputs use pages of 32, 32 and 1 word; healthy host replay and four tail faults retain exact 256-byte prefixes. Physical reuse/cache/SMP and mixed-device disposal remain unqualified. | `gpu-compute-lifecycle-20261020/root-integration.json`, `final-frozen/manifest.json` |
| [GPU returning application](gpu-application.md), [NPU returning application](npu-application.md) | Workers stop and PMIC parks before cleanup. Reviewed Nonsecure publication and matched host restoration are preserved separately from retained live-input profiles. Physical inference and ROM return remain unqualified; critical frozen scopes are below. | `gpu-returning-repeat-fix-20261019/root-review.json`; `gpu-returning-host-20261019/root-host-source-review.json`; `npu-finite-return-20261018/frozen/`; `npu-finite-host-20261018/root-host-source-review.json` |

## Blockers and image-specific evidence boundaries

- **I2S cyclic RX/duplex:** the completed-package counter does not explicitly
  order destination writes and cursor updates, so physical admission requires
  `cyclic-rx-completion-and-reuse-ordering-established`. Only a board
  measurement can meet it: destination writes must trail the counter, and the
  DMA writer must lead the counter and cursor, by less than one period each.
  No such record exists, and CPU-cache review alone is insufficient. The
  receive path keeps one whole period between each checked copy and the
  writer, so the application's four-period ring gives its client one period,
  128 ms, to return each copy. Preserve
  `i2s-hardware-cyclic-20261019/root-cache-review.json` and
  `identity-reuse-clarification.json`; standalone-client cases model service calls.
- **Camera streaming:** the first DMA status interval spans the OV5640
  stream-on sequence, so a slow start can make a stream deliver nothing while
  every slot stays owned ([camera capture](camera-capture.md)). The status
  interval now has a margin under half a frame. Whether a real sensor start
  fits the first interval needs a module on the board.
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
  The DSI image also requires PCK domains 0, 1, 2, 3, 8 and 9 to be ON at
  entry and powers none of them; the DE_SYS power provider can power domains
  1 and 9, but the DSI build does not link it. Radio bootstrap needs at least 46.7 s at one
  data line and 400 kHz, more than the 40 s supervised bound, so the radio
  board test waits for a physically qualified four-bit SDIO profile.

## Continuation and evidence reuse

1. Until the user says physical recovery is available, work offline only. Do
   not run a board launcher, a FEL tool or any program that opens a USB device,
   and do not request a reconnect. The [next offline work](#next-offline-work)
   lists the current priorities.
2. Before the first board test after recovery, bind both queues to their ports
   and prepare again every launch bundle whose launcher changed. Recorded
   storage and mimus bundles hash `tools/check-a733-storage-launch.py`, and
   recorded storage launch bundles also hash `tools/storage-update.py`; both
   tools changed, so the old bundles are refused. Every board-derived image
   now has a new build ID, and software-evidence records that hash the edited
   host tools, `tools/check-a733-smp-supervised.py` and the device runners,
   must be regenerated before the next supervised launch. Use the cached
   eight-core base; the uncached diagnostic is superseded and must not be
   launched unchanged.
3. Run the [ranked board test plan](audit/board-test-plan.md) in its order:
   ROM identity, the returning SRAM probe, DDR initialization, the single-core
   kernel check, the forced-stall return, the boot-clock and thermal report, the
   accepted all-core first-task diagnostic, the full returning eight-core
   workload, and the native USB0 channel last. Each test runs once under the
   port lock with a frozen image and an evidence directory chosen in advance.
   Stop at the first transport failure, preserve evidence and wait for explicit
   recovery; never retry.
4. After the USB0 channel passes, peripheral paths that report through USB0
   become eligible, one bounded test per concrete question. The first run of the
   integrated OS image uses the supervised variant; its launcher exists, and it
   needs a reviewed entry record built from physical evidence first. Record the
   measurements that the rounds left open where a test reaches them:
   `ID_AA64ISAR0_EL1` on every core, the CPU PLL pattern and low-frequency-mode
   words that boot0 leaves, the longest pause of each resident service loop,
   PMIC BUSY durations, mimus load and recognition time, and the DMA0 and GPU
   interrupt delivery.
5. Tests that need more physical recovery come last, after the user confirms
   power-cycle and reconnection. They include card writes, after both cards are
   backed up; the stage-2 loader, a read-only snapshot of the remote-writer
   registers after a cold boot, and the resident lifetime; intentional
   watchdog expiry; CPU clock or PMIC voltage changes, each with the user's
   approval; and tests that need a camera, display, microphone, headset, PCIe
   endpoint or access point. Keep the failed
   historical UFS probe disabled; absent eMMC/UFS are not current qualification
   blockers.
6. Qualify unchanged-model mimus and representative concurrent OS workloads.
   After every required hardware function works, optimize complete recognition
   to at most 0.05 times audio duration; investigate 0.01 times as a bounded stretch.
   Report loading and live latency separately. Reproducible boot images,
   recovery/update procedures and physical qualification remain goal requirements.

Evidence reuse follows these rules:

- Reuse accepted checks while their relevant inputs match; a commit,
  documentation edit or handoff is not a rerun reason. Follow
  [selective validation](validation.md) and the [reference index](reference-index.md).
  Tables above select continuation records; their linked component documents
  preserve full evidence inventories.
- Preserve each accepted ELF, full source/object closure, generated inputs and
  evidence scope. The fix rounds rebuilt many images: a frozen image keeps its
  recorded evidence and scope, and a rebuilt image needs new evidence. Never mix
  current objects into a frozen image. An integration audit record matches only
  its named revision; do not rebuild old images merely to catch up.
- Shared USB headers change embedded layouts even in consumers whose USB host
  path is inactive. Future builds compile affected consumers consistently.
- The shared launcher correction `6407e26` preserves the exact legacy
  preprocessed templates and the combined binary; it adds no physical launch
  evidence.
- The fix packages left their check outputs under
  `~/.cache/sunstoneos/build/audit-*/`. Those are working outputs; freeze
  evidence before treating a new image as accepted. The radio
  fixes are the exception; their evidence is under
  `~/.cache/sunstoneos/evidence/audit-radio-20261020/`.
- Configure a fresh CMake build directory with the compiler and all dependency
  paths supplied together; changing the compiler in an existing cache can clear
  those paths. Save a complete resumable execution state before an
  allocation-only fixture exits.

### Boot chain, DMA authority and clock references

The [microSD container packager](microsd-boot-package.md) creates regular files
only; one actual packer invocation and seventeen host checks pass. Root audit:
`microsd-boot-package-20261020/wrapper-root-integration.json`. The
[cold-entry recorder](cold-entry.md) supplies a guarded 4,164-byte native
monitor that captures header/exception state and parks. Actual boot0 parser,
cleanup/main-tail and eight AArch64 entry cases pass with independent reset-state
models; one ordinary-file container contains the actual monitor. Evidence:
`cold-entry-20261020/frozen/manifest.json`, `instructions/result.json`.
The actual parser cache wrappers separately execute twenty synthetic set/way
operations before disabling the data cache under `cold-boot-cache-20261020/audit.json`.
The [stage-2 loader](boot-slots.md) admits the SD controller state that boot0
leaves and builds the integrated OS image's resident contract in host and
instruction models. On a board its three entry prerequisites refuse, so it
enters no image. The [remote-writer check](remote-writers.md) reads the reset,
clock and DMA-enable state of every DRAM bus master without writing, but it
cannot prove the OR100 processor or the crypto engine excluded, and no boot
image calls it. Memory ownership, physical boot, media writes and recovery are
not established either, so standalone OS boot remains incomplete. Packer license/source equivalence remains unresolved. The primary
agent owns cold-entry/component authority and CPU integration.

DMA fabric admission accepts correctly normalized 24 MHz references from
supported raw oscillators and retains REF in every admitted profile. Ten focused
platform/clock-registry sanitizer cases pass under
`dma-clock-reference-20261020/frozen/manifest.json`; no native image or physical
DMA qualification is added. [DMA1 applicability](dma.md#dma1-request-routing-remains-unresolved)
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
admission in TWI11/USB2/radio/CE/I2S/PWM, the video engine and PCIe.
[SPI](spi-platform.md) has separate parent checks. Raw S_TWI1, LED source 0 and
S_SPI source 0 retain their recorded restrictions. [HDMI](provider-clock-reference.md#hdmi-native-evidence)
binds raw 24/26 MHz TOP input separately from normalized 24 MHz SFR and retains REF.
Actual compiled-object raw-26 720p prepare/start/retained-stop and saved-state
refusals pass under `hdmi-ref26-20261020/root-integration.json`. Calculated pixel
rates are 73.9375/147.875 MHz; 1080p has source/host evidence only. Physical timing
and output remain unqualified; the earlier GPU image retains its frozen scope.
Other function checks do not qualify images or physical clocks.

## Next offline work

Round seven comes next. It first consolidates helpers that several drivers
duplicate, then finishes the items that rounds four to six left partial. All
of it is offline work with host builds, emulators and host models.

### Consolidate duplicated helpers

| Helper family | Deferred findings | Scope and limit |
| --- | --- | --- |
| Task-context checks | cpu-startup-9 (with clock-power-pins-12, x-concurrency-8, x-duplication-6, buses-pcie-9, security-misc-10) | About 15 native adapters and FreeRTOS port internals; the primary agent owns the port. |
| Address-range and identity-range helpers | x-bounds-4 (with x-duplication-9), runtime-memory-6 (with x-coherence-4) | About 190 range checks, and the GPU, USB, NPU, HDMI-audio and DMA mapping checkers. |
| MMIO accessors and cache-operation codes | x-coherence-6, x-coherence-7 | About 59 files, and the DMA and NPU headers. |
| CPU-to-device ownership changes and memory routines | runtime-memory-7, runtime-memory-8 | About 35 call sites; merging `memmove.c` into `memory.c` edits many image source lists. |
| I2C engines and power bridges | clock-power-pins-9, clock-power-pins-10 | Removing the separate I2C engine edits GPU, NPU, check and single-core build files; moving the PMIC onto the shared TWI engine also needs board evidence of the S_TWI0 FIFO and DMA state after boot. The DCDC2 and DCDC4 users must move together. HDMI and DisplayPort can replace their VO1 power copies with the shared DE_SYS provider. |
| Interrupt bindings, EL3 snapshots and extended contracts | x-concurrency-6, supervisor-apps-6, supervisor-apps-3 (with bringup-gaps-11) | About 11 task-side interrupt bindings, the EL3 boot hooks of about 24 applications, and the private extended records of at least seven applications. |
| Build patch functions | The rest of build-system-13 | About 30 standalone projects keep their own fuzzy patch function. |

Each consolidation changes the identity of every image it touches. Rebuild
those images and record new evidence; never mix new objects into a frozen
image.

### Finish the partial items

| Workstream | Next deliverable | Ownership and limit |
| --- | --- | --- |
| Stage-2 entry | Supply the three entry prerequisites in `boot/a733/stage2/admission.c`: remote-writer exclusion, watchdog recovery after a cold boot, and a comparison of boot0's EL3 entry state with the FEL-launched reset state. Call the remote-writer check before the loader reads the card. Add a faster loader SD profile; the 400 kHz one-bit profile reads about 25 KB/s, so 14 MB of models take about ten minutes. | Primary agent only. Exclusion needs a source that documents an OR100 reset or removes OR100 from the A733, and a crypto engine idle status or control-flow proof. Do not repeat the E902 search or infer safety by reversing startup writes. |
| OS image services | Carry a CPU clock grant record in the OS session, by default one that grants nothing, and check it with `a733_cpu_clock_grant_load`. Start the CPU clock driver after the thermal policy, with an owner task that polls within the driver's 10 ms margin, and add a reduced-load response to THROTTLE. Report measured cycle timing with each recognition. Stage the radio and Bluetooth profile and firmware in supervised preparation. | Application owner. A grant that permits a physical change needs the user's approval for that change. |
| Radio and Bluetooth | One owner that loads the AIC8800 firmware once and serves Wi-Fi over SDIO and Bluetooth over UART1; alternate access points in the board profile; a native consumer of the A2DP sink; the mSBC headset profile in the OS image. | Radio and Bluetooth owners. WPA3 waits for the user's decision to pin an elliptic-curve library. |
| I2S receive | An eight-period cyclic receive ring for directions 3 and 5 in `apps/i2s`, and the launch condition in `apps/i2s/prepare.py` restated as a measured margin. | Audio owner. `I2S_PERIODS` also sizes the finite, duplex and USB transfers, so it must not simply be raised. |
| USB host | Adoption of a new Type-C attachment after VBUS removal; Bulk-Only Transport reset and bulk clear-halt recovery; a board workload and wire format for audio capture. | USB host owner. Storage recovery needs changes to the pinned TinyUSB storage class and to bulk-endpoint recovery in the core. |
| Display | Add `drivers/power/a733_de_sys_power.c` to the DSI and deinterlace builds and power their DE_SYS domains; an application that streams frames through the replay ring; display use of the GPU's lent frame. | Display owner. DSI also needs PCK domains 0, 2, 3 and 8 for its IOMMU group, and domain 8 is USB2. |
| Camera | A CAM2 binding: a port field in the camera boot record and `apps/camera/prepare.py`, the PK10–PK19 data pins and MCLK2, and a sourced deskew value for receiver port 1. Open the first stream status interval at the sensor's stream-on write. Continuous capture in the native application. | Camera owner. The stream-on change needs a hook inside the sensor's start sequence. |
| GPU and NPU | GPU compute input and output beyond one 4 KiB page, arbitrary trusted shaders, blend factors, clipping, mipmaps and multisampling. A board image that links the NPU session runtime, with a USB client protocol per model. | GPU and NPU owners. Multi-page compute changes the package wire format, so it is a package of its own. Any mimus offload to the NPU needs an accuracy evaluation. |
| Codecs, G2D and DI301 | Supervised capture of JPEG sequences; repeated JPEG encode; H.264 encoding beyond two frames; an extended uncached pool for full-HD JPEG; G2D rotation once `g2d_rotate.c` is pinned, and YUV conversion; repeated DI301 jobs in one session, with gain feedback and film detection. | Video and graphics owners. Long-term B references stay refused until hardware evidence shows how the engine treats them. |
| PCIe, storage, serial and timing | A supervised PCIe image that links the function client and NVMe driver; a consumer of the FAT filesystem, with its FatFs pin added to `sources.lock.json`; an S_PWM clock-registry scope; GPADC window admission in the native adapter once an application selects windows; an image that runs the SNTP client with the RTC service; the new crypto modes in the crypto application's request format. | Each component owner. |

Rounds four to six also reported defects in files the packages did not own.
Each file's owner fixes them; the [audit record](audit/README.md) gives the
details per package:

- `apps/crypto-single-core/main.patch` and `apps/pwm-single-core/main.patch`
  no longer apply, so the central build cannot configure
  `SUNSTONE_A733_CRYPTO` or `SUNSTONE_A733_PWM`.
- `tools/check-a733-pwm-supervised.py` refuses 600 ticks, the fan's full speed.
- These checks fail on their current images or fixtures: the storage-read
  native, binding and service checks with a fresh fixture,
  `checks/jpeg-supervised-host.py`, `checks/jpeg-sealing-native.py`,
  `checks/gpu-display-launcher.py`, `checks/usb-host-superspeed-native.py`,
  `checks/monitor-application-native.py`, `checks/crypto-runtime-native.py`,
  the I2S application's native parent-and-adapter case and the NPU
  convolution model's startup mode. `checks/board-application.c` lacks HID
  and xHCI mocks. `checks/aic8800-dispatch.py` and
  `checks/a733-radio-session.py` stop at configuration, because their
  generated toolchain file lacks the path maps.
- The GPU display fixtures need regenerating with
  `checks/gpu-display-preparation.py`, because their trusted-program record
  pins the hash of `drivers/gpu/pvr_render_texture.c`.
- `checks/crypto-application-package.py` needs the bound queue, and an unused
  function in `drivers/video/a733_video_encode_platform.c` fails `-Werror`.
  Both PCIe `prepare.py` scripts still accept a 250 ms startup bound. The
  camera record version must change in five files at once. Sixty-eight
  sanitizer checks lack `-fno-sanitize-recover=all`.
- Documents that are now stale: `include/a733_gpu_resources_native.h`,
  `docs/mmc-platform.md` and `docs/hdmi-native.md` call the PMIC provider
  read-only, although it accepts the CPU clock driver's DCDC3 and DCDC5
  selector writes. `docs/board-radio.md` says the poll budget covers the
  largest 4,096-byte receive, and `docs/camera.md` says CAM2 still needs a
  native TWI9 controller.

Use Sol 6.1 medium implementation workers and focused Astra correctness review.
Keep each native path and its host preparation consistent before building. Use
separate worktrees and coordinate shared interfaces before concurrent edits.
CPU, GIC, startup and multicore work remain the primary agent's responsibility.
Resume checks from saved state and reuse successful evidence while its relevant
inputs match. Defer performance optimization and recurring measurements until
required hardware functionality is established. Update canonical hardware and
component records and existing issues for accepted changes.

## Worktree cleanup preserves unfinished work

A cleanup on 2026-10-20 removed 327 of 364 registered worktrees. Each removed
checkout had integrated or patch-equivalent commits and no local files beyond
verified disposable Python bytecode. All branch references were preserved, and
the external source, build and frozen-evidence directories remain intact. The
fix rounds have since added one worktree per package; 112 worktrees are
registered now. Round five reviewed and integrated the `hevc-decode` sources;
that worktree and the unfinished `h264-interframes` worktree remain available. Do not discard retained changes as part of another cleanup
without review.

The cleanup inventory, branch identities, removal journal and final result are under
`~/.cache/sunstoneos/evidence/worktree-cleanup-20261020-204010/`.
`plan.json` records each original path, branch and commit; `result.json` records
the completed cleanup. Recreate a removed checkout at its recorded path with
`git worktree add <path> <branch>` when an old build or reference requires it.
Removing checkouts does not invalidate their frozen test evidence or justify
rebuilding their images.

The [historical handoff](handoff-history-20261019.md) preserves older integration,
recovery and validation context; it supplies no current launch authority.
