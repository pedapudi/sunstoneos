# Read the section needed for the current decision

This index locates recurring references. Current support claims remain in the
hardware matrix and handoff; physical authority remains in the actual evidence
and live board/queue state. Read required project instructions on initial entry.
Reuse prior reading and inspect relevant changes on subsequent visits.

## Repository references

| Decision | Read |
| --- | --- |
| Scope and user constraints | [Goal](goal.md), [working rules](../AGENTS.md) |
| Current board state and next work | [Handoff](handoff.md): “Board state and recovery” and “Continuation and evidence reuse” |
| Device coverage | [Hardware matrix](hardware.md): the relevant device bullet |
| Physical access and recovery | [Board skill](../.agents/skills/sunstone-a733-board/SKILL.md) |
| Boot-input admission versus forced-FEL normal return | [Reset-path guard classification](watchdog.md#choose-the-boot-input-guard-from-the-reset-path) and [captured ROM proof](watchdog.md#captured-board-rom-and-reset-ownership); [GPU actual-input audit](../apps/gpu-finite/HOST.md#failed-physical-attempt-and-input-audit) separates accepted inputs from unobserved native execution |
| Multicore implementation | [CPU skill](../.agents/skills/sunstone-a733-multicore/SKILL.md), then its boundary-specific source table |
| Peripheral implementation | [Device skill](../.agents/skills/sunstone-a733-device/SKILL.md) |
| Effective GPU reference selection and power sequencing | [GPU source audit](gpu-power.md#audit-of-the-nonreturning-identity-probe): default Zero 3W BXM build selection, source-local branches and limits of failed-launch evidence |
| NPU local shutdown versus domain power-off | [NPU CLOSE contract](npu-startup.md#local-stop-after-close): established scoped ownership, observed retirement and missing A733 power-transition guarantees |
| Architectural constraints | [Arm/Linux audit](a733-multicore-reference-audit.md): the relevant named section |
| Boot0 monitor header, cleanup and guarded cold entry | [Cold-entry recorder](cold-entry.md): mutable header/loadword, independent reset-state model and parked report; [parser cache cleanup](cold-entry.md#the-parser-cleans-cpu-caches-before-returning) precedes the main-tail cleanup and has separate instruction-model evidence. [Boot evidence](boot.md) and [ordinary-file packaging](microsd-boot-package.md) retain separate limits |
| Boot ROM and boot0 vendor-binary behaviour | [Boot ROM and boot0 reference](vendor-firmware/boot-rom-and-boot0.md): identity and provenance, media/FEL dispatch, CPU PLL and the N=38 rate, PMIC rails, DRAM training, package parse and handoff, and the shared-SRAM routing bit; curated inventories under [vendor-firmware/data](vendor-firmware/data/) |
| CPU diagnostic result | [Supervised startup](a733-smp-supervised.md): the matching checkpoint heading |
| Cache mappings and publication | [CPU coherence](cpu-coherence.md): “EL3 and nonsecure EL1 use matching memory attributes” and “Cache entry requires published immutable memory”; also the audit's “Publication must cover the executable physical address space” |
| Raw oscillator versus normalized reference admission | [Provider clock reference](provider-clock-reference.md): TWI11/USB2/radio/CE/I2S/PWM parents, raw S_TWI1/LED restrictions and [HDMI independent references and ownership](provider-clock-reference.md#hdmi-native-evidence); [SPI platform](spi-platform.md) distinguishes main SPI REF24 from raw S_SPI; [DMA platform](dma-platform.md#admission-verifies-the-selected-parent-and-programmed-rate) retains REF for normalized fabric parents. Function checks do not qualify images or physical clocks |
| Internal HDMI audio route and finite PCM ownership | [HDMI audio](hdmi-audio.md): I2S3/DMA0 request 6, normalized AUDIO1 and automatic CTS, retained non-cacheable buffers and unknown serializer drain; [offline preparation/decoder](hdmi-audio-preparation.md) provides no live capture |
| LRADC0 raw sampling and board electrical mismatch | [ADC version-2 contract](adc-application.md#lradc0-raw-observations-require-separate-electrical-authority): R23/AVCC versus documented input range, independent electrical authority, fixed channel-0 observations and retained RTC32K/exclusion requirements; [source manifest](../apps/adc/lradc-sources.json) |
| DMA0 consumer Nonsecure access authority | [Host admission](dma-pool-session.md#the-host-command-reuses-the-sealed-compact-launcher): DMA-copy, UART DMA, I2S and HDMI audio share external session-bound channel, controller and descriptor/data permissions through recovery. Consumer-specific configuration and supporting records stay bound and frozen; no safe security-register (`+0x20`) probe |
| DMA1 register layout and unresolved request routing | [DMA1 applicability audit](dma.md#dma1-request-routing-remains-unresolved): common register layout and dedicated gates are source-defined; the CPUX request table does not establish DMA1/CPUS DRAM encoding. Fixed-profile ownership and established Nonsecure access are required before the deferred finite copy path; security-register visibility is unresolved |
| CE local clocks from reset-held/gated state | [Fixed cold CE provisioning](crypto-application.md#fixed-cold-clock-provisioning-for-caller-requests): source-ordered local writes, MBUS-neighbor preservation, retained REF/PERI0 ownership and independent prior-exclusion requirements; [source manifest](../apps/crypto/clock-sources.json) |
| DE_SYS domain 1 initialization for G2D | [G2D resources](g2d.md#resources-remain-owned-through-every-outcome): pinned PCK600 delay/request order, coherent OFF/ON admission, fixed polling/mutex bounds and retained failure; [host authority scope](g2d-preparation.md#focused-software-checks) |
| ISP600 processing without a camera | [Recorded ISP0](camera-isp0.md): effective GPL defaults/setters, TOP DDR input, parameter extents and masters 1/2; [preparation/capture](isp0-preparation.md) binds actual allocation tables, copied output and model limits. Sensor ISP1 pass-through remains separate |
| DI301 field processing and source selection | [Native deinterlacing](deinterlace-native.md): effective SUN60IW2 DI301 selection, GPL configuration adaptation, masked register publication and retained master-9 lifetime; [host preparation](deinterlace-preparation.md) |
| Evidence reuse | [Validation](validation.md): “Reuse valid evidence” |
| Dependency provenance | [Sources](sources.md) and the component's source manifest |

Find a heading with `rg -n '^## ' path`, then read the needed line interval with
`sed -n 'START,ENDp' path`. For a known document, search its relevant phrase or
symbol directly. Use `git diff <last-read-revision> -- path` to inspect changes;
include uncommitted changes. Do not fetch unrelated sections to fill an output
budget. When a command truncates, narrow the query instead of raising the limit.

## Cached primary sources

Paths below are relative to `~/.cache/sunstoneos/sources/`. Downloads and extracted
text stay outside Git. `cpu-audit/reference-cache.json` records PDF/text hashes
for the four Arm documents. The audit supplies their source links and context.

| Reference | Local path | Frequent anchors |
| --- | --- | --- |
| Arm TrustZone for Armv8-A, version 1.0 | `cpu-audit/arm-trustzone-armv8-a.txt` | 3.2 register state; 3.4 physical addresses; 3.5 cache maintenance; 3.6 TLBs |
| Arm GIC architecture, IHI 0069G | `cpu-audit/arm-gic-architecture-ihi0069g.txt` | 11.1 interface ordering; 12.11.12 implemented group bits |
| Arm GIC software overview | `cpu-audit/arm-gic-software-overview.txt` | 4.2.1 CPU-interface initialization |
| Cortex-A55 technical reference manual | `cpu-continue/cortex-a55-trm.txt` | 2.4.8 powerdown; 2.6.2.3 disabled-cache behavior |
| Linux boot contract, pinned v6.17 | `cpu-audit/linux-e5f0a698b34ed76002dc5cff3804a61c80233a7a/Documentation/arch/arm64/booting.rst` | “Caches, MMUs”; “Coherency” |
| TF-A exception context | `cpu-coherence/lib/el3_runtime/aarch64/context.S` | `save_and_update_ptw_el1_sys_regs`, `restore_ptw_el1_sys_regs` |
| TF-A cache maintenance | `cpu-coherence/lib/aarch64/cache_helpers.S` | Range maintenance and set/way operations |

Search a cached text file with `rg -n -C 4 'specific phrase' path`. Read neighboring
lines only as needed. PDF page numbers printed in extracted text preserve the
source location. Inspect the PDF when a diagram or extraction ambiguity matters.
An unavailable cache entry calls for one download from the existing source pin,
not another broad search. Reuse findings from the reference audit; add a concise
source-backed correction there when new evidence changes them.
