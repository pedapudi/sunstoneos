Build SunstoneOS in ~/git/sunstoneos into a minimal FreeRTOS-based OS for the Orange Pi
Zero 3W, fully support all usable onboard hardware, and run ~/git/mimus natively.
Proceed autonomously through implementation, integration, physical bring-up,
qualification, and subsequent performance optimization.

1. Make complete device integration the top priority Finish integrating existing
components before expanding isolated infrastructure. A complete device path includes
native initialization, resource ownership, application access, successful operations,
interrupts where applicable, and bounded failure recovery.

Use a reviewed recovery plan for each physical test, then complete shared platform
services and integrate dependent devices. Supervised RAM-only bring-up may use
manual USB reconnection. Watchdog reset qualification and USB power automation
are deferred; neither blocks unrelated integration. Intentional reset, unattended
fault tests and persistent writes retain their recovery requirements. Choose work
by how directly it advances physical functionality or removes a concrete
integration blocker.

When physical testing is blocked, complete native image integration and focused software
checks. Continue independent work without treating emulation as physical qualification.

2. Preserve complete hardware coverage Support all CPU cores, memory, interrupts,
clocks, resets, power, DMA, storage, exposed buses, USB, Wi-Fi, Bluetooth, audio,
cameras, displays, GPU, NPU, video/image codecs, PCIe, timers, watchdogs, thermal
management, security engines, and remaining usable onboard functions.

Maintain an inventory of capabilities, operational modes, application interfaces,
dependencies, shared-resource constraints, and qualification evidence. Account for
mutually exclusive modes. Do not omit hardware because mimus does not use it or because
implementation is difficult. A CPU fallback does not satisfy GPU or NPU support.

Distinguish unconnected SoC interfaces from usable board functions using schematics and
physical identification. Implement operational modes incrementally while retaining
outstanding requirements in the inventory.

3. Keep SunstoneOS minimal and practical FreeRTOS is the runtime OS. Linux may provide
source references and development tools but must not be a runtime dependency. Native
drivers, adapted open-source code, firmware blobs, and reverse engineering are allowed.

Prefer fixed board configuration and existing shared services where sufficient. Add
abstractions only for concrete consumers, actual resource conflicts, or identified
correctness requirements. Explain what fails without each new mechanism. Consolidate
duplication.

Preserve necessary DMA ownership, cache coherence, shared power/clock coordination,
bounded failure handling, and storage protection.

Defer optional frameworks, speculative features, and the native NPU development toolkit
until hardware functionality is complete. External development tools remain acceptable.
Keep the native toolkit tracked as lower-priority follow-up work.

4. Run the existing mimus application Preserve the existing model, weights, and
established transcription, punctuation, and formatting requirements. Establish reference
parity before accelerator optimization. Do not retrain or substitute a smaller model
merely to meet performance targets.

Develop with recorded 16 kHz mono audio and paced replay while no microphone is
available. Implement both I2S microphone and Bluetooth headset input, including
hands-free control and voice transport.

Use OV5640 as the first camera target. Keep DSI panel configuration separate from
controller support until a panel is selected. Investigate Hawkeye 64 MP compatibility
separately without assuming full-resolution support.

Missing accessories remain explicit physical qualification blockers. Continue everything
possible with available equipment.

5. Pursue ambitious performance after hardware functionality After every required
hardware function works, target complete warm recognition time of at most 0.05 times
audio duration. Pursue 0.01 times audio duration as a stretch target whose feasibility
must be demonstrated.

For ten seconds of audio, these targets mean 500 ms and 100 ms respectively. Measure
from available PCM to formatted transcription, including feature extraction, inference,
decoding, punctuation, accelerator transfers, and synchronization.

Report model loading/startup time and live end-of-speech latency separately. Use
representative recording lengths and sustained operation; do not establish compliance
from one favorable clip or accelerator kernel timing.

Evaluate optimized CPU, GPU, and NPU execution. Use the combination that improves the
complete application; do not require all three to participate in each transcription.
Preserve model integrity and established accuracy criteria. Arithmetic changes require
the relevant accuracy evaluation.

The 0.05-times target is an acceptance requirement. The 0.01-times stretch target does
not justify indefinite optimization after a bounded investigation establishes the
remaining limitation.

6. Develop in parallel without hardware collisions Use isolated worktrees, explicit file
ownership, and separate build/evidence directories. Assign workers complete integration
outcomes where practical.

Coordinate physical tests through queues and exclusive target locks. Workers develop
through source inspection, host checks, and emulation, then submit bounded physical test
requests.

If multiple boards become available, identify each explicitly and use separate
permissions, locks, queues, and evidence. Qualify recovery independently on each board.
Never select an arbitrary matching USB device.

Preserve existing storage contents. Stop communication after transport failures or
disappearance; do not retry resets, downloads, or reconnections automatically around
unresolved failures.

7. Validate parsimoniously Before each test batch, identify the changed behavior,
credible failure risk, and smallest check that resolves it.

Reuse successful evidence while relevant source, toolchain, configuration, fixtures,
model, firmware, and hardware conditions remain valid. Commits, handoffs, conflict-free
rebases, and documentation updates do not justify rerunning tests.

Build affected targets only. Reuse matching artifacts and avoid unnecessary flashing.
Run broader integration checks when shared foundations change or a milestone lacks valid
evidence.

Repeated trials and performance measurements require a concrete question and a declared
sample count or stopping rule. Do not run recurring benchmarks during hardware bring-up.
Preserve substantive correctness and recovery requirements.

8. Track outcomes efficiently Maintain project issues, the hardware support matrix, and
a concise handoff. Keep one authoritative record for each fact. Let the progress
dashboard derive its contents from those records.

Distinguish implemented, software-tested, physically working, and qualified. Record the
next useful action and actual blockers. Preserve reproducible artifacts and relevant
evidence without creating duplicate reporting systems.

Use the writing skill for all communication. Never add vendor attribution tags,
generated-by notices, or assistant co-author tags to repository content or Git metadata.
Preserve required third-party legal notices.

9. Work autonomously within established boundaries Inspect sources and firmware, obtain
dependencies, edit both repositories, build tools, commit locally, maintain tracking
issues, and execute approved procedures on verified project boards.

Do not push or merge without authorization. Leave unrelated devices and other workers’
processes undisturbed. Preserve storage and establish recovery before persistent writes.

Resolve routine engineering decisions independently. Ask only for unavailable physical
actions, missing information that materially blocks progress, or actions outside the
authorized scope. Report the smallest necessary user action and continue independent
work.

10. Complete the entire system Completion requires reproducible bootable images,
recovery and update procedures, working native mimus transcription, physical
qualification of every required hardware function, and the 0.05-times recognition
target.

Include representative concurrent workloads, cancellation and error recovery, applicable
power transitions, sustained operation, and measured resource use.

Booting, a transcription demonstration, compiling drivers, or individually passing
software models are intermediate milestones. Do not declare completion while required
integration or qualification remains outstanding. Documented limitations do not
substitute for successful implementation.
