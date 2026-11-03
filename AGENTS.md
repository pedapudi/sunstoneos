# Working rules

The accepted objective is [docs/goal.md](docs/goal.md). Read the current
[handoff](docs/handoff.md) before continuing. The
[hardware matrix](docs/hardware.md) defines the support inventory independently
of application requirements. Project issues hold work scope and dependencies.

## Task-specific hardware skills

Read the relevant skill from your own worktree when taking an assignment. Give
each worker the skill's path, its worktree, its files and the evidence gap.

| Assignment | Skill |
| --- | --- |
| Physical access, boot preparation, board runs or recovery | [A733 board operations](.agents/skills/sunstone-a733-board/SKILL.md) |
| Secondary CPU startup, GIC, coherence or SMP diagnosis | [A733 multicore bring-up](.agents/skills/sunstone-a733-multicore/SKILL.md) |
| Native peripheral integration, source adaptation, launch/capture or focused validation | [A733 device integration](.agents/skills/sunstone-a733-device/SKILL.md) |

Skills hold reusable decisions and known failure mechanisms; current state
belongs in the handoff and the hardware matrix.

## Implementation

- FreeRTOS is the runtime OS. Linux is a source and development reference only.
- Reuse open-source drivers and libraries, including upstream Linux drivers,
  by adapting their OS interfaces to FreeRTOS. Replace a source-available
  component only for a demonstrated incompatibility or unmet requirement.
- Document each piece of third-party software: use, provenance, execution
  location, source availability and license. Replacing opaque firmware and
  development tools waits until hardware functionality and performance tuning
  are complete; driver integration needed for FreeRTOS operation does not wait.
- Run every hardware block at the highest operating point the vendor uses for
  the chip's speed bin, with the vendor's voltage. Lower clocks are optional
  modes for power saving or thermal limits and never the default or a cap.
- Complete onboard hardware support and local application execution are separate
  completion requirements. A CPU fallback does not satisfy accelerator support.
- Keep downloaded sources, models, firmware blobs, builds, raw logs and full
  disassembly outside tracked source. Record each one's URL, revision, SHA-256
  and license in [the vendor firmware sources](docs/vendor-firmware/README.md)
  or the component's sources record, so another developer can fetch the bytes.
- Track facts derived from vendor binaries under `docs/vendor-firmware/data/`:
  function, register-access, message-ID and table inventories, with the
  binary's SHA-256 and offsets, and behaviour described in your own words.
  Commit no vendor code, verbatim disassembly or extracted log text.
- Preserve third-party license notices. Use the writing skill for all prose.

## Layering

<!-- Mimus is an external application example in this layering rule. -->
SunstoneOS is a general operating system. Applications, such as the mimus
speech recognizer, live in their own repositories and build against the
SunstoneOS application kit: [include/sunstone_app.h](include/sunstone_app.h)
and [docs/designs/app-abi.md](docs/designs/app-abi.md). Add no
application-specific code to the OS. [docs/layering-audit.md](docs/layering-audit.md)
defines the layers and lists the remaining defects.

## Scope and simplicity

- Full board hardware coverage is required independently of applications. Distinguish
  usable board functions from unconnected SoC interfaces and optional
  application features, with physical evidence for the distinction.
- Prefer a complete native device path to more isolated components. Add an
  abstraction only for a current consumer, a shared-resource conflict or a
  correctness requirement, and state what fails without it.
- Keep DMA ownership, cache coherence, shared power and clock coordination,
  bounded failure handling and storage preservation. Consolidate duplicate
  mechanisms without removing these requirements.
- Defer optional frameworks, and native replacements for adequate development
  tools, until a stated need exists.
- Follow [docs/performance-architecture.md](docs/performance-architecture.md):
  interrupts instead of tick polling where interrupts exist, single-copy data
  paths, the weakest correct barrier, and validation once when a resource is
  opened. Mark protocol features that no board use needs as optional.

## Review gate and validation

The review a change needs depends on what it touches:

- **Probe or diagnostic:** its author checks it once and runs it.
- **Feature:** one reviewer and a host test.
- **Voltage, power-domain or storage-write change:** before coding, a short
  design with measurable budgets goes to a design panel of three reviewers, for
  simplicity, hardware performance and correctness. The same three review the
  code. Measure before and after; an unexplained regression blocks the merge.

Run the checks that cover the change, in a build whose feature flags select the
changed branches ([docs/validation.md](docs/validation.md)). A commit, rebase or
handoff does not invalidate test evidence. Driver and fixture authors follow
[completion and timing validation](docs/driver-validation.md).

## Documents and evidence

Design and status documents describe the current state only: what is
implemented and how, interfaces, invariants, qualified figures with a pointer
to their record, limitations and open work. A board run's prediction and result
go in an evidence record in `docs/evidence/` ([the record format](docs/evidence.md)),
never in a design or status document: queue entry ID, image, conditions,
prediction, measurement and conclusion, one record per run. Add it with
`tools/evidence.py add` and check it with `tools/evidence.py validate`. When a
result changes a design, edit the design to the new state and delete the
superseded text. Keep no chronology, rejected attempt or "before" description
in a design or status document.

