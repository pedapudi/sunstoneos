# Eight inference workers in the supervised OS

Status: implemented with focused software checks; physical execution remains
unqualified. The supervised OS uses the eight-worker engine and adapter with
its existing model, weights, arithmetic, producer scratch and session format.

## Supported composition and placement

The supported OS composition uses supervised recorded recognition and
requires `A733_A76_ADMITTED=ON`. It retains the existing CPU clock and performance
mode options, including their supported NPU composition. Resident operation,
microphone capture, radio, headset and GPU-linear placement remain outside this
change and must reject eight workers. Two-worker configurations remain supported
under their existing rules.

The owner runs on A76 core 6 and the first worker on A76 core 7, both at priority
3. The remaining workers run on A55 cores 0 through 5 at priority 2. The actual
OS PMIC task runs at priority 3 on core 0; USB and thermal services use priority
4, and Type-C/power services use priority 5. The optional NPU client runs at
priority 3 on core 1. Priority 2 keeps A55 inference below these owners while
preserving every service affinity. Equal row partitioning remains unchanged;
no heterogeneous speed ratio is assumed.

`mimus_parallel_init` validates the OS owner/worker pair against
`a733_smp_placement.h`. The selected count remains `SUNSTONE_MIMUS_WORKERS`.
The compile guard admits the supported eight-worker composition and rejects
other OS profiles. Standalone native and QEMU configurations retain their
accepted behavior.

## Linked configuration and host admission

The adapter retains a small constant record in OS images containing its
version, worker count, owner core, complete inference core mask and A55/A76
priorities. Its values come from the same compile-time constants that allocate
slots and create tasks. The OS build identity includes the selected count.
The existing link check reads the record from the ELF, checks the selected
build count and profile, and publishes the configuration in the image manifest.

Host preparation and frozen-bundle verification compare the manifest with
the actual ELF record. An eight-worker image also requires the existing
reviewed entry record to name that exact inference configuration. The existing
entry validation already binds the image, board identity, session and platform
evidence. This adds no session bytes, device operation or separate approval
schema. Existing two-worker artifacts without the new record remain eligible
under the existing rules; removing metadata from a new eight-worker manifest
must fail against the linked record.

## Bounded jobs and failure retention

The accepted adapter keeps each callback context until every submitted slot
acknowledges its matching generation. All eight tasks use the existing OS
recognition checkpoints. A throttle checkpoint sleeps once, rechecks session
liveness and only then records progress. A closed session suspends the caller
through the existing terminal path. If a worker stops before acknowledging,
the owner remains blocked with its context and buffers retained. Neither thermal
failure nor performance-mode failure may fabricate a join or start a later
recognition generation.

The thermal policy and checkpoint implementation already have focused evidence.
Their logic is unchanged. The adapter fixture exercises the
changed placement and service priorities, then closes admission before a worker's
completion and observes that the owner has not returned or reused its context.
Physical service latency, heterogeneous throughput and recovery remain unmeasured.

## Budget and focused validation

Eight workers add six 64 KiB stacks and 5,760 bytes of private queue/task slots
relative to the accepted two-worker adapter. The linked configuration record is
at most 32 bytes. No inference-arena, producer scratch or session allocation
grows. The OS image must retain its existing code/RAM reservations, 4 MiB heap,
model addresses and inference-arena bounds; the optional NPU pool remains
separate. The linked section and stack extents pass the bounds recorded below.

Validation reuses accepted engine arithmetic and eight-worker recognition
parity. New checks are limited to:

- One actual FreeRTOS adapter fixture with owner core 6, worker cores 7 and
  0 through 5, and priority-3 services on A55 cores 0 and 1. It must show
  service progress while inference is outstanding and retain the owner/context
  after a worker stops before acknowledgement. The fixture compiles the actual
  generated OS recognition checkpoint and terminal failure path, with board
  liveness and thermal state supplied by the fixture.
- Selected actual OS links covering two-worker compatibility and the supported
  eight-worker composition, with metadata and static-memory bounds checked.
- Focused host cases for exact linked metadata, altered/missing counts or masks,
  entry-record mismatch and unsupported profiles. Preparation and frozen
  verification must apply the same rule.
- Compile/configuration refusals for eight workers without A76 admission and
  for excluded OS profiles.

No physical session or mimus publication is part of this work.

## Recorded software results

The supported CPU/NPU performance composition linked with eight inference
workers at the local engine revision `967747f10dcf038aadc2961b5990d540f6b76455`.
Its ELF SHA-256 is
`220ac54746dd8cf706c6205c1dc45322e1d5dda4ad14dfa8b0bd22bd476645da`.
The linked record contains version 1, eight workers, owner core 6, core mask
255, owner priority 3, A55 priority 2 and A76 priority 3. Worker stacks occupy
458,752 bytes and private slots occupy 6,720 bytes. The 4 MiB heap ends at
`0x41803640`, below the existing `0x42000000` RAM limit. The NPU reservation
remains `[0x47400000, 0x47c00000)`.

The actual FreeRTOS fixture passed fixed placement, service preemption and
failure retention through the generated OS checkpoint. The host checks passed
31 linked-metadata, profile and fresh/frozen entry cases. Six excluded profiles
were rejected by both their compile and configuration guards; an actual
adapter compilation with two workers retained its established A76 pair and
priority 3. The independently built default-two CPU/NPU performance image
serves as the legacy compatibility input; its accepted build was not repeated.

Evidence is in `~/.cache/sunstoneos/evidence/resume-os-eight-20261026/`.
`workers.json`, `host.json` and `guards.json` contain the focused results.
The existing eight-worker recognition parity and thermal-policy evidence are
reused for unchanged engine and policy behavior. No physical execution is
implied by these results.
