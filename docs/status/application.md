# Applications and development execution

**Status: implemented.**

SunstoneOS supplies the FreeRTOS application kit, an OS-only image, the hello-compute control and optional service profiles. Applications own their algorithms and assets in external repositories.

The returning OS image uses one registered stop protocol for every granted
owner. Its supervisor orders application queues, device owners, CPU clock
lowering, thermal stop, PMIC stop and per-core terminals within three seconds.
Device faults during the window retain their resources and record the owner and
code while other cleanup continues. Terminals run only after every owner proves
retirement. [The owner audit](../designs/failure-return-audit.md) lists the
implemented stop proofs, admission limits and retained faults.
The USB0 device grant has its own registration and fault records. Its returning
composition remains refused because active USB0 has no proven native close path.

GPU compute, NPU recognition with an open queue, and failure after a verified
512 MiB storage read return to FEL without watchdog reset on port 3-3
([return evidence](../evidence/failure-return.jsonl)). Those cases qualify healthy
device retirement after application failure. Earlier application failures and
EL0 exceptions also returned without watchdog reset, as the evidence table
records. An NPU failure before its service starts publishes a
never-started report. EL1/EL3 exceptions, SError, unsafe ownership and unfinished
returns retain their watchdog ending. The
[return design](../designs/failure-return.md) defines these boundaries.

