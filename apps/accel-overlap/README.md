# Run the GPU, the NPU and CPU streaming together

This returning application runs GPU vector-add jobs, NPU relu jobs and five CPU streaming workers at the same time, and
records the completion latency and job rate of each accelerator. The design, the placement and the predictions are in
[concurrent-accel.md](../../docs/designs/concurrent-accel.md).

The image needs `SUNSTONE_A733_OS_GPU`, `SUNSTONE_GPU_FAST_TIMING`, `SUNSTONE_NPU_WAKE_TIMING` and
`SUNSTONE_ACCEL_PLL_WATCH`; [cmake/application.cmake](cmake/application.cmake) refuses a build without them.

Build, from the worktree:

```
B=~/.cache/sunstoneos/build/accel-overlap
mkdir -p $B/graphs $B/data
# $B/graphs: artifacts.json with the graph-relu entry of the compiled graph variants, and list.json {"npu_models": ["graph-relu"]}
python3 -B apps/accel-overlap/pack.py --graphs $B/graphs --output $B/data/vectors.region
~/.cache/sunstoneos/venv/bin/python3 -B tools/npu-app-image.py --app $PWD/apps/accel-overlap --graphs $B/graphs \
    --data $B/data/vectors.region --out $B/image --queue-depth 8 --manifest-variable SUNSTONE_ACCEL_OVERLAP_MANIFEST \
    --define SUNSTONE_A733_OS_GPU=ON --define SUNSTONE_GPU_FAST_TIMING=ON \
    --define SUNSTONE_NPU_WAKE_TIMING=ON --define SUNSTONE_ACCEL_PLL_WATCH=ON
```

`recipe.txt` in the output directory holds the session and submit commands. Submit with `--kind npu` and `--restore-rails` (the NPU raises its rail), both input
regions and the session's decoder. [host.py](host.py) decodes the 1,200-byte result: per phase, the wake and round-trip
distributions of both devices, their pipelined job rates, the NPU completions seen while a GPU job was in flight, the
streaming sweeps, and the PLL_NPU watch. It also lists each predicted range and whether the value fell inside it.

Adding `--define SUNSTONE_ACCEL_OVERLAP_SWEEP=ON` builds the streaming sweep instead: five one-second phases of both
devices pipelined under five sets of streaming workers, with a dependent-load chase on core 0 timing the loaded memory
latency. Its result is 1,488 bytes (version 2), and host.py applies the stage-1b predictions and decision rule to it
([design](../../docs/designs/concurrent-accel.md#stage-1b-the-loss-follows-the-number-of-cpu-streams)).

Adding `--define SUNSTONE_ACCEL_OVERLAP_LARGE=ON` with `--manifest manifest-large.json` builds the larger-job sweep:
262,144-element GPU adds in four 1 MiB output slots and a 1024x176 relu (the graphs directory names the `relu-large`
fixture), in nine phases that add device-alone controls and three interleaved streams on one core. Each GPU phase ends
with two serial jobs per slot, one with a one-line sync range and one with the whole slot, which time the output cache
maintenance. Its result is 2,944 bytes (version 3).

`--define SUNSTONE_ACCEL_OVERLAP_WIDTH=ON`, with the same manifest, builds the GPU-width run: the larger-job build with
the add of [add4/vector_add4.cl](add4/vector_add4.cl), in which each lane adds four consecutive elements, and only the
GPU-alone phase. A job runs 2,048 workgroups. [add4/program.h](add4/program.h) holds the compiled program and
[add4/sources.json](add4/sources.json) its compiler record; `apps/gpu-program/compile.py compile --profile 3
--queue-program --inline-arguments` reproduces it. Like the one-element add, it receives its arguments in unified
registers once per task. The result keeps the 2,944-byte layout with version 4 and the later phases zero.

`--define SUNSTONE_ACCEL_OVERLAP_COUNTERS=ON`, with the OS option `SUNSTONE_ACCEL_NSI_COUNTERS=ON` and `--manifest
manifest-counters.json`, builds the NSI counter run. Its graphs directory names four relu graphs of 1,024 rows, in
this order: `relu-large` (176 values per row), `relu-w128`, `relu-w144` and `relu-w256`; pack.py then writes a version-2
vectors region with one input and one reference per graph. The GPU runs the dual-mode add of
[add4inline/vector_add4_inline.cl](add4inline/vector_add4_inline.cl), compiled with `compile.py compile --profile 3
--queue-program --inline-arguments` into [add4inline/program.h](add4inline/program.h), which receives its arguments in
unified registers. The build runs eight phases: idle, the GPU add with its arguments loaded by every lane, the same
add with the arguments loaded once per task, the four relu widths, and the core-2 stream. Each phase starts its
devices, waits 20 ms, then counts NSI ports 6 and 18 over five 100 ms periods and samples both banks 3 ms before and
2 ms after every period end. The result is the larger-job layout with version 6 and eight phases, followed by the counter
record, 9,496 bytes in all (result.h, struct accel_overlap_counter_result). A counter call that fails ends the phases
after the one it happened in, and the record keeps its status. host.py decodes the counts of each period, the jobs per
period and the predictions of [the design](../../docs/designs/concurrent-accel.md#arguments-once-per-task).

A failure ends the session with `ss_fail_value`: the low 12 bits name the check in [main.c](main.c), bits 16 to 19 the
phase (5 after the last phase), and the value the measured operand.
