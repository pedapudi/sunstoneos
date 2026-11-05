# Verify eight CPU workers during NPU execution

This diagnostic runs through the common application kit. CPU6 and seven pinned
priority-2 workers compute checked float32 16-by-16 matrix products. Every unit
poisons its output, reads volatile inputs, checks every cell against an integer
formula and publishes its digest before its completed-unit counter. The host
independently computes the expected digest with integer matrix multiplication.

The CPU control selects no NPU service. The combined profile selects the model
named `shufflenet-v2-published` from caller-supplied catalogue data and runs four
sequential jobs. Its input and reference are authenticated during configuration.
The model's output establishes reference equality only.

An optional public observer registers before scheduling. Its OS adapter owns
all NPU register reads. A first non-idle sample precedes counter snapshot A.
Checked arithmetic wakes the existing worker after every CPU advances. The
worker takes snapshot B and then requires the same job still RUNNING and busy.
Every job also requires one actual IRQ receipt and a complete output match.
The observer is compiled out unless `SUNSTONE_A733_OS_NPU_OBSERVE=ON`.

Work admits no new CPU unit after its five-second cutoff. NPU admission reserves
2.2 seconds before that cutoff; CLOSE retains a separate two-second allowance.
Workers acknowledge one stop epoch before the application publishes its
1,728-byte receipt. Native FINISHED requires SDK CLOSE, final worker park,
local NPU stop and live failure checks. The terminal cleans the explicit eight
MiB NPU pool after every CPU captures and drains its application writers.

Register the application with these root-project CMake settings, using the
repository's existing compiler and dependency configuration:

```text
-DSUNSTONE_A733_OS=ON -DSUNSTONE_MIMUS_BOARD_APPLICATION=ON
-DSUNSTONE_APPLICATION=cpu-npu-load
-DSUNSTONE_APPLICATION_cpu-npu-load_ROOT=<source>/apps/cpu-npu-concurrency
-DA733_A76_ADMITTED=ON -DSUNSTONE_A733_OS_CPU_CLOCK=ON
-DSUNSTONE_A733_OS_CPU_RETURN=ON
```

For the combined profile add NPU and observer enablement, an authenticated
artifact map and the semantic model selection:

```text
-DSUNSTONE_A733_OS_NPU=ON -DSUNSTONE_A733_OS_NPU_OBSERVE=ON
-DSUNSTONE_OS_NPU_CATALOGUE=<artifact-map.json>
-DSUNSTONE_OS_NPU_MODEL_NAME=shufflenet-v2-published
```

`SUNSTONE_OS_NPU_MODELS` optionally supplies a different model-list JSON file.
The diagnostic rejects GPU, DMA, performance mode, audio, radio, headset and USB
class composition. Its source enables FP instructions within the existing task
FP context; service sources retain their general-register compilation policy.

Run `checks/cpu-npu-concurrency.py --out <new-directory>` for producer, arithmetic,
observer and receipt checks. Given a host-test ELF, run
`checks/cpu-npu-integration.py <elf> --out <new-directory>` for close, local-stop,
owner-retirement and FINISHED gates. `checks/os-cpu-return.py` executes the actual
terminal and pool-clean loop with modeled peripheral effects.

Freeze a standalone decoder with `host.py --freeze-image <elf> --mode <0-or-1>
--decoder-output <file>`. Pass that file explicitly as the runner's `--decoder`.
Its embedded binding contains the image hash, resolved local model index and
linked reference bytes. `checks/cpu-npu-geometry.py` checks that decoder against
an actual native result envelope and rejects malformed ownership geometry.
The runner must verify the decoder's image hash before device access.

Host results establish software behavior. Physical FP preemption, concurrent
operation, timing and return remain unqualified until the coordinator's reviewed
control and combined runs complete. Receipt metrics report elapsed GEMM cost,
including preemption, and four trigger-to-IRQ samples; they do not measure CPU
cycles or qualify thermal behavior.
