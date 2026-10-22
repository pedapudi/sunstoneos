# GPU runtime measurements

These measurements compare the GPU service worker at the integration base
(commit `c45a76e`) with the current branch (commit `9a010a6`). They check the
current sources against the budgets in [gpu-runtime.md](gpu-runtime.md). All
values come from host models; no board was used.

Of that design, only worker pacing is implemented: the 200 µs burst, the
in-step waits of up to 50 µs in start, stop and power sequencing, the single
worker wait with a 4 ms bound while a job is in flight, and the 1 ms CPU-run
bound. Concurrent sessions, completion messages, range classes, reset, park,
firmware loading and GPU MMU changes are not implemented. Their budget rows
keep their baseline values at both commits, and the tables below mark them as
not implemented.

## Method

- **Sources and builds.** Each commit was exported with `git archive` and
  configured with the shared configuration script. The GPU service, power,
  compute, interrupt, free-list and memory models and the
  `sunstone-a733-gpu-render` image were built for each commit under
  `~/.cache/sunstoneos/build/gpu-runtime-measure/`.
- **Service and power models.** `checks/pvr-gpu-service.py` ran cases 1 to 3
  of the service model and cases 1, 5 and 6 of the asynchronous power model
  in QEMU. The current harness prints worker counters: timed waits begun and
  expired, tick blocks and the longest CPU run. The base worker has no such
  counters. A copy of the base harness received only the observation code of
  the current harness: a count of scheduler ticks with at least one worker
  callback, and a phase observer that sends no requests. In the base worker
  every wait lasts one tick, so ticks with worker activity equal its timed
  wake-ups.
- **Compute model.** `checks/pvr-compute-service.py` ran cases 1 to 11 and 15
  to 21 for each commit. Case 7 of the base harness received the same job
  window as the current harness: 50 ticks with a job waiting for its fence,
  then completion without client requests. A further copy of both harnesses
  recorded the scheduler tick at each admission of a COMPUTE or COMPUTE_INPUT
  request and the tick at its kick.
- **Power controller.** `checks/a733-gpu-power.c` was compiled for the host
  with each commit's controller, with a counter added to the poll loop of the
  first power-on and first power-off.
- **Static counts.** `llvm-objdump` disassembled the render image of each
  commit; `llvm-size` and `llvm-nm` gave section and symbol sizes.

The models answer every hardware wait at once, so tick figures measure
pacing overhead, not GPU time. Logs, harness copies and executable hashes are
in `~/.cache/sunstoneos/build/gpu-runtime-measure/runs/`.

## Worker pacing

| Metric | Base | Current | Target | Within target |
| --- | --- | --- | --- | --- |
| Worker timed wake-ups during a job, no interrupt delivery | 50 in 50 ticks (1,000 per second) | 12 in 50 ticks (240 per second) | 250 per second without qualified SPI 63 | Yes |
| Worker timed wake-ups, ready and idle, no interrupt callbacks | 200 in 200 ticks | 4 in 200 ticks | Park after 50 ms (not implemented) | Interim |
| Ticks from OPEN reply to READY, synchronous power | 5 | 2, with 0 timed waits | 0 timed waits; 1 wake-up | Yes |
| Ticks from OPEN reply to READY, power controller | 34 | 2, with 0 timed waits | 0 timed waits; 1 wake-up | Yes |
| Ticks from CLOSE reply to STOPPED, power controller | 10 | 1, with 0 timed waits | 0 timed waits; 1 wake-up | Yes |
| Power controller polls, power-on; power-off | 32; 11 | 1; 1 | 1 wake-up each | Yes |
| Power controller callbacks, first power-on | 91 | 62 | Not set | — |
| Ticks from compute OPEN to READY | 13 to 14 | 10 to 13, with 0 timed waits | Not set | — |
| Timed waits or tick blocks from compute admission to kick | Not countable at base | 0 at every kick | 0 | Yes |
| Scheduler ticks from compute admission to kick | 3 | 3 | At most one context switch | See note |
| Ticks from fence write to COMPLETE, no client requests | 1 | 2 | 4 ms bound without interrupt delivery | Yes |
| Longest CPU run outside the ring writes before a kick | Not countable at base | 1,201 to 1,775 µs | 1 ms, checked after each burst | See note |
| Tick blocks per compute case | Not countable at base | 5 to 6, all during OPEN | Not set | — |

Admission to kick took 2,854 to 3,125 µs of emulator time in the current
compute model and crosses three scheduler ticks in both commits. The current
worker spends that time computing; its counters show no timed wait or tick
block in that window. The base harness has no counter to separate CPU time
from waits in the same window, so the tick count is the only comparable
figure.

The CPU-run bound is checked between steps, after each burst. A run can
therefore exceed 1 ms by one 200 µs burst, its last step and the time that
tasks of the worker's priority use after each yield. The runtime design and
[gpu-service.md](../gpu-service.md) state this.

Completion without interrupt delivery takes up to 4 ms instead of one tick.
The design trades this for 1,000 fewer timed wake-ups per second at
`PVR_GPU_SERVICE_JOB_POLL_US` and removes the bound once SPI 63 delivery is
qualified on the board.

## Budget rows not implemented

These rows keep the baseline given in the design, unchanged between the commits: no barrier, cache maintenance,
copy or translation code changed. The render image of each commit contains
303 `DSB`, 9 `DMB`, 132 `ISB`, 4 `DC`, 2 `AT` instructions and 220 calls to
`memcpy`.

| Metric | Base and current | Target |
| --- | --- | --- |
| Jobs in flight | 1 | Render 1 + compute 2 |
| Client wake-ups per compute job | 1 per tick + 32 | 2 |
| `DC` lines per park and unpark | 2,176 + 2,176 | 2,176 + 0 |
| Queue round trips, 3,968-byte result | 31 | 0 beyond completion |
| Copies per input byte; per result byte | 2; at least 5 | 0 for EL1 clients, 1 for EL0 |
| Copies per rendered frame byte | 2 | 0 for scanout |
| Compute job cache work | 132 or 192 lines; 256- or 4,096-byte fill | Written input / 64 + read output / 64; no fill |
| `DSB SY` per submit; per FWCCB drain | 16 transport + 2 per cache call; 11n + 5 | 5; 2 |
| `AT` per register read | 5 | 0 |
| GPU MMU leaf entries, 4 MiB buffer | 1,024 | 2 |
| Hang detection | None | `max_job_us` + 4 ms |
| Firmware header decodes | 1 per call at 5 sites | 1 per boot |
| Firmware hashes | 0 | 1 per boot + 1 per reset |

Unpark wall-clock time and the free-extent check need the board or park and
are not measured.

## Image size and data

| Item | Base | Current | Change |
| --- | --- | --- | --- |
| `sunstone-a733-gpu-render` text | 462,640 B | 464,236 B | +1,596 B |
| `sunstone-a733-gpu-render` bss | 823,153,872 B | 823,154,000 B | +128 B |
| `gpu-service-model` text | 280,376 B | 283,384 B | +3,008 B |
| `gpu-compute-service-model` text | 318,316 B | 321,612 B | +3,296 B |

In the render image the worker grows by 612 bytes, `a733_gpu_power_poll` by
376, `pvr_gpu_start_poll` by 312 and `pvr_gpu_stop_poll` by 280. The growth
holds the in-step clock waits, the time-bounded burst and the worker's pacing
counters that the design adds. The bss growth is those counters in the
service object. The model executables also grow by their added harness code.
