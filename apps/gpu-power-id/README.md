# GPU power and identity diagnostic

This returning single-core diagnostic powers GPU_TOP and GPU_CORE, reads the GPU
hardware identity, powers CORE and TOP off, and parks the shared PMIC service. The earlier TOP-only image
`fd3d5839` did not return on hardware, without an observed failure stage. This
corrected candidate is checked in software only. GPU power and identity operation
remain physically unqualified.

The diagnostic creates no firmware, command buffers, GPU mappings or DMA
backing. Its successful return establishes a narrower boundary than firmware
startup or arithmetic execution. It cannot locate the instruction that stopped
an earlier full GPU image from returning.

## The never-booted identity path has an upstream unwind

The reference is the board Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, already pinned by the GPU sources.
Its `imagination/pvr_device.c` powers the device before reading control registers
(lines 680–739). GPU initialization reads identity before requesting firmware
(lines 595–603). Failure there unwinds through synchronous runtime suspend.
`imagination/pvr_power.c` disables firmware during suspend only when firmware
has booted (lines 353–376).

That order supports this diagnostic's OFF transition when the admitted initial
state has never executed GPU firmware or commands. Closed local gates and an
asserted external reset are required, including when GPU_TOP is inherited ON.
Those register bits alone do not establish absence of prior execution or DMA.
The coordinator must establish that history and exclude every other writer
through return. This path does not provide recovery from a firmware hang.

The reference files are preserved with the candidate. Their SHA-256 identities
are `e81e5b2126db83517fc7435a96e0fddb48e6364b255747f300f448d10255805d`
for `pvr_device.c` and
`5ab4ff9c54cd8060f0feb6b5c8320da5000c1c24a09f30fbf8c03ef283b732d5`
for `pvr_power.c`. The identity decoding retains the Imagination notice in
[main.c](main.c); its license is [LICENSE.pvr](../../drivers/gpu/LICENSE.pvr).

## Existing owners perform power transitions

The initializer creates the existing PMIC service without a display-domain row.
The GPU platform registers its sole domain-5 row before scheduler startup.
Its retained DCDC4 lease uses the shared owner's actual boot observation.
The service verifies supplies and the GPU resource manager checks the inherited
400 MHz parent and enabled 800 mV DCDC4 before local clock changes.

The [resource manager](../../docs/gpu-resources.md) may enable the shared PPU,
AHB and MBUS gates. The [power owner](../../docs/power-domains.md) adopts coherent
GPU_TOP policy/status 0 or 8. When ON needs a transition, it programs the pinned
PCK600 delay row and requests ON. Under the exclusive LOCAL lease, the GPU controller then captures CORE6
policy/status/PWCR. It requires whole policy/status words to match at 0 or 8,
writes PWCR zero and policy 8 with readbacks, and progresses local clocks/reset.
After that progress it checks CORE status 8 before identity access. It selects
the existing 400 MHz parent, opens local clock/bus gates, waits for settling
and releases external reset in the existing order. No PLL retuning or voltage programming occurs.

Only these identity reads reach the GPU register window:

| Address | Width | Meaning |
| --- | --- | --- |
| `0x01800020` | 64 bits | Packed branch, version, units and configuration (BVNC) |
| `0x01800020` | 32 bits | Legacy revision, only if packed branch is zero |
| `0x01800018` | 32 bits | Legacy core identity, under the same condition |

Each read requires an identity Device-nGnRnE mapping, held platform ownership,
and monotonic deadline checks before and after access. The normal format costs
one GPU read; the legacy format costs three. There are no GPU register writes.
The expected identity is `36.56.104.183`. A different identity is still a valid
observation if OFF and provider park complete; it does not qualify that GPU.

OFF first writes CORE policy zero and checks status zero with TOP and local
clocks still available. It then asserts external reset, closes local bus/clock
gates, requests domain-5 OFF, and releases the platform’s resource token.
Resource release independently checks CORE policy and status zero. Completion requires
the actual OFF phases, no token or delegated lease, an answered OFF row with no
count, a healthy OFF domain with no users, and no retained client resource slot.
After service park, the client checks those conditions again and refuses any
remaining PMIC request or borrow. The DCDC4 stable rail lease remains held until
the image lifetime ends. Shared gates remain enabled for their other users.

The client uses the existing absolute 1.5-second service deadline from monitor
entry. Each PCK transition also has the owner's 10-millisecond bound. Pending
client polls wait one scheduler tick; elapsed bounds are observed on resumption.
The returning CPU monitor retains its two-second deadline and uses the accepted
restoration-readback fix from the provider-readiness candidate. The failed
`fd3d5839` image lacked that fix; this does not locate its failure. A failure can return a CPU error report while partial device
ownership remains. Every failed or missing completion retains the claim and
owned RAM for coordinator recovery; ROM visibility alone does not authorize reuse.