**Board-qualified scope:** One eight-worker CPU-only mimus recording matches its
90-frame reference, retires seven secondary cores and restores RAM, boot clocks
and CPU rails. A corrected hello-compute control returns normally with seven
secondaries off. These inputs do not establish general recognition or startup
repeatability. Selected GPU compute, open-queue NPU recognition and verified
read-only storage failures also retire all eight cores without a watchdog reset
([return evidence](../evidence/failure-return.jsonl)). [CPU status](cpu.md)
identifies the CPU result and failed controls.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Application kit, manifest and session ([design](../designs/app-abi.md)) | implemented | 552-byte manifest, 4,096-byte session page | `app-abi-host-layout-checks` in [app-abi.jsonl](../evidence/app-abi.jsonl) |
| OS-only, hello-compute and external-mimus builds | host-tested | 12 OS variants, 84 indexed checks passed | `application-host-gate-selection` in [application.jsonl](../evidence/application.jsonl) |
| Failure return to the boot ROM ([design](../designs/failure-return.md)) | host-tested; selected GPU, NPU and read-only storage failures board-qualified | 13 registered owner identities; three selected failures return without watchdog reset or secondary owner fault | [owner audit](../designs/failure-return-audit.md), [records](../evidence/failure-return.jsonl) |
| Eight-worker CPU-only mimus recording | qualified on the board | 90-frame reference matched, seven secondaries off | [CPU status](cpu.md) |
| GPU compute queue and trusted GPU peer | selected compute failure board-qualified | two completed jobs, one abandoned output, firmware OFF and worker PARK at return | [return record](../evidence/failure-return.jsonl); broader GPU scope in [GPU status](gpu.md) |
| Typed session for an image that grants a device (`tools/session.py`) | host-tested | control, NPU, GPU and credential-free radio-scan profiles have typed builders; USB host facts remain required | `checks/session-build.py`; [runner document](../board-runner.md) |
| Concurrent CPU, GPU and NPU operation | host-tested | none on the board | [NPU status](npu.md) |
| Lifetime from the contract magic in one OS image ([design](../designs/one-os-image.md#lifetime-word-and-sealed-session)) | selected returning paths and supervised stops board-tested; resident startup observed | returning images retire all eight cores to ROM; supervised EL3 and EL1 stops have evidence; resident FEL startup reaches EL1 supervision and watchdog return; successful completion remains unqualified | [return records](../evidence/failure-return.jsonl), the supervised records below, resident record `1794468954726206112-8d88da24` and [resident entry contract](../designs/resident-fel.md) |
| Warm PCM-to-transcription time | open | target 0.05 times the audio duration | `application-recognition-time-target` (not run) |

## Open work

USB and radio failure-injection stages await reviewed USB launch admission
facts. An empty USB port cannot qualify started-xHCI retirement. Audio and headset
capture need physical accessories and return qualification. Storage has an
owner-held consistency record, but failure during a filesystem write remains
unqualified.

1. Run an NPU image that fails before its NPU service starts, to confirm on the board that the never-started stop report keeps the port usable.
2. Run the checks that need external inputs (H.264 capture, SAE); the full gate lists nine remaining checks.
3. Qualify GPU recovery and application recognition.
4. Qualify simultaneous CPU, GPU and NPU operation; the optional builds have host evidence only.
5. Qualify returning accelerator execution; [NPU status](npu.md) lists the finite images that already pass.
6. Add broader application inputs, native audio input, autonomous card boot, fault recovery and concurrent thermal qualification.
7. Qualify successful resident completion over FEL with hello-compute's device-free session. USB0 task creation, readiness waits and supervision follow the manifest-derived `usb-device` grant; hello-compute grants none. USB0 failures retain distinct guard reasons and provider results. USB0-granted FEL sessions still need ROM takeover admission and current USB rail readbacks ([entry contract and open work](../designs/resident-fel.md), [failure audit](../evidence/resident-usb0-stop-audit.md), [physical evidence](../evidence/application.jsonl)).
8. Raise the USB host from 24 device records to the 127 that USB addresses allow. The records live in the USB parent lease, which `A733_USB_MEMORY_PARENT_FOR(devices)` in [include/a733_usb_memory.h](../../include/a733_usb_memory.h) computes from the device and endpoint counts. The linked arena ends at `A733_USB_MEMORY_END` (`0x42a00000`) and holds 24 records. 127 records need a lease of `0x904000` bytes and a 14 MiB arena, `0x42200000` to `0x43000000`. The arena is placed by the platform link scripts, `apps/os/usb-memory-link.patch` and `A733_USB_MEMORY_END`, which the memory-layout work owns. Until it reserves that range, a larger `SUNSTONE_USB_HOST_DEVICES` fails a static assertion that names `A733_USB_MEMORY_ARENA_FOR`.
   Allocating the records from the unified ledger according to the declaration needs two changes outside the device owners. The controller reads and writes the records, so they need non-cacheable memory. The ledger has no `convert_chunk` hook: `umem_boot_hooks` in `apps/os/large.c` installs only the lock and unlock, so `os_owner_memory` refuses `OS_MEMORY_DEVICE`. Adding the hook needs the table builders to remap a converted chunk. Applications also reach the arena through a fixed alias at `0x7a000000`, built by `a733_cached_usb_el1_l3` in `platform/a733/cpu_coherence.S`. Both belong to the table-walk work.
9. Size the USB serial slots (4 per port, `SS_USB_SERIAL_SLOTS` in `drivers/usb/host/serial.c`) from the application's manifest. The slots hold loans of the USB transport's buffers (`acquire` in `drivers/usb/host/serial-native.c`), which are Normal non-cacheable controller buffers in the USB arena, so they move with the device records of item 8. The input event queue uses the manifest's `usb.input_events`: a power of two, defaulting to 1,024 for keyboard or mouse applications. Admission takes its storage from the ledger through `os_usb_service_memory` in `apps/os/usb-service.c`.

## Evidence

Board results are records in `docs/evidence/application.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-application-20261101.md).

<!-- evidence-table:begin -->
9 records in `docs/evidence/application.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `application-host-gate-selection` | 2026-11-01 |  | `` | not-run | passed | os_variants_built 12 variants, core_checks 7 checks, indexed_checks_passed 84 checks, indexed_successes_links_full 74 checks, checks_resumed 9 checks, check_index_entries 790 entries, pure_host_checks 89 checks, remaining_external_input_checks 9 checks |
| `1793579984604886693-7d81d397` | 2026-11-02 | 3-1 | `5bd520a766cf` | failure-return | passed | run_total 170.794 s, secondaries_off 7, rail_return 2.09 s |
| `1793582514152160909-ecc5988b` | 2026-11-02 | 3-1 | `1c4c195b3dce` | failure-return | passed | run_total 171.159 s, secondaries_off 7 |
| `application-recognition-time-target` | 2026-11-02 |  | `` | not-run | inconclusive |  |
| `1793940886685882479-99c7d548` | 2026-11-06 | 3-1 | `ec96397e0cd1` | returned | passed | run_total 32.036 s, launch 14.61 s, npu_trigger_to_irq_p50 2907.9 us, npu_trigger_to_irq_max 2913.1 us, jobs 4, mismatches 0 |
| `1793943016757588735-a94fc5b2` | 2026-11-06 | 3-1 | `6d710e0fbc62` | returned | passed | run_total 35.077 s, launch 17.33 s, npu_trigger_to_irq_p50 2906.6 us, npu_trigger_to_irq_max 2915.5 us, jobs 4, mismatches 0 |
| `1793944293922210812-d6f96434` | 2026-11-06 | 3-1 | `d71fd555fb48` | host-error | passed | run_total 32.203 s, run_total 32.202 s, launch 14.694 s, npu_trigger_to_irq_p50 2906.3 us, npu_trigger_to_irq_max 2914.2 us, jobs 4, mismatches 0 |
| `1793948483319321125-4d7fb7af` | 2026-11-06 | 3-1 | `e44044de5574` | returned | passed | run_total 32.4 s, run_total 32.4 s, launch 14.78 s, npu_trigger_to_irq_p50 2906.5 us, npu_trigger_to_irq_max 2913.5 us, jobs 4, mismatches 0 |
| `1794468954726206112-8d88da24` | 2026-11-12 | 3-3 | `28155e9b4b9d` | watchdog-reset | failed | run_total 24.846 s, launch_stage 9.62776 s, rtc_stage 273, rtc_stop_value 24575, rtc_core 0, board_failure_core 1, wdt_mode_before 65, wdt_mode_after 0 |
<!-- evidence-table:end -->
