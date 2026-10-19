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

The complete inference adapter uses `SUNSTONE_NPU_FINITE_RETURN=1` when compiling the existing `apps/npu-convolution/main.c` and `native.c`. It reuses the authenticated boot record, original service, fixed model, two input-copy/submit/completion/output/compare/REAP operations and terminal CLOSE. The original bootstrap polls local shutdown and publishes stopped with release ordering. The client waits with acquire ordering, reports the first output digest and second copied output, then exits. CLOSE failure or an unobserved stop retains the session. A numerical mismatch exits nonzero after successful device shutdown. The USB service is not started in this mode.

The development ELF under `npu-finite-adapter-20261018` links that adapter using the existing full single-core monitor. It is not a physical candidate. Four instruction cases cover matched outputs, a numerical mismatch, failed CLOSE and an uncompleted stop. They model service and bootstrap replies. The separate FreeRTOS CLOSE and local-stop cases exercise the production lower layers; the complete physical path remains outstanding.

The returning platform must preserve these existing interfaces before enabling that mode:

- At EL3, `a733_smp_contract_valid` authenticates the session uploaded at `0x41f00000`, including its digest, build identity, epoch, generation, lease and deadline. `npu_single_core_seal` copies and revalidates the session into `.npu_runtime_retained` and publishes it with DSB before EL1 adopts write-back memory.
- Retain the copied session, authenticated digest/receipt, `npu_runtime_contract` and `npu_runtime_secondary_off` across EL1 BSS clearing. `npu_session_copy`, `a733_smp_device_preparation_admitted` and the observer identity still validate the established lifetime before providers initialize.
- Stop scheduling synchronously before restoring application memory. A stopped NPU does not imply stopped PMIC or service tasks. Retain the allocation and shared leases until the confirmed return; the failure path must preserve potentially active device memory rather than restore it because a timeout elapsed.

For the frozen development ELF, retained NPU metadata occupies `0x41200010..0x41200380`; BSS occupies `0x41205000..0x41235b30`; the exception stack ends at `0x4123db30`. The pool occupies `0x4123e000..0x4143e000`, with the first 265 pages reserved by the NPU constructor. The conservative writable cleanup interval is `0x41200000..0x4143e000`, or 574 pages. Recalculate these bounds after the returning monitor is linked. Its own state, stack and translation tables need independent lifetime and protection from EL1 writes.
