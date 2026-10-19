# Returned-RAM GPU capture

The GPU host adapter is for the coordinator operating one reviewed returning single-core image. It prepares immutable launch inputs and captures copied GPU output through the newly returned ROM service. Preparation and modeled checks require no board access. A runnable physical admission is not supplied, and neither the image manifest nor successful software checks grant physical eligibility.

`prepare.py` requires the distinct `gpu-single-core-returning-v1` profile and `a733-gpu-finite-entry-v1` admission. It preserves the existing GPU session, launch request, contract and launcher producers. Admission requires current CPU and GPU DDR reservations, the existing single-core platform facts, initial GPU isolation/power/mapping facts, and an explicit supervisor. The `a733-gpu-finite-return-review-v1` record must bind the exact image, build identity, target instance, physical port, supervisor, launcher hash and all host source hashes. Its evidence must cover architectural reset, software handoff, successful normal-return retirement and failure retention. Old retained-only admission records are rejected.

The supervision record must explicitly cover successful CLOSE/stop/quiescence followed by ROM retirement, failure retention until complete power removal, DRAM restoration only after validated normal return, no restoration of old SRAM, manual whole-board power removal and stopping after the first failure. These are input requirements; preparation never invents evidence or fills them with successful defaults. Root must review these lifetime semantics before any execution.

The exact save and restoration envelopes are `0x41000000..0x42200000` and `0x48000000..0x79000000`. The first 18 MiB covers code, writable image data, retained session and receipt, BSS, stacks, uploaded session, protected contract/result, monitor state/log and translation tables. The second 784 MiB preserves the complete authenticated 768 MiB WB and 16 MiB NC geometry. The five allocated GPU parents occupy smaller intervals, but that fact does not establish confinement or permit a smaller snapshot.

`host.py` divides these envelopes into 201 chunks, each at most 4 MiB. The last CPU chunk is 2 MiB; the GPU chunks end at `0x79000000`. Each snapshot is size-checked, synchronized and SHA-256 bound before launch. The shared protected preload helper separately snapshots SRAM and uploads the immutable ELF segments, session, request and launcher with readback. The adapter requires four low protected-pin samples and a disabled watchdog before the sole execution request.

The live lock, transport failure latch, passive new-ROM lookup and failure retention come from `tools/a733-returning-host.py`. The coordinator supplies an existing root-owned claim; the adapter creates no queue authority. The shared helper uses the existing per-port lock and guard, validates live identity before requests and stops all further transport after failure. GPU admission and report interpretation remain local to this application.

After a new A733 ROM instance appears, the adapter reads the protected monitor result, private platform log, retained receipt and copied-output packet. `report.py` requires matched epoch, generation, lease, successful terminal states, sequences 1 and 2, exact publication bounds and bounded counter ordering. Both sets of 32 numerical words must equal the fixed workload's expected arithmetic and agree between log and packet. An incomplete, ambiguous, negative or mismatched report cannot authorize restoration.

Before the first restoration, every saved chunk is rehashed. Each chunk is then loaded from those same saved bytes, checked again, sealed for transfer and compared byte-for-byte with its readback. Success permits ownership release only after all 201 readbacks match. Any intermediate failure retains ownership and prevents success or automatic retry. The returned ROM owns SRAM; the adapter never restores the prior SRAM snapshot.

Use the existing preparation interface with the new application-local producer:

```sh
python3 apps/gpu-finite/prepare.py --image IMAGE --manifest MANIFEST \
  --admission ROOT_REVIEWED_ADMISSION --session-template SESSION_TEMPLATE \
  --output NEW_BUNDLE --clang CLANG --lld LLD
```

The coordinator can invoke `host.py` with `--bundle`, `--fel`, `--evidence`, `--ddr-evidence`, `--platform-survey`, optional repeated `--warm-return` and `--root-owned-claim`. These paths and the fresh target identity must already be covered by admission. No physical invocation is part of the offline implementation.

`host-check.py` exercises the exact image/profile layout, report mutations and actual GPU capture callback with modeled transport/files. The file model preserves the full declared lengths without allocating 802 MiB per case. It checks all chunk addresses, the CPU/GPU boundary, final extent and failures midway through backup or restoration. `prepare-check.py` runs the actual launcher and bundle producers using explicitly synthetic software-only evidence, then checks admission, root-review and bundle rejection. Its fixtures name no physical port and provide no physical authority. The accepted 25 native handoff cases and separately reviewed shared transport checks are reused.
