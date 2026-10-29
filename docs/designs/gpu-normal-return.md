# Successful return after recorded GPU recognition

Status: implemented and software-tested for the optional staged returning OS image. Integrated GPU
recognition, its cleanup timing and ROM return remain unqualified.

The existing returning CPU profile supplies worker acknowledgments, the thermal
and PMIC owners, per-core terminals, copied transcript and host RAM restoration.
Combining `SUNSTONE_A733_OS_CPU_RETURN=ON` with the existing GPU linear option
adds GPU retirement to that path. NPU, performance modes and external input
services remain excluded. The CPU-only option keeps its existing behavior.

## Reuse acknowledged GPU shutdown before publishing completion

The recognizer returns from the synchronous transcription call and obtains every
parallel worker's stop acknowledgment before GPU retirement
(`apps/os/cpu-return/recognition.patch`, `apps/mimus/parallel_runtime.cc`). Thus
no inference task can still read a lent GPU output. The hook also rejects
an in-flight job, incomplete output or a recorded dispatch fault, removes the
exact-sum hook, and closes admission. At least one validated GPU job is required.

The GPU runtime reuses `runtime_close` in `apps/os/gpu.c`, including its one-second
absolute CLOSE bound and finite STATUS observations. An already thermally parked
healthy session needs no second CLOSE. `a733_gpu_backend_stopped` independently
checks both firmware acknowledgments, retired command/control queues, offline
firmware memory, observed isolation, platform OFF, released resource token and
masked interrupt. Its successful proof is required before final retirement.
GPU memory and its DCDC4 parent lease remain held through acknowledged OFF
and isolation. Mimus CLOSE issues no separate common-context cleanup command
106. Full firmware shutdown and isolation establish the memory handoff before
host restoration; no context allocation is disposed independently. DCDC4 stays
at 960 mV.

A completed GPU supply sample holds a PMIC request slot and borrow after OFF.
Final PARK calls `a733_gpu_platform_supply_end`, whose non-performance branch
uses the dedicated `a733_pmic_consumer_gpu_end` operation. It authenticates the
completed request, parent and immutable sample under the current PARK deadline,
then reaps the request. It issues no bus transaction and retains the parent.
Legacy refresh keeps its original acquisition-deadline identity. Pending,
failed or mismatched requests retain state.
An explicit final PARK request performs sample retirement on the GPU worker
after the stopped proof. Thermal CLOSE retains its existing sample so its
subsequent OPEN can reuse it. PARK cannot reopen or dispose backing; it runs only
for the closed owner and epoch, from STOPPED, under one new one-second deadline.
The returning configuration supplies its sole callback, which rechecks the
native stopped proof before ending the sample. The worker publishes PARKED only
after that callback succeeds. Failure retains the service, sample and backing.

After PARKED, the recognizer checks the stopped proof, empty request/completion queues
and no client ticket, then suspends the GPU worker. Both tasks run on core 6;
the higher-priority GPU worker has blocked before the recognizer can collect its
reply. No other task resumes that worker. One copied GPU return record binds
the session epoch, validated job count and completed retirement. Only then may
`os_cpu_return_begin` publish recognition completion to the existing supervisor.
The PMIC owner subsequently verifies all request slots and borrows are idle.

## Restore the additional interrupt and preserve memory permissions

The monitor saves and restores the fixed THS INTID 71 and GPU INTID 95 fields.
GPU peripheral SPI 63 corresponds to architectural INTID 95. Both routes must
start disabled, inactive and clear; final restoration waits for active state to
drain, clears pending state, and preserves other fields in their shared bank.
The existing per-core GIC cleanup and erratum workaround remain applicable.

The GPU uses the local pool layout: its write-back pool ends below 0x41c00000
and its uncached pool occupies 0x41c00000 through 0x41e00000. Both already lie
inside the host's backed-up 0x41000000 through 0x42200000 image reservation.
Preparation binds their actual linker bounds to that reservation. Final
cache publication extends the existing writable-image clean through the
write-back pool; uncached memory needs ordering, supplied by the same terminal.
The existing 16 MiB arena clean includes the GPU output through the monitor's
Nonsecure alias. EL0 and EL1 output mappings stay read-only; no remap is added.
No device writes may remain when the terminals clean or the host restores RAM.

