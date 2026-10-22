# Measurements: NPU output conformance with the SDK simulator

These measurements compare the branch that adds the NPU conformance harness
with its base revision, `audit/integration` at commit `041c09a`, against the
budgets in [the design](npu-conformance.md). All runs used the host only; no
command contacted USB devices or the board.

## Conditions

- **Firmware.** Both revisions were configured with the project's standard
  cross-build options and built with `nice -n 10` and six jobs. Both builds
  produce the same 13 ELF images, and every image's loadable bytes, extracted
  with `llvm-objcopy -O binary`, are identical between the revisions. The
  summed text, data and BSS sizes are 2,026,146, 23,017 and 27,198,560 bytes
  in both. The firmware's instruction counts, wake-ups, copies, barriers and
  lock holds per operation are therefore unchanged, which agrees with the
  design: the package changes no product code.
- **Baseline host cost.** The base revision has no conformance harness. Its
  fixture pipelines, `tools/npu-conv-pipeline.sh` and
  `tools/npu-dual-pipeline.sh`, were run once each in the pinned compiler
  image with no network, six CPUs and a 14 GiB limit. Each took 13.5 s and
  ran one simulator job, one runner build and six compiler commands: import,
  input description, float inference, quantize, quantized inference and
  export.
- **Branch host cost.** `tools/npu-conformance.py run` was run twice, cold,
  with `nice -n 10`. Both runs passed. The counters below come from the
  run's `report.json`; the two runs gave equal counters and memory peaks of
  9,308 and 9,287 MiB. The host check `checks/npu-conformance.py` passes in
  0.06 s.

## Results

| Metric | Budget | Base revision | Branch | Within budget |
| --- | --- | --- | --- | --- |
| Simulator executions per simulator key | One in total; zero when every key is cached | One per pipeline invocation, repeated each invocation | One per run; a second run repeats all 84 jobs | Yes, under the design's rule that drops the cache when a cold run takes under 60 s |
| Simulator jobs, cold | At most 25 base, 55 input-line and 5 repeats | 1 per pipeline invocation | 84: 24 base, 55 input-line, 5 repeats | Yes |
| Runner builds | One per runner and binding; five cold | 1 per pipeline invocation | 5 per run | Yes; no cache, as above |
| Runner invocations, cold | Five | 1 per pipeline invocation | 5 | Yes |
| Compiler commands per run | At most 15 quantized inferences | 6 per pipeline invocation | 15 quantized inferences, plus 5 layer-table reads by the SDK inspector | Yes |
| Peak concurrent processes | At most six | 1 | 6 | Yes |
| Concurrent runner processes | At most two | 1 | 2 lanes: ShuffleNetV2 alone, the four short models in turn | Yes |
| Container memory peak | Under the 14 GiB limit | Not measured | 9,308 MiB; largest single process 4,401 MiB | Yes |
| Container starts per run | One with any miss | 1 per pipeline invocation | 1 | Yes |
| Lines covered by a job pair with different expected outputs | Every input and output line of each pinned-compiler model | Not applicable | All 55 input lines and 52 output lines; no consecutive pair has equal outputs | Yes |
| Lines changed outside owned files | Zero | Not applicable | 9 lines added to `docs/hardware.md` | No |
| Wall time | Recorded, no budget | 13.5 s per pipeline invocation | 46.0 and 46.4 s; ShuffleNetV2's runner invocation, 45.5 s, is the critical path | No budget |
| Firmware code and data size | Unchanged | 2,026,146 text, 23,017 data | Identical images | Yes |

## Result outside budget

The branch adds a nine-line status entry for the conformance harness to the
hardware matrix, `docs/hardware.md`. The repository's working rules keep
completion claims in that matrix, but the design neither lists the matrix as
an owned file nor gives a reason for changing it. The design should either
name the matrix entry as an owned change or the entry should move to the
package's own record, `docs/npu-conformance.md`.
