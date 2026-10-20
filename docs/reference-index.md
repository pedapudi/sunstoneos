# Read the section needed for the current decision

This index locates recurring references. Current support claims remain in the
hardware matrix and handoff; physical authority remains in the actual evidence
and live board/queue state. Read required project instructions on initial entry.
Reuse prior reading and inspect relevant changes on subsequent visits.

## Repository references

| Decision | Read |
| --- | --- |
| Scope and user constraints | [Goal](goal.md), [working rules](../AGENTS.md) |
| Current board state and next work | [Handoff](handoff.md): “Observed host and device state” and “Next actions” |
| Device coverage | [Hardware matrix](hardware.md): the relevant device bullet |
| Physical access and recovery | [Board skill](../.agents/skills/sunstone-a733-board/SKILL.md) |
| Multicore implementation | [CPU skill](../.agents/skills/sunstone-a733-multicore/SKILL.md), then its boundary-specific source table |
| Peripheral implementation | [Device skill](../.agents/skills/sunstone-a733-device/SKILL.md) |
| Architectural constraints | [Arm/Linux audit](a733-multicore-reference-audit.md): the relevant named section |
| Boot0 monitor header, cleanup and guarded cold entry | [Cold-entry recorder](cold-entry.md): mutable header/loadword, actual parser→main-tail cleanup, independent reset-state model and parked report; [boot evidence](boot.md) and [ordinary-file packaging](microsd-boot-package.md) retain separate limits |
| CPU diagnostic result | [Supervised startup](a733-smp-supervised.md): the matching checkpoint heading |
| Cache mappings and publication | [CPU coherence](cpu-coherence.md): “EL3 and nonsecure EL1 use matching memory attributes” and “Cache entry requires published immutable memory”; also the audit's “Publication must cover the executable physical address space” |
| Raw oscillator versus normalized reference admission | [Provider clock reference](provider-clock-reference.md): TWI11/USB2/radio/CE/I2S/PWM parents, raw S_TWI1/LED restrictions and [HDMI independent references and ownership](provider-clock-reference.md#hdmi-native-evidence); [SPI platform](spi-platform.md) distinguishes main SPI REF24 from raw S_SPI; [DMA platform](dma-platform.md#admission-verifies-the-selected-parent-and-programmed-rate) retains REF for normalized fabric parents. Function checks do not qualify images or physical clocks |
| Internal HDMI audio route and finite PCM ownership | [HDMI audio](hdmi-audio.md): I2S3/DMA0 request 6, normalized AUDIO1 and automatic CTS, retained non-cacheable buffers and unknown serializer drain; [offline preparation/decoder](hdmi-audio-preparation.md) provides no live capture |
| CE local clocks from reset-held/gated state | [Fixed cold CE provisioning](crypto-application.md#fixed-cold-clock-provisioning-for-caller-requests): source-ordered local writes, MBUS-neighbor preservation, retained REF/PERI0 ownership and independent prior-exclusion requirements; [source manifest](../apps/crypto/clock-sources.json) |
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
