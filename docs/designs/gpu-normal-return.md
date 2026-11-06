# Successful return after recorded GPU recognition

Status: implemented and qualified on the board for the returning recognition image with GPU linear-layer recognition. Physical cleanup timing is not separately measured.

Under the returning lifetime, an image built with the GPU linear option (`SUNSTONE_A733_OS_GPU_LINEAR=ON`) adds GPU retirement to the return path. The CPU profile supplies worker acknowledgments, the thermal and PMIC owners, per-core terminals, the copied transcript and host RAM restoration. NPU, performance modes and external input services are excluded. The CPU-only option behaves as without the GPU.

## Retirement sequence

- The recognizer returns from the synchronous transcription call and obtains every parallel worker's stop acknowledgment before GPU retirement (`apps/os/cpu-return/recognition.patch`, `apps/mimus/parallel_runtime.cc`), so no inference task can read a lent GPU output.
- The hook rejects an in-flight job, an incomplete output and a recorded dispatch fault, removes the exact-sum hook, and closes admission. At least one validated GPU job is required.
- The runtime reuses `runtime_close` in `apps/os/gpu.c`, with its one-second absolute CLOSE bound and finite STATUS observations. A thermally parked healthy session needs no second CLOSE.
- `a733_gpu_backend_stopped` checks both firmware acknowledgments, retired command and control queues, offline firmware memory, observed isolation, platform OFF, the released resource token and the masked interrupt. Its proof is required before final retirement.
- GPU memory and its DCDC4 parent lease stay held through acknowledged OFF and isolation. Mimus CLOSE issues no separate common-context cleanup command; full firmware shutdown and isolation establish the memory handoff before host restoration. DCDC4 stays at 960 mV.
- A completed GPU supply sample holds a PMIC request slot and borrow after OFF. Final PARK calls `a733_gpu_platform_supply_end`, whose non-performance branch uses `a733_pmic_consumer_gpu_end`. It authenticates the completed request, parent and immutable sample under the PARK deadline, reaps the request, issues no bus transaction and retains the parent. Pending, failed or mismatched requests retain state. Legacy refresh keeps its acquisition-deadline identity.
- Final PARK runs on the GPU worker after the stopped proof, only for the closed owner and epoch, from STOPPED, under one new one-second deadline. It cannot reopen or dispose backing. Thermal CLOSE keeps its sample so a later OPEN can reuse it. The returning configuration supplies the only callback, which rechecks the native stopped proof before ending the sample. The worker publishes PARKED only after the callback succeeds; failure retains the service, sample and backing.
- After PARKED, the recognizer checks the stopped proof, empty request and completion queues and no client ticket, then suspends the GPU worker. Both tasks run on core 6, and the higher-priority GPU worker has blocked before the recognizer collects its reply. No other task resumes the worker.
- One copied GPU return record binds the session epoch, the validated job count and completed retirement. Only then may `os_cpu_return_begin` publish recognition completion to the supervisor. The PMIC owner then verifies that all request slots and borrows are idle.

## Interrupts and memory

- The monitor saves and restores the fixed THS INTID 71 and GPU INTID 95 fields (GPU peripheral SPI 63 is INTID 95). Both routes start disabled, inactive and clear. Final restoration waits for active state to drain, clears pending state and preserves other fields of the shared bank. The per-core GIC cleanup and erratum workaround apply.
- The GPU write-back pool ends below 0x41c00000 and its uncached pool occupies 0x41c00000 through 0x41e00000, inside the host's backed-up 0x41000000 through 0x42200000 reservation. Preparation binds the linker bounds to that reservation. Final cache publication extends the writable-image clean through the write-back pool; uncached memory needs only the ordering the same terminal supplies. The 16 MiB arena clean includes the GPU output through the monitor's Nonsecure alias. EL0 and EL1 output mappings stay read-only. No device write may remain when the terminals clean or the host restores RAM.
- The host path requires GPU retirement source review beside the exact-image, GPU entry authority and normal-return reviews. It captures and validates the GPU record before restoring any DRAM. The CPU rail follow-up keeps its claim and helper pins; the receipt records GPU OFF with DCDC4 at 960 mV.

## Timing and cost

- The job hot path adds no operations, allocations, copies, queue handoffs, barriers or wake-ups. Retirement adds one hook removal, the CLOSE and STATUS exchanges, one PARK request, one stopped proof, one worker suspension and one copied record. Sample end performs no I2C transfer.
- The recognizer publishes finalization-start before the five-second recognition cutoff, after the inference workers stop. The supervisor extends only that phase to the three-second-remaining boundary. Successful GPU retirement publishes CPU completion before the boundary; the last three seconds are reserved for CPU, thermal and PMIC closure and per-core return. GPU CLOSE and final PARK each have a separate one-second budget.
- Code and read-only data grow by 2,016 bytes plus 456 bytes for the sample and park helpers; mutable storage grows by 123 bytes, including the 64-byte copied record.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Return after full recorded recognition with exact-sum qualification | returned, all GPU jobs retired | [gpu.jsonl](../evidence/gpu.jsonl) `1793560272415337471-7f3b4f28` |
| Return after qualified recognition | 186 completed GPU jobs, reference CRCs and transcript | [gpu.jsonl](../evidence/gpu.jsonl) `1793560622000131271-53954059` |
| Software checks | service, supply, finalizer, terminal and capture cases pass; size growth as above | [gpu-normal-return.jsonl](../evidence/gpu-normal-return.jsonl) `gpu-normal-return-software` |

The software checks use modeled service completion and modeled authority and cannot admit physical execution.

## Build

Follow the [returning OS recipe](../../apps/os/NORMAL-RETURN.md) and add `-DSUNSTONE_A733_OS_GPU_LINEAR=ON` and `-DSUNSTONE_A733_OS_MODEL_DIR=MODEL_DIRECTORY` for the pinned model files. Preparation and capture use `apps/os/cpu-return-host.py`; the GPU configuration adds the local pools and the final-retirement review requirements. The default GPU point stays 1008 MHz at 960 mV.
