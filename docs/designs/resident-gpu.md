# Resident cold-card GPU recognition

Status: implemented software path; physical qualification remains open. Cold GPU startup, watchdog-reset retirement,
cross-PLL behavior and combined thermal operation remain physically unqualified.

The resident recorded OS accepts the existing GPU linear service alongside
eight CPU workers, CPU clock grants, admitted A76 cores and the selected
ShuffleNet NPU application. Performance commands remain disabled. The GPU runs
at the existing vendor 1,008 MHz / 960 mV point; the NPU keeps 852 MHz / 800 mV.
The NPU catalogue application remains separate from Mimus recognition.

## Reuse the existing runtime and card format

The GPU firmware is already linked into the runtime image, authenticated by
`gpu_configuration`, and copied into private firmware backing by the existing
GPU reservation provider. Its staging clock field is replaced with the actual
GPU top-point clock by `a733_gpu_backend_bind` before firmware publication.
No firmware rewrite, new card extent or new loader is required.

The four card extents remain runtime, session, combined Mimus models and WAV.
GPU adds no session fields: sizes remain 808/904 bytes without NPU and
1,080/1,176 bytes with NPU, depending on the CPU grant. The actual linked GPU
firmware, placement hashes, task configuration and read-only output mapping
must match the manifest before card preparation accepts its cold authority.
The existing NPU pointer/hash authentication also remains required.

GPU and NPU already share the board's resource and domain managers through
distinct clients. GPU client 7 and its two fixed owners cannot alias NPU or
CPU ownership. Existing native session and GPU pre-provider checks enforce
those identities. The GPU arena remains `0x46000000..0x46c00000`, its output
view `0x46c00000..0x47000000`, and the separate NPU pool
`0x47400000..0x47c00000`. Device backing continues to use existing allocators.

## Bind cold authority to the actual image

`card.py` accepts GPU only for the recorded resident profile. Its existing
cold policy binds the image, stage2, SID authority, session and worker placement.
A GPU entry in that policy names the actual linked configuration and hashed
condition records. It preserves the supervised entry's conditions for prior
GPU/remote exclusion, TOP5/CORE6 state, local clocks and rail authority, shared
PMIC/register ownership, IRQ95 state and memory alias exclusion.

The supervised manual-power-removal condition cannot authorize a resident boot.
Cold preparation instead requires explicit evidence that watchdog reset retires
GPU firmware and all memory transactions, the next cold entry excludes prior
writers, and uncertain failures retain resources until that qualified reset.
Normal GPU CLOSE, reset register defaults, a silent interval and the previous
boot's GPU-loss marker do not establish these facts. Records bind the board,
image, SID authority and evidence class. Missing or mismatched facts refuse
preparation; model records require `--model-fixture` and remain nondeployable.

For GPU plus NPU, one shared cold clock record must be referenced by both the
GPU conditions and the NPU clock conditions. It covers NPU PLL enable while
GPU is powered, GPU PLL start/stop while NPU remains powered, and continued
CPU/DDR admission. Its image and both operating points must match. This is the
cold counterpart of the supervised simultaneous-lifetime condition; separate
single-device records cannot silently authorize the combination.

## Preserve bounded failures and startup ordering

The existing GPU service retains its OPEN/job/reply deadlines. CPU clocks must
be ready before GPU OPEN. The existing thermal owner closes healthy GPU work
between calls at THROTTLE, reopens at NORMAL and terminates at CRITICAL. An
accepted uncertain GPU failure ends the resident board lifetime and stops
watchdog renewal. An unaccepted GPU refusal preserves the existing CPU fallback;
that fallback is not GPU qualification. NPU completion and late-IRQ checks keep
their existing independent stopped-resource contract.

The previous GPU-loss marker may suppress OPEN for a boot, but it does not
replace cold retirement evidence. The underlying shutdown still requires the
reviewed fresh-idle/drain and firmware-stop proofs before OFF. This integration
adds no reset, OFF shortcut, polling loop, task or per-job authority check.

## Scope, budgets and validation

Owned changes are the recorded resident composition guards in OS CMake/link
checking, GPU cold-policy handling in `card.py`, focused existing card fixtures,
and this component documentation. The CE owner retains stage2 remote admission
and the predecessor contract. Session stamping, loader extents, GPU firmware,
power providers and common SD code need no changes.

