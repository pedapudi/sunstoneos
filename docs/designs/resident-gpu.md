# Resident cold-card GPU recognition

Status: **host-tested**. The resident recorded OS profile with the GPU linear service is implemented in the OS link checks and in the mimus checkout's card producer (`sunstoneos/apps/os/card.py`, `sunstoneos/apps/os/prepare.py`). Cold GPU startup, watchdog-reset retirement, cross-PLL behavior and combined thermal operation are **open** and physically unqualified.

The resident recorded OS accepts the GPU linear service alongside eight CPU workers, CPU clock grants, admitted A76 cores and the selected ShuffleNet NPU application. Performance commands are disabled. The GPU runs at the vendor 1,008 MHz and 960 mV point and the NPU at 852 MHz and 800 mV. The NPU catalogue application is separate from Mimus recognition.

## Runtime and card format

The GPU firmware is linked into the runtime image, authenticated by `gpu_configuration` and copied into private firmware backing by the GPU reservation provider; `a733_gpu_backend_bind` replaces its staging clock field with the GPU top-point clock before firmware publication. The card keeps its four extents (runtime, session, combined Mimus models, WAV), and GPU adds no session fields: sizes are 808 and 904 bytes without the NPU and 1,080 and 1,176 bytes with it, depending on the CPU grant. The linked GPU firmware, placement hashes, task configuration and read-only output mapping must match the manifest before card preparation accepts the cold authority, and the NPU pointer and hash authentication stays required.

GPU and NPU share the board's resource and domain managers through distinct clients. GPU client 7 and its two fixed owners cannot alias NPU or CPU ownership, and the native session and GPU pre-provider checks enforce this. The GPU arena is `0x46000000..0x46c00000`, its output view `0x46c00000..0x47000000` and the NPU pool `0x47400000..0x47c00000`.

## Cold authority bound to the image

`card.py` accepts GPU only for the recorded resident profile. Its cold policy binds the image, stage2, SID authority, session and worker placement, and a GPU entry names the linked configuration and hashed condition records. The entry keeps the supervised conditions for prior GPU and remote exclusion, TOP5/CORE6 state, local clocks and rail authority, shared PMIC and register ownership, IRQ95 state and memory alias exclusion. The supervised manual-power-removal condition cannot authorize a resident boot.

Cold preparation requires explicit evidence that a watchdog reset retires GPU firmware and all memory transactions, that the next cold entry excludes prior writers, and that uncertain failures retain resources until that qualified reset. Normal GPU CLOSE, reset register defaults, a silent interval and the previous boot's GPU-loss marker do not establish these facts. Records bind the board, image, SID authority and evidence class; missing or mismatched facts refuse preparation, and model records require `--model-fixture` and are nondeployable.

For GPU plus NPU, one shared cold clock record is referenced by both the GPU conditions and the NPU clock conditions. It covers NPU PLL enable while the GPU is powered, GPU PLL start and stop while the NPU stays powered, and continued CPU and DDR admission, and its image and both operating points must match. Separate single-device records cannot authorize the combination.

## Failures and startup order

The GPU service keeps its OPEN, job and reply deadlines, and CPU clocks must be ready before GPU OPEN. The thermal owner closes healthy GPU work between calls at THROTTLE, reopens at NORMAL and terminates at CRITICAL. An accepted uncertain GPU failure ends the resident board lifetime and stops watchdog renewal. An unaccepted GPU refusal keeps the CPU fallback, which is not GPU qualification. NPU completion and late-IRQ checks keep their stopped-resource contract. The previous GPU-loss marker may suppress OPEN for a boot but does not replace cold retirement evidence, and shutdown still requires the fresh-idle, drain and firmware-stop proofs before OFF. The integration adds no reset, OFF shortcut, polling loop, task or per-job authority check.

## Card records

`apps/os/card.py` takes a `gpu` object with exactly `configuration` (the link manifest's `gpu_profile`) and `conditions`, which maps every `GPU_CONDITIONS` name in `card.py` to a `{path, sha256}` reference; with the NPU it also includes `cold-concurrent-gpu-npu-cross-pll-qualified`. The image, monitor, canonical-session, SID and CPU-grant bindings stay mandatory.

- Ordinary records use schema `a733-os-card-gpu-condition-v1` and name their `condition`. Each supplies `reviewed: true`, the policy's `identity`, `sid_authority`, `image_sha256`, `stage2_sha256` and `evidence_class`, and a nonempty `evidence` list of hashed file references. Board records require `physical_authority: true` and `modeled: false`; model fixtures require the opposite values and `--model-fixture`.
- The reset record also requires true `after_uncertain_loss`, `before_stage2_reload`, `before_reserved_memory_reuse`, `firmware_and_bus_transactions_retired`, `pll_gpu0_disabled`, `gpu_gates_closed` and `gpu_bus_reset_asserted`, with `initial_supply_uv` 800000 or 960000. These describe qualified observations, not inferred reset defaults. Startup raises DCDC4 from 800 to 960 mV or accepts an already 960 mV rail, and the proof covers memory writes before stage2 reloads runtime and models and before the OS clears or reuses backing.
- The shared clock record uses schema `a733-os-card-gpu-npu-clock-v1`, scope `simultaneous-powered-cold-lifetime`, `gpu` fields `core_hz: 1008000000` and `supply_uv: 960000`, and `npu` fields `mhz: 852` and `supply_uv: 800000`. It requires true `npu_pll_enable_with_gpu_active`, `gpu_pll_start_stop_with_npu_active` and `cpu_ddr_clocks_remain_admitted`. The GPU and NPU clock records reference the same file bytes by hash. The cold policy binds them to the canonical session, so embedding the session hash in the shared record would create a hash cycle through the NPU clock record.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Focused card and admission cases; linked startup and owner cases | 34; 4 | `resident-gpu-software` |
| Stage2 growth; accepted CE monitor | 0; 32,456 B with 312 B spare | `resident-gpu-software` |
| Resident image against the supervised GPU/NPU image | code +1,664 B, read-only +208 B, BSS +128 B | `resident-gpu-software` |

Records: [resident-gpu.jsonl](../evidence/resident-gpu.jsonl). No source or model result establishes the physical facts the records require.

## Open work

1. Qualify on a board that a watchdog reset retires GPU firmware and bus transactions, and supply the reset and clock records from real observations.
2. Qualify the combined GPU and NPU cross-PLL behavior and thermal operation.
