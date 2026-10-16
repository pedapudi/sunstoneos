Build sunstoneos in ~/git/sunstoneos into a minimal,
FreeRTOS-based OS for the attached Orange Pi Zero 3W, fully support
all onboard hardware, and port ~/git/mimus to run on it. Proceed
autonomously through implementation, hardware bring-up, debugging,
and qualification.

1. Architecture
FreeRTOS is the operating system. Linux may serve as a development
reference but must not be a runtime dependency. Native drivers,
adapted open-source code, vendor firmware, and reverse engineering
are allowed. Investigate difficult dependencies early. Continue
beyond the first successful boot or transcription.

2. Complete hardware coverage
Inventory the physical board and all usable SoC blocks from
schematics, documentation, source code, and device identification.
Create a support matrix independently of mimus requirements.

Include all CPU cores, memory, interrupts, clocks, resets, power,
DMA, storage, exposed buses, USB, Wi-Fi, Bluetooth, audio, cameras,
displays, GPU, NPU, video codecs, PCIe, timers, watchdogs, thermal
management, security engines, and remaining onboard functions.

For each item, record its capabilities, operational modes, driver,
application interface, dependencies, and qualification evidence.
Account for shared pins, bandwidth, and mutually exclusive modes.
Do not silently remove difficult or unused hardware from scope.
A CPU fallback does not satisfy accelerator support.

3. Working mimus
Port the existing model and portable C++ engine first. Develop
without a microphone using recorded 16 kHz mono audio and paced
replay. Preserve model integrity and deterministic reference
behavior, including punctuation and formatting.

Then add Arm optimizations and multicore execution. Evaluate GPU
and NPU backends separately, with accuracy checks when arithmetic
changes. Measure compute time and end-to-end latency against the
existing performance target. Do not retrain or change the model
merely to simplify the port.

4. External peripherals
Implement I2S microphone input and Bluetooth headset microphone
input, including hands-free control and voice transport. OV5640
is the first camera target. Keep DSI panel configuration separate
from controller support until a panel is selected.

Investigate Hawkeye 64 MP compatibility separately, including
hardware resolution limits. Do not assume full-resolution support.

Implement and test everything possible with available equipment.
Missing peripherals remain explicit qualification blockers.
Software tests do not substitute for physical interface tests.

5. Parsimonious validation
Apply change-dependent validation in both repositories. Before
each test batch, identify the changed behavior, relevant failure
risk, and smallest check that resolves it.

Use focused checks during iteration. Reserve broader integration
checks for milestones or changes affecting shared foundations.
Documentation-only changes receive documentation checks. Do not
add tests that merely repeat implementation details.

Reuse successful evidence while relevant inputs remain unchanged:
source dependencies, toolchain, build configuration, fixtures,
model, test code, firmware image, hardware, and relevant device
state. Commits, conflict-free rebases, handoffs, and final summaries
are not reasons to rerun tests.

Rebuild and flash only when necessary. On the final candidate,
run acceptance checks that lack valid evidence. Repeat trials
only to investigate failures or measure repeatability, with a
stated sample count or stopping rule.

Preserve substantive correctness requirements while replacing
unconditional full-suite reruns with this policy.

6. Tracking and communication
Maintain project issues, a hardware support matrix, and a concise
handoff. Record dependencies, acceptance criteria, source and
image identities, reusable test evidence, limitations, and the
next useful action. Establish these records and standing rules
before substantial implementation.

Keep one authoritative record for each fact. Use the writing skill
for all communication. Never add vendor attribution tags,
generated-by notices, or assistant co-author tags to repository
content or Git metadata.

7. Autonomous working boundaries
You may inspect sources and firmware, obtain dependencies, build
tools, modify both repositories in isolated branches or worktrees,
commit locally, maintain project issues, and load or flash the
verified Orange Pi target.

Preserve existing storage contents and establish recovery before
persistent writes. Respect shared hardware access and USB failure
rules. Leave unrelated devices and other workers' processes
undisturbed.

Resolve routine engineering choices independently. Ask only for
decisions that cannot be inferred, unavailable physical access,
or actions outside these boundaries.

8. Completion
Completion requires reproducible bootable images, recovery and
update procedures, working local mimus transcription, and
functional qualification of every item in the hardware matrix.

Include representative concurrent workloads, error recovery,
applicable power transitions, and measured resource use. A
compiling driver, emulator result, placeholder API, or documented
limitation does not establish hardware support.

Booting and running mimus are intermediate milestones. Do not
declare the overall goal complete while required hardware support
or qualification remains outstanding.

9. Blockers
Continue independent work when one component is blocked. If
further progress requires missing hardware, unavailable interfaces,
or a user decision, report the specific blocker, supporting
evidence, attempted approaches, and smallest action needed.

Do not keep repeating unsuccessful experiments without a new
hypothesis or changed conditions. Do not substitute a blocker
report for successful completion.