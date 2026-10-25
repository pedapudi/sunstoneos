# Working rules

The accepted objective is [docs/goal.md](docs/goal.md). Read the current
[handoff](docs/handoff.md) before continuing. The
[hardware matrix](docs/hardware.md) defines the support inventory independently
of application requirements. Project issues hold work scope and dependencies.

## Task-specific hardware skills

Read the relevant repository skill when taking an assignment. Give subagents its
path together with their file ownership and accepted evidence; they do not need
every hardware document or the full conversation. Resolve links from the skill's
directory. If automatic skill discovery is unavailable, open `SKILL.md` directly.
Each assignment should also identify its worktree, owned files, frozen inputs,
remaining evidence gap and expected deliverable. Workers open the skill from that
worktree; the default checkout may have older guidance. Keep live USB instances
and process ownership in the handoff and queue, where they can be refreshed.

| Assignment | Skill |
| --- | --- |
| Physical access, boot preparation, queued board tests or recovery | [A733 board operations](.agents/skills/sunstone-a733-board/SKILL.md) |
| Secondary CPU startup, GIC, coherence or SMP diagnosis | [A733 multicore bring-up](.agents/skills/sunstone-a733-multicore/SKILL.md) |
| Native peripheral integration, source adaptation, launch/capture or focused validation | [A733 device integration](.agents/skills/sunstone-a733-device/SKILL.md) |

These skills describe reusable decisions and known failure mechanisms. Current
state and completion claims remain in the handoff, hardware matrix and component
evidence. Update a skill when a verified finding changes a reusable instruction;
do not copy each run's status into it.

## Implementation

- FreeRTOS is the runtime OS. Linux is a source and development reference only.
- Reuse suitable open-source drivers and libraries, including upstream Linux
  drivers, by adapting OS interfaces to FreeRTOS. Native execution does not
  require an independent rewrite. Replace source-available components only for
  a demonstrated incompatibility or requirement the existing code cannot meet.
- Prioritize documentation of third-party software usage, provenance, execution
  location, source availability and license status. Keep dependency issues current.
  Defer replacement of opaque firmware and development tools until complete
  hardware functionality and performance tuning; existing dependencies may serve
  both stages. This does not defer driver integration needed for FreeRTOS operation.
- Default every hardware block to the highest operating point the vendor uses
  for the chip's speed bin, with the vendor's voltage. Lower clocks are
  optional modes (power saving, thermal limits), never the default or a cap.
- Complete onboard hardware support and local mimus execution are separate
  completion requirements. A CPU fallback does not satisfy accelerator support.
- Work on isolated branches or worktrees. Preserve other workers' changes.
- Keep downloaded sources, models, firmware blobs, builds, raw logs and full
  disassembly listings outside tracked source. Pin each one and record where it
  can be obtained (URL and revision), its SHA-256 and its license in
  [the vendor firmware sources](docs/vendor-firmware/README.md) or the
  component's own sources record, so that another developer can fetch the same
  bytes.
- Facts derived from vendor binaries may be tracked: function, register-access,
  message-ID and table inventories, with the binary's SHA-256 and offsets. Put
  them under `docs/vendor-firmware/data/` and describe behaviour in your own
  words. Do not commit vendor code, verbatim disassembly or extracted log text
  beyond short identifiers.
- Derive a hardware sequence from the references before running it on a board:
  vendor source, the vendor binaries, upstream Linux and TF-A, and the manuals.
  Diff SunstoneOS against that sequence, including checks the reference does not
  perform. A board run tests a prediction from that diff; it does not search for
  the cause one guess at a time.
- Preserve third-party license notices. Never add generated-by notices, vendor
  attribution tags, or assistant co-author tags to project files or Git metadata.
- Use the writing skill for all communication and documentation.

## Scope and simplicity

- Full board hardware coverage remains required independently of mimus. Distinguish
  usable board functions from unconnected SoC interfaces and optional application
  features. Record the physical evidence for that distinction.
- Prefer a complete native device path over additional isolated components when
  its dependencies are available. Keep missing accessories and recovery conditions
  explicit; software models do not establish physical operation.
- Add an abstraction only for a concrete current consumer, a shared-resource
  conflict, or an identified correctness requirement. Explain what fails without
  it. Prefer fixed board configuration and existing providers where sufficient.
- Keep DMA ownership, cache coherence, shared power/clock coordination, bounded
  failure handling and storage preservation. Consolidate duplicate mechanisms;
  do not remove these requirements merely to reduce code size.
- Keep evidence and status updates proportional to the change. Reuse the existing
  snapshot tools and canonical records; do not add parallel reporting systems.
- Implement useful operational modes incrementally while retaining all required
  modes in the hardware inventory. Defer optional frameworks and native replacements
  for adequate development tools until their stated need.
