The finite NPU supply observer runs the production PMIC provider on the accepted returning FreeRTOS platform. It reads DCDC2 twice under one stable rail lease, with at least 5,160 microseconds between snapshots. Selector 30 denotes the NPU startup profile of 800 mV. The report records both raw snapshots, voltage decoding, provider stages and elapsed time.

The client reaps both requests, releases its rail lease, checks empty request slots and borrows, released mutexes, inherited pin/controller settings and idle TWI before publishing a complete result. Outcome zero means both readings were enabled, programmed to 800 mV and unchanged. A disabled, different or changing setting produces outcome one after the same cleanup. A provider or cleanup failure exits with nonzero monitor status and cannot admit warm reuse. The provider's platform clock and pin leases retain execution lifetime.

The image performs no regulator-data writes, NPU register access, DMA, PLL programming or domain transition. Two configuration observations establish neither analog voltage nor NPU inference. Existing DCDC1 and RTC physical evidence supports reuse of the returning platform; this DCDC2 operation still needs its own physical observation.

Build and check from the repository root:

```sh
python3 apps/npu-returning/build.py --evidence ~/.cache/sunstoneos/evidence/npu-supply-returning-20261018
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python apps/npu-returning/check.py --evidence ~/.cache/sunstoneos/evidence/npu-supply-returning-20261018
```

The build pins and copies 26 unchanged objects from `pmic-provider-probe-20261018`, their source records and the host launcher. It compiles one new client and records its complete compiler dependency closure, tool hashes, ELF and binary. The six linked cases cover the supported profile, unsupported voltage, changing voltage, a disabled supply, startup refusal and an address NACK. Counter, task/queue and MMIO effects are modeled; provider and ownership callbacks execute.

For one supervised RAM-only physical attempt, use the copied returning host runner with the new binary and its exact hashes. Refresh the current second-board warm launcher survey and bind the established DDR continuity. Require exclusive PMIC/TWI/PL0/PL1 and image/result RAM ownership, the accepted timer/GIC return contract and the reviewed manual power-removal plan. The client deadline is 500 ms within the unchanged two-second monitor window. Success requires a complete report, outcome zero, monitor status zero, restored saved RAM and responsive ROM identity. A complete outcome-one report establishes an unsupported supply configuration, with cleanup assessed separately. Stop communication after the first transport failure and preserve its outcome. No queue claim or hardware operation is performed by build or check.

The complete inference adapter uses `SUNSTONE_NPU_FINITE_RETURN=1` when compiling the existing `apps/npu-convolution/main.c` and `native.c`. It reuses the authenticated boot record, original service, fixed model, two input-copy/submit/completion/output/compare/REAP operations and terminal CLOSE. The original bootstrap polls local shutdown and publishes stopped with release ordering. The client waits with acquire ordering, suspends worker and bootstrap, then requests PMIC parking outside active requests and borrows. It suspends the parked PMIC task and checks empty slots, released mutexes and disabled, clear, inactive NPU interrupt 97. After reporting copied output, it validates quiescence again and enters a task critical section before return. CLOSE failure or an unobserved stop retains the session. A numerical mismatch exits nonzero after successful device shutdown. The USB service is not started in this mode.

The development ELF under `npu-finite-quiescence-20261018` links that adapter using the existing full single-core monitor. It is not a physical candidate. Six client instruction cases cover matched outputs, a numerical mismatch, failed CLOSE, an uncompleted stop, failed task quiescence and lost final quiescence. They model service, bootstrap and quiescence replies. Nineteen native instruction cases in `checks/npu-finite-quiescence.py` exercise the actual task quiescence and PMIC checkpoint, with scheduler and hardware boundaries modeled. The checkpoint fixture constructs the actual power-domain owner over an
already-ready modeled PMIC provider. It runs the owner's idle and step
callbacks and models task-notification waits. The three checkpoint cases
cover immediate parking, completion of an active PMIC request, and refusal
to park with an unreaped slot. `--case` selects these cases without replaying
the sixteen unchanged quiescence cases. The separate FreeRTOS CLOSE and local-stop cases exercise the production lower layers; the complete physical path remains outstanding.

The returning platform must preserve these existing interfaces before enabling that mode:

- At EL3, `a733_smp_contract_valid` checks the contract, copies the session uploaded at `0x41f00000` into `.npu_runtime_retained` and hashes only that copy. The copy must match the contract's digest, build identity, epoch, generation, lease and supervisor. `npu_single_core_seal` checks the copy against the monitor's contract without hashing it again and publishes it with DSB before EL1 adopts write-back memory. A later write to the uploaded page cannot reach startup.
- Retain the copied session and its admission flags, `npu_runtime_contract` and `npu_runtime_secondary_off` across EL1 BSS clearing. `npu_session_copy`, `a733_smp_device_preparation_admitted` and the observer identity still validate the established lifetime before providers initialize.
- Stop scheduling synchronously before restoring application memory. Device-local quiescence parks the PMIC and suspends the worker and bootstrap. The platform still owns timer and scheduler shutdown. Retain the allocation and shared leases until the confirmed return; the failure path must preserve potentially active device memory rather than restore it because a timeout elapsed.

For the frozen development ELF, retained NPU metadata occupies `0x41200010..0x41200380`; BSS occupies `0x41205000..0x41235b30`; the exception stack ends at `0x4123db40`. The pool occupies `0x4123e000..0x4143e000`, with the first 265 pages reserved by the NPU constructor. The conservative writable cleanup interval is `0x41200000..0x4143e000`, or 574 pages. Recalculate these bounds after the returning monitor is linked. Its own state, stack and translation tables need independent lifetime and protection from EL1 writes.

The supply fixture executes the real counter constructor and clock queries.
It models the architectural counter instruction so the PMIC adapter's direct
counter reads and the shared clock use the same time and initialized frequency.
A no-op counter constructor would leave the adapter unable to sample time.
The six supply cases and three updated checkpoint cases pass under
`~/.cache/sunstoneos/build/npu-resume-1024/`, in `supply-returning/` and
`checkpoints-final/`. Earlier failure logs remain beside those directories.
No physical execution or return is established by these checks.
