# Returned-RAM GPU capture

The GPU host adapter is for the coordinator operating one reviewed returning single-core image. It prepares immutable launch inputs and captures copied GPU output through the newly returned ROM service. Preparation and modeled checks require no board access. A runnable physical admission is not supplied, and neither the image manifest nor successful software checks grant physical eligibility.

`prepare.py` requires the distinct `gpu-single-core-returning-v1` profile and `a733-gpu-finite-entry-v1` admission. It preserves the existing GPU session, launch request, contract and launcher producers. Admission requires current CPU and GPU DDR reservations, the existing single-core platform facts, initial GPU isolation/power/mapping facts, and an explicit supervisor. The `a733-gpu-finite-return-review-v1` record must bind the exact image, build identity, target instance, physical port, supervisor, launcher hash and all host source hashes. Its evidence must cover architectural reset, software handoff, successful normal-return retirement and failure retention. Old retained-only admission records are rejected.

The fixed returning image seals exactly 592 session bytes. Preparation rejects
larger program and texture envelopes before reading admission evidence or
creating a bundle. The shared renderer accepts those envelopes for other images;
its acceptance does not change the fixed returning image's session layout.

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

Root source review found no new blocker in admission, copied-result validation or full DRAM restoration. All 18 host sources match the preserved preparation bundle. Evidence is recorded in `gpu-returning-host-20261019/root-host-source-review.json`; the 93 accepted host cases and shared transport checks were reused. Target-specific admission and physical GPU operation remain outstanding.

`host-check.py` exercises the exact image/profile layout, report mutations and actual GPU capture callback with modeled transport/files. The file model preserves the full declared lengths without allocating 802 MiB per case. It checks all chunk addresses, the CPU/GPU boundary, final extent and failures midway through backup or restoration. `prepare-check.py` runs the actual launcher and bundle producers using explicitly synthetic software-only evidence, then checks admission, root-review and bundle rejection. Its fixtures name no physical port and provide no physical authority. The accepted 25 native handoff cases and separately reviewed shared transport checks are reused.

## Candidate and bounded physical procedure

Use the corrected two-job image under
`~/.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019/`:

- Image: `build/return.elf`, SHA-256
  `2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d`.
- Manifest: `manifest.json`, build identity
  `34572150354cb042d46a5cc9c5830c93f5c60eaae973a5e449f27033b32de5e2`.
- Reused evidence: `root-review.json`, `changed-path-result.json`,
  `client-result.json` and `host/result.json`. The earlier publication and
  host evidence retain the limits described in [RETURN.md](RETURN.md).

The preparation and capture checks default to this corrected image. The older
publication image has a callback that rejects the second submission; its
publication results remain useful, but it is not the two-job candidate.

The candidate starts at `sd_single_core_entry` on the boot CPU, checks that
the seven secondary CPUs remain OFF, and enters the single-core FreeRTOS SRE
port at Nonsecure EL1. Its tasks run at EL1t. It creates no native USB task.
After two jobs, healthy CLOSE and provider quiescence, the EL1 exit shim
publishes the output before SMC enters the GPU return monitor.
The monitor restores interrupts and requests the reviewed ROM reset.
This path has no dependency on the all-core boot barrier. Single-core kernel
and USB results from another image do not establish this candidate's GPU
startup, computation, stop or return.

Each job uses one workgroup of 32 lanes, with unsigned input words `seed=17`
and `factor=3`. The expected words follow the [fixed shader](../../docs/gpu-compute-job.md).
An independent host calculation gives first word `0x9e3779ad`, last word
`0x9e5679c8`, and SHA-256
`a7d48e5bff0ccbb3d2f7557f5b2ba5db5014a958e2fa11a9599c2f7b8b6bd4ef`
for one 128-byte little-endian result. Both jobs produce the same words,
with completion sequences 1 and 2. Concatenating their 256 result bytes gives
SHA-256 `30f134071ed2090211b1e1a68949480afb56a960768775e30cdbba27a54d02c8`.
The return packet, log and receipt must also pass `report.py` before restoration.

Before a physical attempt, the coordinator must bind the candidate to the
current target and establish the [GPU entry facts](../../docs/gpu-application.md).
These include initial isolation and drain, the complete RAM reservations, seven CPUs OFF,
supported settled DCDC4 power, the 400 MHz parent, shared-provider ownership,
remote-master exclusion and the required mappings. The entry records must
also cover the platform validator's ROM USB ownership conditions.
Current ROM continuity requires its recorded cache-off preparation and fresh
survey. A board's earlier observations cannot supply another powered lifetime's
admission. No physical admission is present in the software fixtures.

The port `3-3` records under
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/` provide reusable
facts when the coordinator establishes continuity from their powered lifetime:

| Record | Fact available for admission |
| --- | --- |
| `ddr-2/session.json` | Successful cold training of 12,288 MiB after the recorded power removal. It does not establish exclusive ownership of the GPU reservations. |
| `set5-bb2-run/session.json` | The current single-core kernel completes and returns. The GPU candidate has its own frozen monitor and memory geometry. |
| `set5-r9-v6-usb/session.json` | Native USB takeover, exchange, quiescence and ROM return work with the changed PMIC provider. The finite GPU candidate creates no USB task. |
| `bb6-run/` and `set5-bb6-run/` | PMIC service and DCDC4 supply verification work in this lifetime. Reuse the configured 800 mV evidence with settling and subsequent writer exclusion. |
| `bb8-run/` | NSI priority changes persist. The session must describe those fields and the observed 400 MHz MBUS clock. |

These records avoid repeating kernel, USB, DDR or PMIC checks merely to prepare
the candidate. They do not yet bind its GPU PLL, initial reset/drain, complete
RAM ownership or current launch instance. Those are the remaining physical
admission facts; full multicore operation and watchdog qualification are not
dependencies of this image.
The [entry capture procedure](ENTRY.md) gives the remaining register addresses,
native predicates, access widths and scratch-restoration steps.

The current host closure adds the session-size guard and changes ten inherited
files, including the shared returning transport and launcher inputs. An old
root-review record cannot authorize the current bytes. Freeze and review the
current 18-file host closure, launcher and target-specific admission together
through the existing `prepare.py` interface. Reuse unchanged component evidence;
check only affected launcher or host behavior identified by that review.

Once those conditions are satisfied, the coordinator can reserve one attempt
on an available, qualified board using the existing queue and per-port lock.
`host.py` saves all 802 MiB, launches once, and waits up to 30 seconds for a new
ROM instance. The native session has a 60-second deadline; the shorter host
window can reject a late return and does not establish native completion.
Individual memory transfers have 120-second limits and other FEL requests
have 15-second limits. These limits do not bound the total snapshot time.
Success requires both numerical results, normal stop, validated return and
all 201 restoration readbacks. The first transport failure or invalid return
retains ownership for supervised recovery and forbids another attempt.
Invoke the runner directly in a persistent process and observe its progress
without a process-killing timeout. Its failure path holds the board lock until
coordinator recovery. A wrapper that kills the runner after 300 seconds would
release that lock despite unresolved device ownership.

Offline evidence for the session-size change is under
`~/.cache/sunstoneos/evidence/gpu-resume-20261024/`: `session-envelope/result.json`
records two larger-envelope refusals and `producer/result.json` records the
actual corrected-image producer and validator. Its newly built launcher has
SHA-256 `7af33010ca810084a5b5dee8f23aa0d16f461f7c34ebcc0bad8f50e779aa18c8`.
That successful producer check models admission; it grants no board authority
and does not execute launcher instructions. Earlier host and native behavioral
checks were reused. No physical test or performance measurement ran.