The dedicated returning host path requires GPU retirement source review
alongside the existing exact image, GPU entry authority and normal-return review.
It captures and validates the GPU record before restoring any DRAM. CPU rail
follow-up retains the claim and its existing helper pins; the receipt records
GPU OFF with DCDC4 still at 960 mV. Source evidence is not physical qualification.

## Budgets and selective checks

The job hot path adds zero operations, allocations, copies, queue handoffs,
barriers or wake-ups. Retirement adds one hook removal, the existing CLOSE/STATUS exchanges and one PARK request, one stopped proof, one worker suspension and one small copied
record. The optional PARK phase calls sample end only after successful OFF.
The sample end performs ledger/result/reap operations and zero I2C transfers.
No recurring polling task or new shutdown protocol is introduced. Before CPU
completion is published, each existing 20 ms supervisor pass adds one acquire
read of the GPU return state. Finalizer entry and completion add bounded state
checks once; GPU submission and completion add no recurring operations.

The recognizer publishes finalization-start before the existing five-second
recognition cutoff, after the inference workers stop. The supervisor extends
only that explicit finalization phase to the three-second-remaining boundary;
ordinary recognition retains its original cutoff. Successful GPU retirement
must publish CPU completion before that boundary. The remaining three seconds are reserved for CPU/thermal/PMIC closure and
per-core return under the original deadline. The original five-second reserve
remains the successful-return budget. Physical cleanup timing is unmeasured.
GPU CLOSE and final PARK each have a separate one-second budget. Native object comparisons record 2,016 added bytes of code and read-only data
for the selected return features, plus 456 bytes for the two new sample/park
helpers. Mutable storage grows by 123 bytes, including the 64-byte copied
record, its four-byte phase, copied configuration pointers and saved interrupt
state. The original budget allowed an 80-byte record, 64 bytes of extra SPI
state and the callback pointer; these allocations fit that budget. These object-level comparisons exclude reused CPU terminal code and linker
collection. Linked checks cover healthy and thermally parked shutdown, pending job and
output refusals, missing stopped proof, failed or expired PARK, worker suspension
ordering, finalization cutoffs, both SPI restorations and read-only output
mapping. The actual PMIC/provider check covers retirement after the old sample
deadline, invalid request/parent, fault, armed and expired refusals, and legacy
refresh's unchanged deadline restriction. It measures zero bus operations.
Host cases reject absent GPU review and missing or mismatched GPU completion
before permitting RAM restoration. Existing CPU worker, thermal and rail-helper
evidence remains applicable to their unchanged mechanisms.

## Software evidence and physical boundary

Evidence is under `~/.cache/sunstoneos/evidence/sol-gpu-return-1028/`:

- `service/`: three two-core FreeRTOS cases with actual service, native firmware
  memory and modeled firmware responses. They cover final PARK, thermal
  CLOSE/reopen followed by PARK, and retained callback failure.
- `supply/`: seven cases with actual PMIC, rail, GPU resource and platform
  providers over modeled registers. No sample-end case performs a bus access.
- `finalizer-final/`: seventeen linked hook, caller and shutdown-timing cases.
  Service replies, stopped proof and scheduler APIs are explicit model boundaries.
- `terminal/` and `terminal-delivery/`: affected interrupt, cache-range,
  mapping and restoration cases. The final successful report uses the exact
  prepared session epoch. Cache effects are modeled; mapping checks execute.
- `preparation-delivery/` and `capture-delivery/`: actual dedicated bundle production
  and ten host checks, including review gating, successful capture/restore and
  refused GPU report variants. The transcript and GPU report come from linked
  native producers with modeled service completion. All authority is labeled
  as modeled and cannot admit physical execution.

The native image selects eight workers, CPU grants, GPU linear recognition and
successful return; its link check finds two interrupts and three start entries.
Its default GPU point remains 1008 MHz at 960 mV. The existing exact-sum hardware
results and relocated read-only cache-permission probe do not establish integrated
recognition, production-address cache visibility, cleanup timing or this return.
A physical attempt needs exact image/session authority and the existing retained
claim through CPU rail restoration. No hardware or queue operation was performed
for these software checks.

Build with the [existing returning OS recipe](../../apps/os/NORMAL-RETURN.md),
adding `-DSUNSTONE_A733_OS_GPU_LINEAR=ON` and
`-DSUNSTONE_A733_OS_MODEL_DIR=MODEL_DIRECTORY` for the pinned model files.
Preparation and capture use `apps/os/cpu-return-host.py`; the linked GPU
configuration adds the exact local pools and final-retirement review requirements.