- Design for the hardware's performance as part of implementation. Follow
  [docs/performance-architecture.md](docs/performance-architecture.md): write a
  short design with measurable budgets, have it reviewed for over-engineering,
  hardware use and correctness before coding, and measure before and after.
  An unexplained regression blocks the merge. Prefer interrupts to tick polling,
  single-copy data paths, the weakest correct barrier and verification once at
  admission. Do not add protocol features that no board use needs merely for
  specification completeness; mark any that exist as optional.

## Validation

Apply [docs/validation.md](docs/validation.md) in both this project and the mimus
port. The user's selective validation policy replaces unconditional full-suite
reruns; substantive correctness requirements still apply. A commit, clean rebase,
handoff, or final response does not invalidate successful test evidence.

Inspect effective feature flags before choosing a build configuration. Select
the intended application and cover the changed branches it uses; a headset-only
build does not cover changed transcription startup. This prevents an avoidable
second build while preserving checks for the actual requested application.

## Efficient tool use and coordination

- Use [the reference index](docs/reference-index.md) to find recurring guidance
  and cached primary sources. Read required instructions once; reuse that context
  while their content is unchanged. For subsequent lookups, retrieve the relevant
  heading or symbol and a bounded excerpt, not the whole document.
- After context recovery, carry forward the last-read revision, source pins,
  conclusions and unresolved questions. Inspect relevant Git changes before
  rereading. Missing context or a changed requirement warrants reading the affected
  section. Never substitute a cached conclusion for live board/queue state.
- Search cached manual text before opening a PDF or browsing again. Preserve the
  source URL, document revision, section and hash when caching a new reference.
  Reopen the original for figures, ambiguous extraction, missing details or a
  changed source. Do not repeatedly retrieve unchanged external documentation.
- Give workers the relevant skill, document sections and evidence pointers.
  Avoid entire handoffs, manuals or source inventories unless their scope requires
  them. Do not reread unchanged writing guidance for each message.
- Discover unfamiliar paths with one scoped `rg --files` search, then reuse the
  discovered paths. Do not add existence checks before every known-file read.
- Read relevant symbols or sections. Summarize large manifests and command
  results; do not print entire inventories or repeatedly request truncated output.
  For JSON manifests, print named fields or fixed-size samples; never print a
  complete path-to-hash map.
- After a failed command, identify its cause before retrying. Preserve completed
  steps and resume only the failed or affected steps. Stop repeated downloads
  after a stable access failure and use an available authoritative source.
- Preserve configured executable paths. Resolving symlinks for hashing must not
  change invocations: `ld.lld` needs its name, and Python needs its virtualenv path.
  Reuse the generator and toolchain from a successful native configuration;
  do not select an unverified build tool for the next device target.
  Supply the compiler and all dependency paths together on the first CMake
  configuration. Changing a compiler in an existing cache can clear those paths
  during automatic reconfiguration. Preserve the failed cache and use a fresh
  build directory; diagnose missing variables before changing source patches.
- Set `PYTHONDONTWRITEBYTECODE=1` when running Python from preserved source
  snapshots. Incidental `.pyc` files are not source inputs; compare the declared
  sources and compiled dependencies instead of generated cache files.
- After context recovery, read the objective and inspect current changes. Reuse
  unchanged source summaries and evidence instead of rediscovering the workstream.
- Multicore work may be delegated like any other integration; the coordinator
  runs its board tests.
- For each integration, keep one implementation owner and one reviewer. Review concrete
  ownership, concurrency and device behavior before execution; report only new
  findings. Integrate completed work before adding another workstream.
- A message to a running reviewer updates its current task; a reviewer that has
  finished needs a new follow-up request.
- Give implementation and debugging to an implementation agent, and focused
  correctness review to a separate reviewer agent. Use lower reasoning effort
  only for mechanical edits and evidence reconciliation. Give each agent concise
  task-specific context instead of the full thread. Change active workers at a
  safe handoff boundary, preserving edits, live processes and successful evidence.
  Reserve review for substantive correctness risks; routine edits stay with their owner.
- Keep handoffs concise: changed behavior, exact inputs, result, remaining gap.
  Use existing evidence tools; do not add a reporting system for bookkeeping.

## Development and board-test pipeline

- Assign complete device paths: native image, host launch and capture, focused
  checks, and a bounded physical test request. Agree on their shared interfaces
  early so host integration can proceed alongside native implementation.
- Integrate completed assignments promptly. Choose the number of active device
  paths from independent file ownership, available implementation/review capacity
  and coordinator integration capacity; do not impose a fixed three-path limit.
  Add a complete useful outcome when it can progress without delaying integration
  or physical testing. Physical board serialization does not limit software work.
  Reduce concurrency when shared edits, duplicate work or review queues impede
  progress; do not split tasks merely to increase the worker count.
- The coordinator integrates completed paths and prepares the next eligible
  board test while workers continue implementation. Prioritize tests that unblock
  shared startup or several devices, then ready paths with available peripherals.