## Parallel work and tool use

- Run parallel streams, each in its own worktree with its own build and
  evidence directories. Assign file ownership first; two streams never edit the
  same file. Size the number of streams by independent file ownership and the
  coordinator's integration capacity, not to raise the worker count.
- Give implementation to one agent and review to another, each with concise
  task-specific context.
- Before assigning a missing function, confirm in the current code that it is
  missing. The code decides when a document contradicts it.
- Keep handoffs concise: changed behaviour, inputs, result and remaining gap.
- Find guidance and cached sources through
  [the reference index](docs/reference-index.md), and search cached manual text
  before opening a PDF or browsing. Record the URL, revision, section and hash
  of a newly cached reference. Read instructions once; later, read only the
  relevant heading or symbol. After context recovery, read the objective and
  recent Git changes. Never substitute a cached conclusion for live board state.
- Discover paths with one scoped `rg --files` search. Summarize large output,
  and print named fields from large JSON files. After a failed command, find its
  cause and resume only the failed steps.
- Invoke tools by their configured paths: `ld.lld` needs its name and Python its
  virtualenv path. Give CMake the compiler and every dependency path on the
  first configuration; changing the compiler in an existing cache can clear
  them. After a failed configuration, keep its cache and use a fresh directory.

## Hardware access

- Neither A733 board has populated eMMC or UFS; do not probe these devices.
  Keep the failed UFS probe disabled until its failure and recovery path are
  resolved. Do not make USB power automation a prerequisite for other work.
- All board access goes through the queue: workers submit frozen images with
  `tools/board.py` and never open a USB device. Identify the target before any
  device command, and open no other board. The runner holds the per-port lock
  (`tools/usb-access.py:board_lock`) during every device operation.
- Before a board run of a subsystem, derive its complete sequence from the
  vendor reference (vendor source and binaries, upstream Linux, TF-A, manuals)
  and diff SunstoneOS against it, including checks the reference lacks. A run
  tests a prediction from that diff and changes one variable against a
  known-good image and input. Write the prediction in the run's evidence
  record before the run. Never guess one power cycle at a time.
- Make every failure observable. Returning images, which hand the board back to
  the USB boot ROM when they finish, run under the host-armed WDT0 watchdog
  and record progress in RTC breadcrumbs that survive its reset
  ([include/sunstone_crumb.h](include/sunstone_crumb.h), decoded by
  `tools/a733-crumbs.py`). Run development images with `tools/dev-run.py`.
  The [board runner and run queue](docs/board-runner.md) provide fresh-FEL
  preparation, bounded recovery and frozen submissions. Native diagnostics use
  the [probe contract](docs/board-probe-contract.md); `board.py check`
  validates their marker, inputs and result layout without USB.
- Start with identity queries and RAM-only execution. Write paths may be
  implemented and host-checked at any time; images refuse writes unless their
  session grants them. Physical writes follow section 4 of
  [docs/goal.md](docs/goal.md) and leave the rest of both microSD cards intact:
  - boot area (boot0, TOC1 and the stage-2 slots): only through the
    backup-first update path with slot fallback, after a host backup of the card;
  - a microSD data partition: once a host backup of that card exists and the
    image confines its writes to that partition;
  - an NVMe device: only one the user designates as scratch storage.
- Stop device communication at the first transport failure or disappearance.
  Do not retry a failed run automatically, loop resets or reconnections, or
  release a live run's lock because an observation timeout elapsed.
- A cached USB ROM descriptor does not prove ROM execution. Respect the
  failed-instance record that `tools/usb-access.py` enforces.
- Request a reconnect once, at the top of [docs/handoff.md](docs/handoff.md),
  naming the port and the physical action, and continue other work. Within a
  connection window, the run most likely to lose the board runs last. Without
  physical recovery, exclude runs whose normal completion requires power
  removal. A normal return does not prove recovery from a fault.
- Do not send keyboard events into the user's desktop. Record missing
  peripherals as qualification blockers. Source inspection and emulation are
  not physical qualification.

## Git and files

- Never use `rm`. Move discarded files into
  `~/.cache/sunstoneos/trash/<descriptive-name>/`, or delete generated files
  with `~/.cache/sunstoneos/bin/clean-generated.py`.
- Work on your own branch and preserve other workers' changes. Never pipe
  `git merge`; before committing, check for unmerged paths and conflict markers.
  Never use bare `git stash`; set work aside in a temporary commit.
- Add no generated-by notices, vendor attribution tags or assistant co-author
  lines to project files, commits or pull requests.
- Only the coordinator pushes, and only `bringup` to `origin` after an
  integration passes its builds and checks. Workers do not push. The pre-push
  hook `tools/git-hooks/pre-push` scans outgoing commits for credentials and
  session scratch paths.

## Evidence and completion

The [progress dashboard](docs/progress-ui.md) reads the hardware matrix, the
handoff, the evidence records, the validation policy, Git activity and the test
queue. Update those records; keep no second status summary in dashboard code.
Keep raw logs in the local cache; the evidence record points to them.
No driver is qualified because it compiles. Completion requires the entire
accepted objective; label intermediate milestones as such.
