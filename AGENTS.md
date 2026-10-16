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

- Identify the target before issuing device commands. Do not open other boards.
- Hold an exclusive target lock during device operations. Coordinate access with
  existing shared-board tooling when a controller or acoustic resource is shared.
- Start with identity queries and RAM-only execution. Preserve storage contents
  and establish recovery before persistent writes.
- Stop device communication at the first transport failure or disappearance.
  Do not loop resets, downloads, or reconnections around an unresolved failure.
- Do not transmit keyboard events into the user's desktop during tests.
- Document missing peripherals as qualification blockers and continue independent
  work. Do not count source inspection or emulation as physical qualification.

## Evidence and completion

Record the changed behavior, selected check, result, and relevant input identities.
Use concise summaries in the repository and keep raw evidence in the local cache.
No driver is qualified merely because it compiles. Completion requires the entire
accepted objective; intermediate milestones must be labeled as such.