- Only the coordinator pushes. It pushes `bringup` to `origin` after an
  integration passes its builds and checks, and first scans the unpublished
  changes for keys, tokens, credential files and session scratch paths.
  Workers commit to their own branches and do not push.
- Freeze each submitted image and its required inputs in the existing evidence
  directory. Workers may continue editing and building in separate directories
  while that image waits or runs. Physical results apply to the frozen inputs.
- Waiting for the board does not block software work. Pause only work whose next
  decision depends on the physical result; continue independent integration gaps.
- Review ownership and hardware risks early. Check fixture compatibility before
  a long batch, and resume only affected steps after a fixture repair. Review
  changed behavior and unresolved findings without repeating accepted reviews.
- Build valid fixture records from the actual producers and preserved artifact
  identities. Do not leave placeholder hashes or build IDs in nominal cases.
  Resolve native hooks by their defining compilation unit when local symbol
  names repeat; reject ambiguous matches during setup, before runtime cases.
  Validate the complete nominal provider configuration with the actual driver
  before linking. Generic board fixtures may not satisfy a device's clock,
  power or endpoint requirements; derive those fields from its accepted fixtures.
- Use the existing queue, evidence records and dashboard. Group physical cases
  only when their resource ownership and recovery conditions permit safe reuse.
  A retained or failed session still requires its prescribed recovery.

## Hardware access

- The user confirms that neither current A733 board has populated eMMC or UFS.
  Do not probe or schedule media qualification for these absent devices. Preserve
  the existing contents of both microSD cards, apart from the writes that the
  storage rule below permits. Existing optional-storage drivers
  and historical evidence remain available for a future populated board.
- Parallel workers develop in isolated worktrees and use separate build/evidence
  directories. Assign file ownership before overlapping work begins.
- One coordinator owns physical board testing. Workers use host checks/emulation
  and submit bounded board-test requests with artifact identities, prerequisites,
  expected results, and evidence destinations. A queue claim does not replace
  the exclusive board lock or establish recovery qualification.
- Integrate changes and reuse valid component evidence before scheduling physical
  checks. Do not retry failed board tests automatically or release a live test's
  ownership solely because an observation timeout elapsed.
- Identify the target before issuing device commands. Do not open other boards.
- Hold an exclusive target lock during device operations. Coordinate access with
  existing shared-board tooling when a controller or acoustic resource is shared.
- A733 tools use `tools/usb-access.py:board_lock`, keyed by stable USB port as
  `/tmp/sunstoneos-a733-<USB-port>.lock`. Do not acquire a project-wide mimus
  lock for this board. Unrelated boards have independent ownership.
- Start with identity queries and RAM-only execution. Write paths may be
  implemented and host-checked at any time; images refuse writes unless their
  session grants them. Physical writes depend on the target:
  - boot area (boot0, TOC1 and the stage-2 slots): only through the backup-first
    update path with slot fallback, after a host backup of the card;
  - a microSD data partition: once a host backup of that card exists and the
    image confines its writes to that partition;
  - an NVMe device: only on a device the user designates as scratch storage.
- Stop device communication at the first transport failure or disappearance.
  Do not loop resets, downloads, or reconnections around an unresolved failure.
- Request a reconnect at the top of [docs/handoff.md](docs/handoff.md), naming
  the port and the physical action, and continue with the other board or
  offline work while it is pending. Order the tests of each connection window
  from the lowest recovery risk, so the test most likely to lose the board runs
  last.
- Before intentional USB re-enumeration, require a reconnect access rule for the
  target port and user. Wait for udev permissions before the next protocol request.
  Do not reset again to work around missing host permissions.
- Keep the failed UFS probe disabled until its failure and recovery path are
  resolved. Do not make watchdog qualification or USB power automation a blanket
  prerequisite for unrelated development. Supervised RAM-only hardware tests may
  use demonstrated software return and manual USB reconnection, with a reviewed
  recovery plan, storage preservation and one bounded attempt. Stop after the
  first transport failure and wait for explicit reconnection; never retry it.
- Defer intentional watchdog expiry and unattended peripheral fault tests until
  their reset destination and recovery are qualified. Existing image admission
  checks still apply; do not fabricate qualification flags. A cached USB ROM
  descriptor does not prove ROM execution. Respect the failed-instance record
  in `tools/usb-access.py`.
- Do not transmit keyboard events into the user's desktop during tests.
- Document missing peripherals as qualification blockers and continue independent
  work. Do not count source inspection or emulation as physical qualification.

## Evidence and completion

The [progress dashboard](docs/progress-ui.md) derives its contents from the hardware
matrix, handoff, application evidence, validation policy, Git activity and test
queue. Update those canonical records during normal work. Do not maintain a
second status summary in UI code or regenerate the UI for progress updates.

Record the changed behavior, selected check, result, and relevant input identities.
Use concise summaries in the repository and keep raw evidence in the local cache.
No driver is qualified merely because it compiles. Completion requires the entire
accepted objective; intermediate milestones must be labeled as such.
