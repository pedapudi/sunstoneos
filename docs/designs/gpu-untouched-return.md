# Return a GPU construction refusal without claiming GPU execution

Status: implemented and host-tested; the return outcome has not run on a board.

A refusal before the scheduler starts leaves GPU OPEN disabled and lets recognition run on the CPU. The returning image reports that no GPU execution occurred, so GPU qualification is false for this outcome even when the CPU transcript matches the reference. The constructor in `apps/os/gpu.c` writes a reason (1 to 14) at its failure exit through `start_refused`, after suspending any partially created worker. The reason is immutable and a repeated constructor cannot overwrite it.

## Proof required for the return

Returning evidence requires no GPU start, open, epoch, ticket, pending job or runtime failure, and no placed or completed GPU work:

- An uninitialized platform or backend is zeroed. An initialized platform is NEW, with no resource token, PMIC request, rail lease, power request or counted power-domain row. An initialized backend is BOUND to those local objects, with no runtime clock, identity, error or compute mode.
- The service is COLD, with no request or reply.
- A worker is absent, or marked suspended before scheduler startup and still reported suspended by FreeRTOS (`eTaskGetState`). An indefinitely blocked worker does not satisfy the check.
- Partial software construction may retain DEVICE-tagged RAM. The return does not release that ledger and does not claim a hardware-drain proof. The host restores its saved RAM after CPU cleanup.

An accepted OPEN, or a started worker without the refusal proof, keeps the existing failure handling. A partially activated GPU, pending request, missing acknowledgment or uncertain hardware state never produces a successful cleanup receipt.

## Return record

The record keeps its 64-byte extent. Version 2 encodes zero jobs, zero hardware-completion flags and the refusal reason. It asserts no firmware OFF, isolation, voltage or GPU numerical result. A linked capability marker admits version 2; older bundles keep the strict version 1 successful-retirement decoder. The host reuses the ordinary CPU terminal proof, DRAM restoration and the mandatory rail helper.

## Why the proof holds

`a733_gpu_mimus_start` runs native platform and PMIC bindings, firmware and session memory construction, backend binding and service queue and task creation. These operations construct RAM state, translate or publish memory and read architectural counters. Device power, register writes and DMA require runtime OPEN. Service initialization invokes no interrupt callback, and its worker is suspended before the scheduler if the final binding refuses. The proof does not apply to a constructed worker that has started.

The only task-state query runs during the zero-job refusal return. The GPU return target enables the FreeRTOS getter; other targets keep their configuration. The getter uses task critical sections, whose port implementation supports EL0 callers through SVC, and checks the suspended and event lists under the scheduler lock. Recognition and job hot paths add no operations.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Software-tested image code-size growth | task-state getter 364 bytes, refusal proof 1,828 bytes, constructor 608 bytes | [gpu-untouched-return.jsonl](../evidence/gpu-untouched-return.jsonl) `gpu-untouched-return-staged-image` |

## Open work

Run the constructor-refusal return on a board and record the result.
