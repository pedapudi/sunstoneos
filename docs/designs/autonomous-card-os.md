# Cold SD boot supplies the resident OS and recorded recognition inputs

**Status: host-tested** for typed-session framing, runtime flattening, policy
validation and ordinary-file bundle planning (`apps/os/card.py`,
`tools/card-boot.py`, `checks/card-boot.py`, `checks/stage2-session.py`). Physical
installation and autonomous boot remain open. The producer emits plans; the
storage maintenance tool has no connected physical writer in its CLI.

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
| Session | `0x41f00000` | Typed session, including its authenticated input facts, bounded by the 4 KiB reservation |
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

For the first autonomous boot on port 3-3 (coordinator decision 2026-11-13,
for that test only), the cold session takes these values:

- `sid_evidence` (session bytes 320..351) is the SHA-256 of the four SID words
  packed as little-endian 32-bit words in order. The words are the port 3-3
  board's 851464448, 1275087104, 1487077908 and 1082729491, from
  `~/.cache/sunstoneos/board-words.json`.
- `sid_authority` (the little-endian 64-bit word at bytes 312..319) is the
  first 8 bytes of that digest read as a little-endian integer, with bit 0 set,
  so it is never zero.

Stage 2 reads the SID words from `0x03006200`, hashes them the same way and
refuses entry (status 7, detail 4) when the digest differs or `sid_authority`
is zero. A host run of the linked `enter()` on the slot A session shows a
different SID refused before any WDT0 write.

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
generation at bytes 24..39 and CPU-grant epoch and generation at grant offsets
32..47. Every other byte remains authenticated, including the performance word
and the reserved CPU bytes. The native grant contains no approval digest.

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

The policy schema is `sunstone-card-boot-policy-v1` with scope
`resident-cold-boot`. It records the image, monitor, linked manifest and canonical
session hashes, manifest worker count, board identity and target, four SID words
and SID authority. Its fixed evidence references identify the session's
qualification and recovery, wiring, memory and four thermal records; the
qualification and recovery hashes must agree, as native board admission requires.
The manifest worker count excludes the application's owner task; seven workers
and the owner form the eight-task recorded configuration.
A CPU grant also requires the matching read-only CPU clock and supply record
([B1](cpu-a76-dvfs.md)), the SID bin's two vendor
maxima and voltages, and a transition budget of 1 to 64 per boot. The manifest
must declare clock grants. This profile requires both clusters, performance
mode zero and all 28 reserved CPU bytes zero. The policy's SHA-256 stays in the
host bundle. These identities do not establish physical facts or authorize a
card write. The evidence class is `board-records` or
`model-fixture`; model evidence requires `--model-fixture` and stays labeled in
the output. The producer emits `physical_qualified: false` for either class.

## Resident supervision follows the admitted task selection

Workload 2 is valid for resident recorded builds. The image creates PMIC,
recognition, thermal and supervision tasks, and USB0 only when the session
grants `usb-device`. It creates no USB-host, Type-C or
USB-host-power tasks, so the resident health check requires reports only from
those selected tasks; service identifiers and windows are unchanged. Modes 0 and
4 keep their USB-host requirements. A successful recorded inference retains its
transcript and continues service progress. Missing selected-service progress, a
provider failure or a fault closes admission and stops watchdog renewal. USB
enumeration and transcript consumption do not control recognition startup or
watchdog health.

## Preparing the ordinary-file bundle

