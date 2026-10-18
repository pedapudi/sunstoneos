Build SunstoneOS into a complete, practical FreeRTOS-based OS for the Orange Pi Zero 3W/A733, support all usable onboard hardware, and run ~/git/mimus natively. Proceed autonomously through implementation, physical integration, qualification and subsequent performance optimization.

Work in ~/git/sunstoneos-worktrees/bringup. Read AGENTS.md, docs/goal.md, docs/handoff.md and docs/hardware.md before continuing. Preserve existing work and reuse valid evidence.

1. Prioritize working hardware

Complete native device paths before adding infrastructure or expanding software-only demonstrations. Each path must include initialization, resource ownership, application access, successful operations, interrupts where applicable, and appropriate failure handling.

Support all CPU cores, memory, interrupts, clocks, resets, power, DMA, storage, exposed buses, USB, Wi-Fi, Bluetooth, audio, camera and display interfaces, GPU, NPU, codecs, PCIe, timers, watchdogs, thermal management, security engines and other usable onboard functions. Application requirements do not narrow hardware coverage.

FreeRTOS must be the runtime OS. Linux may supply driver source and development references but must not be a runtime dependency.

2. Finish multicore bring-up directly

The primary agent must personally handle multicore diagnosis, implementation, review and board testing without delegation.

Reuse the successful physical core-1 release, preparation, shutdown and ROM-return evidence. Continue from the implemented GIC-600 redistributor power-up correction: establish the relevant physical state, provide matching diagnostic cleanup, and qualify per-core interrupt initialization, all remaining cores, cache coherence and FreeRTOS scheduling.

Do not repeat a failed launch without a concrete correction or a narrower diagnostic justified by new evidence.

3. Use both boards and parallel development efficiently

Keep independent identities, permissions, locks, queues and evidence for the boards on USB ports 3-1 and 3-3. Preserve both microSD cards. Qualify the second board’s DDR, platform state and recovery independently.

Use the original board for direct multicore work and the second for eligible independent hardware integration. The primary agent coordinates physical access. Subagents develop without hardware access and submit frozen artifacts for queued testing.

Use Sol 6.1 medium implementation workers and focused Astra review for substantive correctness risks. Run independent complete device workstreams concurrently when they can advance without conflicting edits or integration bottlenecks. Integrate completed work promptly and reassign useful work instead of leaving workers idle unnecessarily.

4. Preserve recovery and storage

Start with supervised RAM-only execution and reviewed return procedures. Stop device communication after the first transport failure; preserve evidence and require explicit recovery before reuse.

Do not make watchdog expiry or USB power automation prerequisites for unrelated work. Keep the failed UFS probe disabled until its failure and recovery are resolved. Persistent writes require established recovery and preservation of existing contents.

5. Run the existing mimus application

Preserve its model, weights, transcription, punctuation and formatting requirements. Establish reference parity before acceleration; do not substitute a smaller model to meet performance targets.

Use recorded 16 kHz mono audio and paced replay while no microphone is attached. Implement I2S microphone and Bluetooth headset input.

No MIPI camera or display is attached. Support OV5640 first; keep DSI panel configuration separate until a panel is selected. Record accessory-dependent qualification blockers and continue independent work.

6. Keep the system minimal

Prefer fixed configuration, existing services and suitable open-source implementations. Add abstractions only for concrete consumers or demonstrated correctness requirements.

Document third-party source, firmware and tool dependencies, their licenses, provenance and execution locations. Opaque dependencies are temporarily acceptable. Defer their replacement and the native NPU development toolkit until hardware functionality and performance tuning are complete. Do not rewrite adequate open-source dependencies merely to make them repository-native.

7. Validate parsimoniously

Before each test batch, identify the changed behavior, credible failure risk and smallest useful check. Reuse successful builds and tests while their relevant inputs remain valid. Run only affected or previously unexecuted cases after fixture repairs.

Do not rerun tests for commits, handoffs or documentation changes. Avoid recurring benchmarks during bring-up. Fix recurring fixture setup defects instead of repeatedly repairing individual runs.

8. Optimize after hardware functionality

After required hardware functions work, achieve complete warm mimus recognition within 0.05 times audio duration. Investigate 0.01 times as a bounded stretch target.

Measure PCM-to-formatted-transcription time, including preprocessing, inference, decoding, punctuation, transfers and synchronization. Report loading and live end-of-speech latency separately. Evaluate CPU, GPU and NPU execution while preserving model integrity and accuracy.

9. Track progress and continue autonomously

Keep GitHub issues, the hardware matrix and handoff current. Let the existing dashboard derive progress from those records. Distinguish implemented, software-tested, physically working and qualified.

Use the writing skill. Never add vendor attribution tags, generated-by notices or assistant co-author tags; preserve required legal notices.

Local commits and issue updates are authorized. Do not push or merge without authorization.

Continue across intermediate milestones while useful authorized work remains. Ask only for necessary physical actions, genuinely blocking information or actions outside scope. Report usage-limit interruptions explicitly.

Completion requires reproducible bootable images, recovery and update procedures, native mimus, physical qualification of every required hardware function, representative concurrent workloads and the 0.05-times recognition target. Missing accessories and documented limitations remain outstanding requirements.