Stage2 growth is zero. The accepted CE monitor is reused unchanged: 32,456
bytes with 312 bytes spare. The existing supervised eight-worker GPU/NPU image uses 1,138,848 bytes
of executable code and 194,057 bytes of CODE read-only data. Its write-back pool
ends at `0x41b27000`, leaving 888,832 bytes before the fixed noncacheable pool.
The resident image uses 1,140,512 bytes of executable code and 194,265 bytes
of CODE read-only data, an increase of 1,872 bytes for existing resident paths.
Its BSS grows by 128 bytes, reducing the remaining device pool by 128 bytes;
the pool end is unchanged. The real card producer confirms
the flat runtime fits below the session reservation.
Host preparation adds condition hashing and copies frozen condition evidence. There is no added steady-state native work.

The native resident eight-worker CPU_CLOCK/A76/GPU/ShuffleNet build and four
linked GPU startup/owner cases pass. The real card producer and 34 focused cases
cover cold authority, shared clock records, exact image and board binding,
owner aliases, native session admission and unchanged four-extent geometry.
An initial fixture compile omitted the linked recognition-core definition.
After correcting that fixture, the batch passed 29 checks before its CPU-owner
mutation targeted the grant size field. Correcting that offset and resuming
only the five remaining owner/native cases passed. Both setup failures and the
continuation are preserved; no production behavior was changed for them.
Existing GPU arithmetic, NPU inference, worker parity, provider-sharing and
thermal evidence is reused. No inference or physical workload was replayed.

Evidence is in `~/.cache/sunstoneos/evidence/resident-gpu-cold-1026`. The OS ELF
SHA-256 is `6b3d370b9002e61076fbe56d856843d4e824b5b066edcfe19d7e8330ace7f7fa`.
The accepted CE monitor and its closure are under
`~/.cache/sunstoneos/evidence/cold-ce-lineage-1026`; its ELF SHA-256 is
`7580268b15a847056971cb53224b321a79ee69bbe30e4b638fa25993e05cdf54`.

## Supply the cold GPU records

Use the existing `apps/os/card.py` command and its cold policy. Add a
`gpu` object with exactly `configuration` and `conditions`: configuration is the
actual link manifest's `gpu_profile`; conditions maps every `GPU_CONDITIONS`
name in `card.py` to a `{path, sha256}` reference. With NPU, also include
`cold-concurrent-gpu-npu-cross-pll-qualified`. The existing image, monitor,
canonical-session, SID and CPU-grant bindings remain mandatory.

Ordinary GPU records use schema `a733-os-card-gpu-condition-v1` and name their
`condition`. Every record supplies `reviewed: true`, the policy's `identity`,
`sid_authority`, `image_sha256`, `stage2_sha256` and `evidence_class`, plus a
nonempty `evidence` list of hashed file references. Board records require
`physical_authority: true` and `modeled: false`. Model fixtures require the
opposite values and explicit `--model-fixture`; they cannot authorize deployment.

The reset record additionally requires true `after_uncertain_loss`,
`before_stage2_reload`, `before_reserved_memory_reuse`,
`firmware_and_bus_transactions_retired`, `pll_gpu0_disabled`,
`gpu_gates_closed` and `gpu_bus_reset_asserted`, with `initial_supply_uv`
800000 or 960000. These describe qualified observations, not inferred reset
defaults. Existing startup raises DCDC4 from 800 to 960 mV or accepts an already
960 mV rail; other values refuse. The retirement proof must cover memory writes
before stage2 reloads runtime/models and before the OS clears or reuses backing.

The shared clock record uses schema `a733-os-card-gpu-npu-clock-v1`, scope
`simultaneous-powered-cold-lifetime`, `gpu` fields `core_hz: 1008000000` and
`supply_uv: 960000`, and `npu` fields `mhz: 852` and `supply_uv: 800000`.
It requires true `npu_pll_enable_with_gpu_active`,
`gpu_pll_start_stop_with_npu_active` and `cpu_ddr_clocks_remain_admitted`.
Both GPU and NPU clock records reference the same file bytes by hash. The cold
policy binds those records to the canonical session; embedding that session
hash in the shared record would create a hash cycle through the NPU clock record.
No source or model result supplied here establishes the required physical facts.
