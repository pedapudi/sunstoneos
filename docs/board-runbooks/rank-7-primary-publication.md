# CPU0 preparation before another all-core launch

Status: **offline checks pass**. The coordinator owns recovery, admission and
one bounded attempt on port 3-1. This proposal does not bind a live USB instance.
The package is
`~/.cache/sunstoneos/evidence/cpu-startup-resolution-20261024/`.

The first-task diagnostic's failed launches have no returned checkpoint before
secondary release. Its pre-release admission runs only after the complete
image's boot and publication prefix. The CPU0-only diagnostic stops at that
earlier boundary, before any GIC write or secondary power request. It can
separate a failure in that prefix or its return from the later all-core path.
It cannot locate an instruction if no report returns.

## Executed interval and ownership

The unchanged overlay calls the frozen image's actual `a733_cached_prepare`
from its actual boot path. The native entry establishes the stack, clears BSS,
sets the counter frequency and stops the three architectural timers. Preparation
builds and publishes the translation tables and image memory, validates CPU0's
identity and blocks EL1 table walks. The overlay checks that EL3's SCTLR, TCR,
TTBR0 and MAIR match their saved values, then restores and checks EL1's SCTLR
and TCR. A mismatch parks without requesting reset.

Successful restoration reaches the existing CPU0 return: two 32-bit writes
set its reset vector to ROM address `0x20`, then `RMR_EL3=2` requests an
AArch32 warm reset. No GIC, secondary power, CPU clock or voltage register is
written. The host retains and restores every RAM reservation after ROM return.
The return still changes CPU0's reset vector and enters the ROM; it is not a
state-neutral observation of the powered lifetime.

The package preserves native ELF SHA-256
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`
and overlay SHA-256
`15050bd72572bb54239661418ef1efb66ba7ad41ba807a8aa0fe9e2335c1f33f`.
Its build manifest is byte-identical to
`primary-publication-probe-20261018/prepared/manifest.json`, the manifest of
the passing `physical-108` run. The historical `build/` directory has the same
executable but an older source-label hash; it is not this package's input.

## Source audit and remaining unknowns

| Boundary | Evidence | Consequence |
| --- | --- | --- |
| Launch and CPU0 reset | The launcher and native uploads match the historical pass. The clean failed lifetime had no earlier native RMR return; its cache-off helper returns through `BX LR`. | Uploaded bytes do not locate the failed execution. The smaller diagnostic includes the first native transition and the existing ROM return. |
| Translation and cache state | The Arm TrustZone guide distinguishes Secure and Nonsecure cache tags. The pinned Linux boot contract requires publication and instruction visibility. | Upload comparison alone is insufficient. The diagnostic reuses its accepted publication path; no broad cache discard or new mapping is added. |
| GIC | The 32-bit inherited-state captures match. IHI 0069G permits SRE access while a redistributor sleeps; Linux and TF-A differ in permitted ordering. | No clock, reset or SRE-order correction follows from the missing return. This diagnostic stops before GIC initialization. |
| Secondary power | The sampled power/controller words match the earlier OFF state. Linux delegates CPU power-on to PSCI firmware. | Neither these reads nor Linux's kernel driver establish execution after a direct A733 power request. This diagnostic releases no secondary. |
| Shared cluster control | The observed DSU identity is `0x41`. The coherence plan selects no shared-cluster workaround for that revision and skips zero masks. The actual CPU0 instruction check rejects any such register write. | The presence of a CLUSTERACTLR writer in the native image does not show that earlier diagnostics changed it. Other unsampled shared state remains unknown. |
| Clock, supply and thermal state | The passing lifetime's MBUS value is inferred from ROM re-entry; the clean failed lifetime did not capture it. No electrical trace covers the failed interval. | No frequency or supply cause is established. No clock or voltage change is proposed. |

Primary references and immutable pins are in the
[Arm/Linux audit](../a733-multicore-reference-audit.md). The shared-cluster
selection is in `platform/a733/cpu_coherence.c`; the pinned TF-A source is
revision `0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e`. The returning overlay is
`boot/a733/primary-publication-probe.S`. The
[previous diagnosis](rank-7-diagnosis.md) retains the physical comparisons.

## Focused checks and budgets

No native instruction or runtime hot path changes. The report remains 256
bytes and the overlay reservation remains 16 KiB. The native startup deadline
and host return bound remain those of the existing returning profile; this
package neither extends them nor retries a failure.

The actual-instruction check covers normal boot/publication/restoration and a
restoration readback failure that retains execution without reset. Two further
boot cases use nonzero inactive EL1 or EL3 translation controls. Both restore
every supplied value and perform only the two CPU0 reset-vector writes before
the RMR request. These inputs are synthetic; they do not represent a capture
of either failed lifetime. Five existing host cases cover return, transport
failure, missing return, invalid report and changed-input refusal.

`instruction.json`, `host/software.json`, `snapshot/manifest.json` and
`ready-proposal.json` bind the results and inputs. The template's historical
admission is not current authority. The coordinator must supply current cold
DDR, cache/ownership, survey, recovery and admission records before running
the existing `primary-publication-return-v1` profile.

After a returned report, review its nonce, restored controls, ROM identity and
RAM restoration before deciding whether to run the separately preserved
[all-core pre-scheduler proposal](rank-7-pre-scheduler.md). A successful CPU0
return changes the powered lifetime; later all-core success would not prove
that an earlier reset state caused the failure. A missing return retains all
ownership and requires physical recovery. Neither outcome establishes sustained
eight-core scheduling, active-load shutdown, power transitions or thermal
robustness.
