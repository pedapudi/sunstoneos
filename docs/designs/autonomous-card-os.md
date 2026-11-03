# Cold SD boot supplies the resident OS and recorded recognition inputs

**Status: host-tested** for the session framing, the flat-runtime check and the
canonical session hash (`apps/os/card.py`, `boot/a733/stage2/`,
`checks/stage2-session.py`). **Open** for the card producer that binds the cold
policy and evidence to a bundle (`apps/os/card.py` holds only the offline
checks; the policy schema, evidence classes and bundle emission below are the
design), and for physical boot from an allocated card.

The path is ROM, pinned boot0, the stage-2 monitor, a verified resident
FreeRTOS OS slot, then recorded mimus recognition. The card supplies the runtime
image, session, two models and WAV. Recognition starts from the admitted
providers' readiness; no host RAM upload, trigger or result acknowledgment is
required. USB0 may expose the retained transcript.

The target includes eight inference workers and both CPU clusters at their speed
bin's vendor operating points, with the thermal owner and DSU policy. A
two-worker bundle remains a compatibility case. Cold entry, reset policy and fast
SD transfer keep their independent hardware admission requirements. An
ordinary-file bundle never establishes physical rail state, topology,
remote-writer exclusion, reset reliability or memory qualification.

## The slot carries the complete inputs

The slot is version 1 with four extents ([boot-slots.md](../boot-slots.md)):

| Extent | Load address | Contents |
| --- | --- | --- |
| Runtime | `0x41000000` | Initialized ELF load segments, with bounded zero gaps; native startup owns its BSS |
| Session | `0x41f00000` | 808-byte recorded OS session, or 904 bytes with the CPU grant |
| Models | `0x44000000` | Base model, zero padding to `0x44800000`, then overlay model |
| Recorded input | `0x45000000` | Authenticated WAV |

The models span 14,949,200 bytes; the WAV has 114,924 bytes. `flat_runtime()`
in `apps/os/card.py` validates the initialized runtime span against the linked
session, result, contract, monitor-BSS, boot-stack and table reservations, so
flattening cannot overwrite session or retained runtime state. It does not copy
an ELF container into executable memory.

The card producer consumes the resident ELF and its passing link check, the
stage-2 ELF and binary, the fixed input files, a cold session template with its
referenced evidence, the card allocation and the ordinary backup. It emits
frozen flat inputs, the monitor package, a generation-bound slot plan and
backup-first storage sessions. Slot selection and generation stay with
`tools/boot-slots.py`. The output records every source and artifact identity and
the unresolved physical prerequisites. Existing partitions and non-target slots
keep the preservation rules.

## Every boot derives a fresh session and CPU authority

The input template has epoch and generation zero (`template_check`). The slot
planner assigns its generation before hashing the extent; the persisted epoch
stays zero. For the recorded-card profile, `sid_evidence` carries the SHA-256 of
the four SID identity words and `sid_authority` names the reviewed cold policy;
headset and radio profiles keep their field meanings. Stage 2 checks the SID
before any OS entry or CPU authority publication.

Stage 2 validates typed-session framing and rejects persisted nonzero board or
CPU-grant epochs before clearing retained results or arming WDT0. An active
grant names the selected slot generation; an inactive grant keeps generation
zero. After the cold entry and reset prerequisites pass, the loader arms the
resident watchdog, stamps the board epoch and hashes the complete session into
the resident contract. OS admission stamps its private CPU-grant copy with that
epoch and validates service policy; the persisted grant stays unchanged. Native
admission requires matching generation, image identity, owners, grant bounds and
the complete session digest.

Canonical template hashing (`canonical_session`) zeroes board epoch and
generation at bytes 24..39. For the 904-byte form it also zeroes grant epoch and
generation at bytes 840..855 and the approval digest at bytes 872..903. No owner,
image, SID, temperature trip or operating-point bound is excluded, which avoids
circular identities.

The cold CPU policy is an immutable reviewed record for the board, resident
image, canonical template, operating-point maxima and a transition budget per
boot. Its evidence identifies the board's SID bin, approved vendor voltages,
CPU, PMIC and clock ownership, thermal policy and cold recovery policy. It does
not reuse approval for a specific supervised RAM epoch. The producer refuses
missing or mismatched policy and evidence. The CPU driver selects and checks the
bin's supported points, and the thermal-owned startup sequence requests CPU-B,
CPU-L and DSU before recognition. Thermal limits and refusal paths can retain a
lower point, so the bundle records policy, not measured clocks. A grant names
permitted transitions; runtime refusal or a thermal ceiling does not establish
the requested frequency. Eight-worker placement and priorities are those of the
linked, inspected adapter.

