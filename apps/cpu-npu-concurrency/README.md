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
The combined profile's manifest declares `"observe": true` in its `npu` section.
The admission refuses that declaration outside the returning lifetime, and
`ss_npu_observe` refuses an application that does not declare it.
The application registers its graph after admission, so the declaration's
`tensor_bytes` must hold the graph's whole write region: its memory pool and
its outputs, each in whole pages. For shufflenet-v2-published the OS layout
(`nbg_assembly_layout`) gives a 1,093,632-byte pool and a 4,096-byte output, so
the manifest declares 1,097,728 bytes; the input needs 151,552 and the
coefficients and descriptors 917,504, inside `graph_bytes`.

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
```

For the combined profile add the artifact map of the reviewed
shufflenet-v2-published graph and vectors. The application links only the reference output (at most eight bytes). The
graph and the input arrive in the manifest's two input regions, `models` and `vectors`, which the runner uploads: `models`
starts with the graph index that `tools/npu-external-pack.py` writes ("NMDL", version, count, then each graph's offset and
length) and holds the graph behind it; `vectors` holds the input tensors. The application reads the index, registers the graph
with the OS at run time (`ss_npu_register_graph`, as the declaration's `graph_bytes` and `tensor_bytes` bound it) and runs the
input from the second region. It does not name `npu.graph_region`: that field needs the ring (more than one job in flight),
and this application runs the synchronous service.

```text
-DSUNSTONE_CPU_NPU_ARTIFACTS=<artifact-map.json>
```

`vectors.py --artifacts <artifact-map.json> --models checks/data/npu-test-model-list.json --name shufflenet-v2-published
--output <reference.c> --region <vectors.region>` authenticates the graph, the input and the reference against the catalogue
and writes the reference source and the vectors region. `tools/npu-app-image.py --app apps/cpu-npu-concurrency --manifest
manifest-npu.json --manifest-variable SUNSTONE_CPU_NPU_MANIFEST --freeze-decoder '--mode 1' --define SUNSTONE_CPU_NPU_ARTIFACTS=<artifact-map.json>` packs
the models region, fills both regions' addresses, sizes and digests and the declaration into the manifest, and builds the
image and a session built from the decoder frozen to that image (`--freeze-decoder` is required: the unfrozen decoder fails at the runner's decode), for a board run. The gate image (`ret-npu-load`) uses the manifest as it stands, with unpinned regions.

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
