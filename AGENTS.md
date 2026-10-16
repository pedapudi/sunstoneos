# Working rules

The accepted objective is [docs/goal.md](docs/goal.md). Read the current
[handoff](docs/handoff.md) before continuing. The
[hardware matrix](docs/hardware.md) defines the support inventory independently
of application requirements. Project issues hold work scope and dependencies.

## Implementation

- FreeRTOS is the runtime OS. Linux is a source and development reference only.
- Complete onboard hardware support and local mimus execution are separate
  completion requirements. A CPU fallback does not satisfy accelerator support.
- Work on isolated branches or worktrees. Preserve other workers' changes.
- Keep downloaded sources, models, firmware blobs, builds, and raw logs outside
  tracked source. Pin dependencies and record their provenance and hashes.
- Preserve third-party license notices. Never add generated-by notices, vendor
  attribution tags, or assistant co-author tags to project files or Git metadata.
- Use the writing skill for all communication and documentation.

## Validation

Apply [docs/validation.md](docs/validation.md) in both this project and the mimus
port. The user's selective validation policy replaces unconditional full-suite
reruns; substantive correctness requirements still apply. A commit, clean rebase,
handoff, or final response does not invalidate successful test evidence.

## Hardware access

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
- Start with identity queries and RAM-only execution. Preserve storage contents
  and establish recovery before persistent writes.
- Stop device communication at the first transport failure or disappearance.
  Do not loop resets, downloads, or reconnections around an unresolved failure.
- Before intentional USB re-enumeration, require a reconnect access rule for the
  target port and user. Wait for udev permissions before the next protocol request.
  Do not reset again to work around missing host permissions.
- The UFS probe exposed a failure that the CPU secure timer did not recover.
  Qualify an independent hardware watchdog and ROM return before executing new
  peripheral-access sequences. Keep UFS execution disabled until that recovery
  path is integrated. A cached USB ROM descriptor does not prove ROM execution;
  respect the host's failed-instance record in `tools/usb-access.py`.
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