Build `sunstone-a733-os` and `a733-stage2`. The recorded performance profile
selects `SUNSTONE_A733_OS=ON` and `SUNSTONE_MIMUS_WORKERS=8` (the stage-2
loader's resident contract selects the resident lifetime at run time); audio, radio, headset, GPU and NPU are off. The
session carries the CPU clock grant (`tools/session.py build ... --clock-grant`)
and no performance-mode grant. The CPU clock owner's startup sequence
requests the vendor points, so no host performance command is needed. The thermal diagnostic freezes
its first-recognition window in the 602-row, 60-second buffer; it is bounded
diagnostic evidence rather than lifetime telemetry.

The producer takes an OS ELF that admits the resident lifetime and its passing
link record, stage-2 ELF and binary, at most two packed input assets, pinned boot0
and an optional packer cross-check. It also requires the cold template, policy,
allocation, original full backup, current ordinary card image and identified-card
record. It rejects the stop-on-finish diagnostic marker. Each manifest input
must match its session fact and one asset slice; padding outside those slices
must be zero. The caller combines the two model inputs into one asset.

The producer checks the full original backup against the allocation, binds the
backup and current image hashes to the supplied CID and target, and detects
source changes across planning. Capture provenance remains a physical
prerequisite: a supplied CID does not prove where a file came from. The producer
creates a new output directory and never applies its plans. Each storage session requires
its own verified range backup and the storage admission before any physical
write. The monitor package and boot0 are separately protected storage ranges, and
the bundle does not make their installation atomic with a slot header. A physical
installation retains recovery for interruption of those writes and establishes
monitor and session compatibility before treating the new slot as a fallback.
The initial producer profile grants no devices and accepts only thermal, input
and optional CPU-grant extents. Optional DRAM restore keeps its own temperature and memory qualification
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
| Initialized runtime span of the checked eight-task image | 2,100,600 bytes | [autonomous-card-os.jsonl](../evidence/autonomous-card-os.jsonl), `card-boot-typed-bundle-host` |
| Stage-2 monitor initialized bytes | 31,896, with 872 bytes before the fixed report at `0x47008000` | same |
| Typed recorded session without CPU authority | 1,008 bytes | same |
| Cold boot latency | not measured | `autonomous-card-os-physical-boot` (not run) |

Static budgets are four slot extents, one flat-runtime pass, one bounded
model-copy pass, and no host activity during recognition startup. The task mask
is computed once from the sealed session; each supervision pass adds one bit test
per service, and service progress reporting is one atomic store. Runtime cost is
the slot hash and copy and input authentication, plus one 16-byte SID digest and
constant-size session stamping.

## Open work

1. Provide a reviewed physical writer through the queue, with independent
   recovery, fresh range backups and coordinator-approved storage grants.
2. Designate a card with a compatible layout, reserve its allocation and
   preserve its complete, identified host backup before installation, autonomous
   recognition, service timing and a normal watchdog reboot. The native loader
   still refuses unproven remote writers and CPU and cache ownership before SD
   access; no production card reservation or full backup is recorded.
   The bounded [resident FEL completion test](resident-fel.md) is qualified
   on port 3-3; cold card boot remains unqualified. FEL entry
   admits dynamic OFF; cold card entry retains stage 2's static OFF.
   The authenticated restore points record FAT32 starts at sectors 32,768 and
   2,048; both conflict with the required sectors 0..40,959. The existing
   backups cover only 395 and 589 sectors. These cached records establish no
   current card state or authority to repartition ([allocation audit](../evidence/autonomous-card-os.jsonl),
   `card-boot-recorded-layout-refusal`).
3. Measure cold boot latency. Stage 2 selects four-bit, 50 MHz descriptor-DMA
   reads after identification. Its global 30-second software deadline includes
   both candidate loads during fallback. WDT0 arms after loading, so a
   pre-entry hardware stall has no established watchdog recovery.
4. Provide recovery for interruption of the monitor and boot0 writes.
5. Resolve EL1 watchdog renewal requiring BOOT input low while cold resident
   admission permits it high. This discrepancy remains open for card boot.
6. Update the external application's manifest to declare CPU clock grants for
   the vendor-point cold profile. The pinned recorded manifest declares eight
   execution tasks but no clock grant; its host bundle cannot qualify that
   profile. The ordinary-file fixture explicitly records `cpu_authority: false`.
7. Define the cold-boot capture path. A session without USB0 has no live export.
   Loader reports reside in DRAM, and the next OS entry clears the RTC session
   record. A rebooting card image therefore needs a capture mechanism before
   its next entry erases the evidence.
