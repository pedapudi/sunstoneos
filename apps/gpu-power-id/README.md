# GPU power and identity diagnostic

This returning single-core diagnostic powers GPU_TOP, reads the GPU hardware
identity, powers GPU_TOP off, and parks the shared PMIC service. It is implemented
and checked in a software model. Physical GPU power and identity operation remain
unqualified until the coordinator admits and runs the frozen image.

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
PCK600 delay row and requests ON. The GPU controller then selects the existing
400 MHz parent, opens the local clock/bus gates, waits for settling and releases
external reset. No PLL retuning or voltage programming occurs.

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

OFF asserts external reset, closes the local bus and clock gates, requests
domain-5 OFF, and releases the platform's resource token. Completion requires
the actual OFF phases, no token or delegated lease, an answered OFF row with no
count, a healthy OFF domain with no users, and no retained client resource slot.
After service park, the client checks those conditions again and refuses any
remaining PMIC request or borrow. The DCDC4 stable rail lease remains held until
the image lifetime ends. Shared gates remain enabled for their other users.

The client uses the existing absolute 1.5-second service deadline from monitor
entry. Each PCK transition also has the owner's 10-millisecond bound. Pending
client polls wait one scheduler tick; elapsed bounds are observed on resumption.
The returning CPU monitor retains its existing two-second deadline and return
implementation. A failure can return a CPU error report while partial device
ownership remains. Every failed or missing completion retains the claim and
owned RAM for coordinator recovery; ROM visibility alone does not authorize reuse.

## Build and evidence

Run with the existing host Python environment containing pyelftools and Unicorn:

```sh
~/.cache/sunstoneos/venv/bin/python apps/gpu-power-id/build.py --output /absolute/new/build
~/.cache/sunstoneos/venv/bin/python apps/gpu-power-id/check.py \
  --image /absolute/new/build/image.elf --output /absolute/new/client-results.json
```

The build reuses 31 objects from the physically passing DCDC2 snapshot image.
Six objects supply the new consumer, GPU platform, native adapter, GPU power
controller, `memmove`, and the current shared power owner with its reviewed
deadline fix. Eleven shared structure sizes and member offsets must match across
old and new objects. The recorded private PMIC callback-member rename does not
change its offset or callback order.

Eleven focused cases execute the linked client, mapping predicate and identity
loads. They cover both identity formats, unexpected identity, ON/OFF faults,
wrong translation, a read crossing the deadline, backward time, retained token,
counted row and missing provider park. Power transitions and provider progress
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
inactive INT95 and exclusive GPU_TOP/CCU authority. No prior GPU boot or command
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
