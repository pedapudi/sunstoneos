# Cold SD boot supplies the resident OS and recorded recognition inputs

The final path is ROM → pinned boot0 → the existing stage-2 monitor → a
verified resident FreeRTOS OS slot → recorded mimus recognition. The card
supplies the runtime image, session, two models and WAV. Recognition starts
from the admitted providers' readiness; no host RAM upload, trigger or result
acknowledgment is required. USB0 may expose the retained transcript.

The target includes eight inference workers and both CPU clusters at their
speed bin's vendor operating points, with the existing thermal owner and DSU
policy. A two-worker bundle remains a compatibility case. Cold entry, reset policy
and fast SD transfer retain their independent hardware admission requirements. An ordinary-file bundle never establishes physical
rail state, topology, remote-writer exclusion, reset reliability or memory
qualification.

## Existing formats carry the complete inputs

The slot remains version 1 with four extents:

| Extent | Load address | Contents |
| --- | --- | --- |
| Runtime | `0x41000000` | Initialized ELF load segments, with bounded zero gaps; native startup owns its BSS |
| Session | `0x41f00000` | 808-byte recorded OS session, or 904 bytes with the existing CPU grant |
| Models | `0x44000000` | Base model, zero padding to `0x44800000`, then overlay model |
| Recorded input | `0x45000000` | Existing authenticated WAV |

The models span 14,949,200 bytes; the WAV has 114,924 bytes. The producer uses
the existing model hashes and addresses. It validates the entire initialized
runtime span against the linked session/result/contract/BSS reservations, so
flattening cannot overwrite session or retained runtime state. It does not
copy an ELF container into executable memory.

`apps/os/card.py` consumes the actual resident ELF and passing link check,
the actual stage-2 ELF/binary, fixed input files, a cold session template and
its referenced evidence, the card allocation and ordinary backup. It emits
frozen flat inputs, the monitor package, a generation-bound slot plan and
existing backup-first storage sessions. Slot selection and generation remain
owned by `tools/boot-slots.py`; its plan path binds the OS session while
it assigns the generation. The output records every source/artifact identity
and unresolved physical prerequisites. Existing partitions and non-target
slots retain the current preservation rules.

## Every boot derives fresh session and CPU authority

The input template has epoch and generation zero. The slot planner assigns
its generation before hashing the extent; the persisted epoch remains zero. For workload 2, the existing `sid_evidence` field carries
the SHA-256 of the four SID identity words; `sid_authority` names the reviewed
cold policy. This interpretation is confined to the recorded-card profile;
headset and radio profiles retain their existing field meanings. Stage 2
checks the actual SID before any OS entry or CPU authority publication.

After the cold entry/reset prerequisites pass, stage 2 arms the resident
watchdog and stamps its fresh counter epoch into the board session. A
904-byte template also carries the existing CPU grant with epoch zero and
the same slot generation. Stage 2 stamps that grant's epoch in the same
operation, then hashes the complete session and publishes the resident
contract. It rejects a persisted nonzero epoch instead of replaying a RAM
launch grant. Native OS admission continues to require matching epoch,
generation, image identity, owners, grant bounds and complete session digest.

The cold CPU policy is an immutable reviewed record for this board, resident
image, canonical template, operating-point maxima and transition budget per
boot. Canonical template hashing zeroes board epoch/generation at bytes 24..39.
For the 904-byte form it also zeroes grant epoch/generation at bytes 840..855
and the approval digest at bytes 872..903. No owner, image, SID, temperature
trip or operating-point bound is excluded. This avoids circular identities. Its
source evidence identifies the board's SID bin, approved vendor voltages,
CPU/PMIC/clock ownership, thermal policy and cold recovery policy. It does not
reuse approval for a specific supervised RAM epoch. The producer refuses
missing or mismatched policy/evidence. The existing CPU driver selects and
checks the bin's supported points. The existing thermal-owned startup sequence
requests CPU-B, CPU-L and DSU before recognition. Thermal limits and refusal
paths can retain a lower point; the bundle records policy, not measured clocks. Eight-worker
placement and priorities remain those of the linked, inspected adapter. A grant names permitted transitions; runtime
refusal or a thermal ceiling does not establish the requested frequency.

## Resident supervision follows the admitted task selection

Workload 2 is valid for resident recorded builds. The image creates PMIC,
USB0, recognition, thermal and supervision tasks. It creates no USB-host,
Type-C or USB-host-power tasks. The resident health check therefore requires
reports only from those selected tasks; service identifiers and the existing
windows remain unchanged. Modes 0/4 retain their USB-host requirements.

A successful recorded inference retains its transcript and continues service
progress. Missing selected-service progress, a provider failure or a fault
closes admission and stops watchdog renewal. USB enumeration and transcript
consumption do not control recognition startup or watchdog health.

