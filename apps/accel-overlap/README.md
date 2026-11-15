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

A failure ends the session with `ss_fail_value`: the low 12 bits name the check in [main.c](main.c), bits 16 to 19 the
phase (5 after the last phase), and the value the measured operand.
