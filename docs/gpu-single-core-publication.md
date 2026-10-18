# Frozen GPU startup and code publication

The frozen single-core GPU image does not have the demonstrated CPU helper's
Secure-only `DC CVAC` gap. Keep the accepted ELF unchanged. Its startup is
sufficient for the existing cold, exclusively owned upload contract; it is not
a mechanism for accepting an arbitrary warm cache state. The current second-board
candidate still needs root to bind the actual pre-upload state and continuity.
No source inspection can supply that physical authority.

This conclusion applies to ELF SHA-256
`5bd80e10e2bf7f13dfeede7cbe1bb7a915937d3842c2195c5fe09cdbf8d19938`,
profile `gpu-single-core-supervised-v2`. Frozen sources and generated platform
are under `~/.cache/sunstoneos/evidence/gpu-single-core-20261018/`.

## Exact path

| Boundary | Frozen implementation and implication |
| --- | --- |
| Upload | `snapshot/sources/tools/check-a733-smp.py:235` uploads and reads back sealed bytes. This establishes data equality, not cache publication. |
| First SRAM execution | `provider-valid-fixture/bundle/launch.S` checks A32 SVC/primary, MMU, I-cache, D-cache and endian state before programming RVBAR. Its code executes before the GPU entry's maintenance. |
| First A64 execution | `snapshot/sources/apps/storage-read-single-core/start.S` checks EL3/primary and disabled MMU/I/D/endian state, sets stack/vectors, then calls `invalidate_data_cache`, followed by `IC IALLU; DSB SY; ISB`. These instructions cannot retroactively establish their own first fetch. |
| Data-cache sweep | `snapshot/sources/arch/aarch64/cache.S` reads CLIDR.LoC, visits each reported data/unified level, uses CCSIDR geometry including CCIDX, and executes `DC ISW` for every set/way. EL3 is Secure, so the sweep is not restricted to Secure-tagged lines. |
| Monitor admission | `snapshot/sources/apps/storage-read-single-core/monitor.c` copies and seals the session, rechecks secondary CPUs OFF, constructs the table graph and retained snapshots, and sets `SCR_EL3=0x505` for Nonsecure A64 EL1. EL3 remains cache-disabled. |
| EL1 adoption | `snapshot/sources/arch/aarch64/start.S` clears ordinary BSS and calls the runtime. `native/build/gpu-single-core/platform.c:69` adopts the prepared EL1 table root, MAIR/TCR, invalidates EL1 TLBs and enables M/C/I. There is no intervening code upload or self-modification. |

The code which must be visible before its own invalidation is the SRAM launcher
at `0x52000` and the Secure A64 prefix at `0x41000000` through the cache helper,
not merely the later Nonsecure EL1 entry. The required pre-upload cleanup covers
the launcher/request/report/stack SRAM as well as image/contract/table DRAM.
The cache-disabled ROM PIO transfers and their readbacks must occur after that
cleanup, with completed writes before execution and no intervening cached writer.
Adding another maintenance call in EL1 cannot solve a first-fetch problem.

## Cache coverage and dirty data

Arm's cached TrustZone guide, version 1.0, sections 3.4–3.5, distinguishes
Secure and Nonsecure physical tags and states that Secure set/way operations
affect both. It does not imply coverage of an unrelated system cache.
The cached Cortex-A55 TRM, document 100442_0200_00_en, A6.2.1–A6.2.4,
describes disabled-cache behavior and warns that `DC ISW` performs a clean and
invalidate on this core. An old dirty line can therefore overwrite a new FEL
upload when the GPU performs its post-upload sweep. Merely clearing SCTLR bits
or matching the FEL readback does not exclude this failure.

A733 User Manual V1.00, section 5.1.1 and figure 5-1, describes private L1/L2
and DSU shared L3. Datasheet V0.93, section 3.1, specifies shared 1 MiB L3.
The A55 TRM B1.16/B2.24 describes CLIDR.LoC and Ctype3 reporting the implemented
L3. This supports including the documented DSU L3 in the loop; it does not
prove that every possible system component is enumerated. No separately
configured CPU system cache is identified in those A733 descriptions.

The pinned Linux v6.17 boot contract, `Documentation/arch/arm64/booting.rst`,
“Caches, MMUs”, requires image publication to PoC and no stale instruction
entries, and warns that system caches can need by-VA maintenance. This is a
coverage criterion, not evidence that an extra A733 cache exists or that GPU
startup shares the separate multicore helper's failure.

## Second-board evidence and decision

The completed 20-register PMIC probe on port `3-3` returned from `003:093` to
`003:101` and actually executed the newly added EL1 read of register `0x15`.
Its frozen `platform/a733/monitor.S` executes Secure whole-cache `CISW` plus
`IALLU; DSB SY; ISB` before its reset to ROM. Its entry uses the same set/way
and I-cache sequence before entering EL1. This is positive evidence for that
particular primary replacement/transition and for reaching the cache-cleaning
return path. It is not a GPU instruction trace or proof of absence of every
possible unenumerated cache. Evidence and frozen source are under
`~/.cache/sunstoneos/evidence/gpu-supply-probe-20261018/`.

Root can reuse this return as the immediately preceding enumerated-cache cleanup
if it establishes the continuous sole-primary, cache-disabled ROM PIO lifetime
and excludes subsequent cached writers. Under that actual contract, the GPU
upload is already at memory, first fetch occurs with caches disabled, and its
Secure sweep removes clean inherited lines before cached EL1 adoption. There is
no additional mandatory GPU image patch or multicore test demonstrated here.
The older `003:093` survey alone does not establish the current entry state.

If root cannot establish this pre-upload contract, refuse the frozen launch.
The concrete software alternative is a GPU-specific SRAM preparation step
**before** uploading the GPU image: clean/invalidate the complete owned upload
and table ranges, including launcher SRAM, through both Secure and Nonsecure
physical aliases to PoC,
complete instruction invalidation and barriers, restore/read back the prior
controls, then upload and verify while preventing new cached writers. It must
execute outside the GPU DRAM prefix being replaced. Reuse the established
Nonsecure alias technique rather than issuing MMU-off EL3 VA maintenance, which
would target only Secure addresses. Any system cache not respecting architected
by-VA maintenance requires its documented own preparation. A post-upload clean
or an EL1-only hook is not a substitute. This alternative needs coordinated
launcher/host ownership and a focused check/review; it has not been implemented
or physically qualified by this audit.

The source-backed decision is conditional acceptance of the frozen startup,
not physical admission of node `003:101`. No code changed, no accepted check was
replayed, and no new review or board access was performed.
