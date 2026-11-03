# Eight inference workers in the supervised OS

Status: **host-tested**. The supervised OS uses the eight-worker engine and adapter ([mimus-eight-workers.md](mimus-eight-workers.md)) with its model, weights, arithmetic, producer scratch and session format unchanged. The adapter is in the mimus checkout and the admission and link checks are focused host checks; physical execution is **open**.

## Supported composition and placement

The supported OS composition is supervised recorded recognition with `A733_A76_ADMITTED=ON`. It keeps the CPU clock and performance-mode options, including their NPU composition, and the [GPU extension](gpu-eight-workers.md) also admits supervised recorded GPU recognition. Resident recorded CPU/NPU operation uses its own cold entry contract, and resident GPU, microphone capture, radio and headset are excluded. Two-worker configurations keep their rules.

The owner runs on A76 core 6 and the first worker on A76 core 7, both at priority 3, and the remaining workers on A55 cores 0 through 5 at priority 2. The OS PMIC task runs at priority 3 on core 0, USB and thermal services at priority 4, Type-C and power services at priority 5, and the optional NPU client at priority 3 on core 1. Priority 2 keeps A55 inference below these owners while preserving every service affinity. Row partitioning is equal, with no heterogeneous speed ratio.

`mimus_parallel_init` validates the OS owner and worker pair against `a733_smp_placement.h`, the selected count is `SUNSTONE_MIMUS_WORKERS`, and the compile guard admits the supported eight-worker composition and rejects other OS profiles. Standalone native and QEMU configurations are unchanged.

## Linked configuration and host admission

The adapter keeps a small constant record in OS images (at most 32 bytes) with its version, worker count, owner core, complete inference core mask and A55 and A76 priorities, taken from the compile-time constants that allocate slots and create tasks. The OS build identity includes the selected count. The link check reads the record from the ELF, checks the selected count and profile, and publishes the configuration in the image manifest.

Host preparation and frozen-bundle verification compare the manifest with the ELF record, and an eight-worker image also requires the reviewed entry record to name that exact inference configuration. This adds no session bytes, device operation or approval schema. Existing two-worker artifacts without the record stay eligible, and removing the metadata from an eight-worker manifest fails against the linked record.

## Bounded jobs and failure retention

The adapter keeps each callback context until every submitted slot acknowledges its matching generation, and all eight tasks use the OS recognition checkpoints. A throttle checkpoint sleeps once, rechecks session liveness and only then records progress. A closed session suspends the caller through the terminal path, and if a worker stops before acknowledging, the owner stays blocked with its context and buffers retained. Neither thermal failure nor performance-mode failure fabricates a join or starts a later recognition generation. The thermal policy and checkpoint implementation are unchanged. Physical service latency, heterogeneous throughput and recovery are unmeasured.

## Cost

Eight workers add six 64 KiB stacks and 5,760 bytes of private queue and task slots to the two-worker adapter. No inference-arena, producer scratch or session allocation grows. The OS image keeps its code and RAM reservations, 4 MiB heap, model addresses and inference-arena bounds, and the optional NPU pool stays separate.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Linked CPU/NPU performance composition | version 1, eight workers, owner core 6, mask 255, priorities 3, 2 and 3; stacks 458,752 B, slots 6,720 B; heap ends at `0x41803640` | `mimus-os-eight-workers-software` |
| Host cases (linked metadata, profiles, fresh and frozen entry) | 31 pass | `mimus-os-eight-workers-software` |
| Excluded profiles rejected by compile and configuration guards | 6 | `mimus-os-eight-workers-software` |

Records: [mimus-os-eight-workers.jsonl](../evidence/mimus-os-eight-workers.jsonl). The FreeRTOS adapter fixture (owner core 6, workers on cores 7 and 0 through 5, priority-3 services on A55 cores 0 and 1) passes fixed placement, service preemption and failure retention through the generated OS checkpoint.

## Open work

1. Run a supervised eight-worker session on a board and measure service latency and recovery.