## Build and evidence

Run with the existing host Python environment containing pyelftools and Unicorn:

```sh
~/.cache/sunstoneos/venv/bin/python apps/gpu-power-id/build.py --output /absolute/new/build
~/.cache/sunstoneos/venv/bin/python apps/gpu-power-id/check.py \
  --image /absolute/new/build/image.elf --output /absolute/new/client-results.json
```

The build reuses 30 objects from the frozen provider-readiness candidate,
including its accepted CPU monitor. Seven replacements compile the consumer,
GPU platform/native adapter, power controller, resource manager/native adapter,
and current shared TOP owner. The input-object audit checks every power/platform
layout user and its actual dependency file; all three layout users are rebuilt.
The power structure grows by 16 bytes for cached diagnostics. Eleven shared structure sizes and member offsets must match across
old and new objects. The recorded private PMIC callback-member rename does not
change its offset or callback order.

Thirteen focused cases execute the linked client, mapping predicate and identity
loads. They cover both identity formats, unexpected identity, ON/OFF faults,
wrong translation, a read crossing the deadline, backward time, retained token,
counted row, missing provider park, and complete/partial cached CORE refusals.
A seen bit records a completed read, including one whose following clock check
fails; it does not make the observation admissible. Power failures print only
cached CORE values, phase, step, error and last read, then take retained error
return. No new hardware read is made for the report. Power transitions and provider progress
are modeled; these cases do not requalify their physical behavior. Existing
shared-owner deadline and resource evidence is preserved by reference rather
than rerun. The normal client case makes six explicit clock samples; the
legacy case makes twelve, in addition to the shared drivers' checks. These are
observation costs, not performance results.

## Physical admission is separate

The proposal needs current ROM identity, DDR and CPU/GIC continuity, cache-off
entry, and authority over the returning platform's 18 MiB RAM interval
`[0x41000000, 0x42200000)`. It uses no accelerator pool. The coordinator grants
RAM disposition and retains the existing supervised failure policy.

PMIC admission requires the inherited S_TWI0 gate/reset, APBS route, PL0/PL1
configuration, settled supplies and excluded PMU writers. GPU admission adds
stable oscillator/REF/PERI0 ownership, closed local GPU gates, asserted reset,
inactive INT95 and exclusive GPU-local authority covering CCU and CORE6
policy/PWCR at `0x07066000`. TOP5 remains controlled by the shared row. An older
root record granting only TOP5 does not authorize these new CORE writes: the
coordinator must extend the existing GPU-local grant explicitly. No prior GPU boot or command
execution may have occurred in this powered lifetime. These conditions prevent
the identity-only OFF path from retiring an inherited executing device.

The seven parent words and their decoding are specified in
[gpu-resources.md](../../docs/gpu-resources.md). They are RTC/CCU reads and do not
require a GPU engine access. A successful DCDC4 snapshot establishes an observed
800 mV setting only while the coordinator preserves its supply continuity.

After the finite NPU host validates copied/reaped jobs and local stop/quiescence,
it restores the 18 MiB interval and releases the former CPU DRAM allocations.
Powered NPU SRAM is outside that interval. Its domain, rail and SRAM remain
under coordinator authority, with no resumed NPU activity permitted during
this diagnostic. Uncertain NPU completion blocks succession.

This diagnostic starts a fresh ledger; it does not import or prove NPU leases.
It has no domain-4 row and performs no NPU register or SRAM access. Supply
verification reads DCDC2 without changing its setting. Shared clock and gate
operations preserve NPU fields. Provider park retires this image's requests;
it does not release inherited NPU ownership or prove global NPU quiescence.
The coordinator separately authorizes the 18 MiB CPU overwrite interval.

Use the existing retaining queue wrapper and frozen returning-kernel runner
with expected text `COMPLETE GPU power identity: OFF and provider parked`.
Require status zero, normal CPU return and the complete identity lines before
classifying BVNC. A draft proposal or a model pass grants no device access.


The selected BXM source and the architectural PWCR restriction are documented in
[gpu-power.md](../../docs/gpu-power.md). BXM writes CORE without polling it; this
candidate's bounded exact status check is a conservative acceptance condition,
not a promise that hardware will acknowledge. Inherited policy 8/status 0 is
refused before PWCR or local clock writes. No CORE delay values are invented.
The pinned SCP fragment is a host-inspected source clue, not a runtime dependency
or evidence that its boot path executed on either board.

The frozen provider-readiness image `3adfa4ad` remains the first proposed board
test. This later candidate needs a fresh recovered board, current root admission
and explicit CORE authority. It must not reuse operating records from the failed
powered lifetime. Neither candidate has been launched by this offline work.