## Preparing the ordinary-file bundle

Build `sunstone-a733-os` and `a733-stage2` with the existing cached toolchain.
The recorded performance profile selects `SUNSTONE_A733_OS=ON`,
`SUNSTONE_A733_OS_RESIDENT=ON`, `A733_A76_ADMITTED=ON`,
`SUNSTONE_A733_OS_CPU_CLOCK=ON` and `SUNSTONE_MIMUS_WORKERS=8`.
Audio, radio, headset, GPU, NPU and the optional performance-command service
are off. The CPU clock owner's existing startup sequence requests the vendor
points; a separate host performance command is unnecessary. The resident
performance-command build remains refused. The thermal diagnostic freezes
its first-recognition window in the existing 602-row, 60-second buffer; it is
bounded diagnostic evidence rather than lifetime telemetry.

`apps/os/card.py --help` lists the required paths. Inputs include the actual
ELF/link record, stage-2 ELF and binary, the fixed models and WAV, pinned
boot0 and packer, cold template/policy, allocation, original full backup,
current ordinary card image and identified-card record. The producer checks
the full original backup against the allocation. It creates a new output
directory and never applies its plans.

The cold policy uses schema `a733-os-card-policy-v1` and scope
`resident-recorded-cold-boot`. It records the runtime and monitor hashes,
canonical session hash, exact linked inference configuration, board identity,
four SID words and SID authority. Its fixed evidence references identify the
session's qualification/recovery, wiring, memory and four thermal records.
The qualification and recovery hashes must agree, as native board admission
requires. A CPU grant also requires matching read-only B1 evidence, the SID
bin's two vendor maxima and a transition budget of 1 to 64 per boot. The
policy's complete SHA-256 is the grant's approval identity. These identities
do not establish physical facts or authorize a card write.

The evidence class distinguishes `board-records` from `model-fixture`.
Model evidence requires `--model-fixture` and remains labeled in the output.
The bundle contains frozen inputs, flat runtime, combined models, WAV,
monitor package, generation-bound slot plan and backup-first storage sessions.
Each storage session still requires its own verified range backup and the
existing storage admission before any physical write. The producer emits
`physical_qualified: false` for either evidence class.

## Focused evidence and remaining qualification

Focused checks consume actual producer outputs: slot selection, exact loaded
bytes and digests, session generation/SID/epoch binding, fresh CPU-grant
publication and native OS admission. Linked recorded startup must reach
recognition readiness without USB-host input. Resident service checks cover
nominal reports, absent unselected tasks, missing selected tasks and failure
retention. Malformed assets, ELF spans, generation, policy, SID and stale
epochs must refuse before publication. Existing inference and storage evidence
is reused when the effective code remains unchanged.

Static budgets are four slot extents, one flat-runtime pass, one bounded
model-copy pass, and no host activity during recognition startup. The task
mask is computed once from the sealed session; each supervision pass adds
one bit test per service. Service progress reporting remains one atomic store. Runtime
cost remains the existing slot hash/copy and input authentication, plus one
16-byte SID digest and constant-size session stamping. Cold boot latency is
unmeasured; the SD owner replaces identification-speed loading through the
existing post-DRAM SD core. Physical boot completion requires the resulting
whole path to run from a preserved, explicitly allocated card.

The linked instruction check starts at the loader's post-load entry function
with the authenticated producer session already in memory. It models SID,
watchdog and counter registers, then passes the published contract/session to
the OS's native admission. This establishes the publication contract, not
physical SD transfer, startup latency or a complete hardware boot. Separate
checks cover the existing SD provider and recorded readiness/automatic trigger.

The native loader still refuses unproven remote writers and CPU/cache
ownership before SD access. No production card reservation or backup has been
inferred from the ordinary-file fixture. Physical completion additionally
requires the reserved card allocation, backup-first installation, successful
autonomous recognition, service timing and normal watchdog reboot. Optional
DRAM restore retains its own temperature and memory qualification requirements;
the default bundle uses the pinned full-training boot0.

The monitor package and boot0 remain separately protected storage ranges.
The bundle does not make their installation atomic with a slot header. A
physical installation must retain recovery for interruption of those writes
and establish monitor/session compatibility before treating the new slot as
a working fallback. Ordinary-file plan checks cannot qualify that recovery.

The checked eight-worker image has a 2,100,336-byte initialized runtime span.
The combined stage-2 monitor has 32,560 initialized bytes, leaving 208 bytes
before the fixed report at `0x47008000`. Its BSS ends at `0x47029fe0` and its
stack at `0x4702dfe0`, inside the existing 4 MiB reservation. The post-load
904-byte entry case executes 37,561 modeled instructions before OS entry;
this is an instruction budget, not a measured boot time.