The policy schema is `a733-os-card-policy-v1` with scope
`resident-recorded-cold-boot`. It records the runtime and monitor hashes,
canonical session hash, linked inference configuration, board identity, four SID
words and SID authority. Its fixed evidence references identify the session's
qualification and recovery, wiring, memory and four thermal records; the
qualification and recovery hashes must agree, as native board admission requires.
A CPU grant also requires matching read-only B1 evidence, the SID bin's two vendor
maxima and a transition budget of 1 to 64 per boot. The policy's SHA-256 is the
grant's approval identity. These identities do not establish physical facts or
authorize a card write. The evidence class is `board-records` or
`model-fixture`; model evidence requires `--model-fixture` and stays labeled in
the output. The producer emits `physical_qualified: false` for either class.

## Resident supervision follows the admitted task selection

Workload 2 is valid for resident recorded builds. The image creates PMIC, USB0,
recognition, thermal and supervision tasks and no USB-host, Type-C or
USB-host-power tasks, so the resident health check requires reports only from
those selected tasks; service identifiers and windows are unchanged. Modes 0 and
4 keep their USB-host requirements. A successful recorded inference retains its
transcript and continues service progress. Missing selected-service progress, a
provider failure or a fault closes admission and stops watchdog renewal. USB
enumeration and transcript consumption do not control recognition startup or
watchdog health.

## Preparing the ordinary-file bundle

Build `sunstone-a733-os` and `a733-stage2`. The recorded performance profile
selects `SUNSTONE_A733_OS=ON`, `SUNSTONE_A733_OS_RESIDENT=ON`,
`A733_A76_ADMITTED=ON`, `SUNSTONE_A733_OS_CPU_CLOCK=ON` and
`SUNSTONE_MIMUS_WORKERS=8`; audio, radio, headset, GPU, NPU and the
performance-command service are off. The CPU clock owner's startup sequence
requests the vendor points, so no host performance command is needed, and the
resident performance-command build is refused. The thermal diagnostic freezes
its first-recognition window in the 602-row, 60-second buffer; it is bounded
diagnostic evidence rather than lifetime telemetry.

The producer takes the resident ELF and link record, stage-2 ELF and binary, the
fixed models and WAV, pinned boot0 and packer, cold template and policy,
allocation, original full backup, current ordinary card image and identified-card
record. It checks the full original backup against the allocation, creates a new
output directory and never applies its plans. Each storage session still requires
its own verified range backup and the storage admission before any physical
write. The monitor package and boot0 are separately protected storage ranges, and
the bundle does not make their installation atomic with a slot header. A physical
installation retains recovery for interruption of those writes and establishes
monitor and session compatibility before treating the new slot as a fallback.
Optional DRAM restore keeps its own temperature and memory qualification
requirements; the default bundle uses the pinned full-training boot0.

## Validation

`checks/stage2-session.py` runs the loader's post-load entry function with the
authenticated producer session in memory. It models SID, watchdog and counter
registers, passes the published contract and session to the OS's native
admission, and covers bounded framing, stale epochs and refusal before watchdog
writes. It establishes the publication contract, not physical SD transfer,
startup latency or a complete boot. Separate checks cover the SD provider and the
recorded readiness and automatic trigger. Linked recorded startup must reach
recognition readiness without USB-host input; resident service checks cover
nominal reports, absent unselected tasks, missing selected tasks and failure
retention. Malformed assets, ELF spans, generation, policy, SID and stale epochs
refuse before publication.

| Item | Figure | Record |
| --- | --- | --- |
| Initialized runtime span of the checked eight-worker image | 2,100,336 bytes | [autonomous-card-os.jsonl](../evidence/autonomous-card-os.jsonl), `autonomous-card-os-static-budgets` |
| Stage-2 monitor initialized bytes | 32,560, with 208 bytes before the fixed report at `0x47008000` | same |
| Modeled instructions before OS entry (904-byte form) | 37,561 | same |
| Cold boot latency | not measured | `autonomous-card-os-physical-boot` (not run) |

Static budgets are four slot extents, one flat-runtime pass, one bounded
model-copy pass, and no host activity during recognition startup. The task mask
is computed once from the sealed session; each supervision pass adds one bit test
per service, and service progress reporting is one atomic store. Runtime cost is
the slot hash and copy and input authentication, plus one 16-byte SID digest and
constant-size session stamping.

## Open work

1. Implement the card producer: policy and evidence validation, frozen flat
   inputs, monitor package, generation-bound slot plan and backup-first storage
   sessions, emitting `physical_qualified: false`.
2. Reserve a card allocation and run backup-first installation, autonomous
   recognition, service timing and a normal watchdog reboot. The native loader
   still refuses unproven remote writers and CPU and cache ownership before SD
   access; no production card reservation or backup exists.
3. Replace identification-speed loading through the SD owner's post-DRAM SD core
   and measure cold boot latency.
4. Provide recovery for interruption of the monitor and boot0 writes.
