# Two interrupt-completed NPU starts in one board lifetime

Status: **runbook only**; nothing here has run. A set frozen from it needs the
root review of the interrupt host and a root approval of each start's
identities and grants before its claim.

## What the run measures

The interrupt-driven finite image ([design](../designs/npu-irq-completion.md))
runs the convolution twice per start, completes each job on interrupt 97 and
logs each job's time with the NPU's cycle counters and the system counter.
One cold lifetime on port 3-3, whose speed bin admits 852 MHz, runs two
starts:

- **Start 1, 492 MHz, cold.** `apps/npu-finite/build.py --interrupt`. The
  image enables PLL_NPU, routes the shared SRAM to the NPU, completes both jobs
  on interrupt, stops, routes the SRAM back and returns to the ROM.
- **Start 2, 852 MHz, rate change.** `build.py --interrupt --rate-852`. Its
  register pass finds PLL_NPU locked at 492 MHz, as start 1 left it, and
  retunes it to 852 MHz at the unchanged 800 mV.

Start 2 repeats the qualified path of `board-session-20261025-7/port-3-3`,
whose third start retuned a 492 MHz PLL to 852 MHz, so a failure there is
attributable to the interrupt path or the rate.

## Predictions

- Every start ends with status 0, a normal ROM return and verified DRAM
  restoration; both outputs match the reference; the outputs of both starts
  are byte-identical; the log holds three notified completions and the
  service's stopped receipt.
- `npu_result.timing`: startup entry to READY about 4.0 ms at both rates, as
  in the polled lifetime. Every job has a completion time after its
  publication (`job_us` not null).
- `npu_result.timing.counters`: per job, publication to delivery
  (`start_to_irq_us`) is the NPU's compute plus tens of microseconds, under
  100 µs at 492 MHz; delivery precedes the completion observation.
- If the job is compute bound, `active_cycles` agree within 10% between the
  starts, and start 2's `start_to_irq_us` is about 0.58 of start 1's.

## Order

The steps follow `docs/board-runbooks/npu-starts.md` with two starts, the
interrupt images and the interrupt host in place of the polled ones.

| Step | Records | Action | Pass |
| ---: | --- | --- | --- |
| 0 | — | The user removes and restores the board's power; open a new session directory | The A733 ROM answers on a new FEL instance |
| 1 | `is-ddr` | Cold DDR start | 12,288 MiB, stage 5 |
| 2 | `is-read-0`, `is-irq97-1`, `is-cache-off-1`, `is-survey-1`, `is-dcdc2-1`, `is-cache-off-2`, `is-survey-2` | Cold state read, IRQ97 capture, cache-off and survey, DCDC2 snapshot, cache-off and survey | Cold predictions match; IRQ97 disabled, inactive and not pending; DCDC2 at 800 mV |
| 3 | `is-npu-1` | Root finalization of package 1, then start 1 with the interrupt host | Exit 0; outputs match; timing as predicted |
| 4 | `is-read-1`, `is-irq97-2`, `is-cache-off-3`, `is-survey-3`, `is-dcdc2-2`, `is-cache-off-4`, `is-survey-4` | State read with the 492 MHz stopped-state predictions, then the step 2 observations | Stopped state matches; IRQ97 as in step 2 |
| 5 | `is-npu-2` | Root finalization of package 2 with the 852 MHz image, then start 2 | Exit 0; outputs match; timing as predicted |
| 6 | `is-read-2` | State read with the 852 MHz stopped-state predictions | Every checked word matches |

## Stop conditions and recovery

- Stop at the first nonzero exit; do not retry. Keep the port lock and the
  claim until the user's power removal.
- A missing interrupt ends the job at its 1 s deadline as a retained timeout;
  the image parks and the board needs a power removal.
- A failure inside the routing window parks the CPU at EL3 with the SRAM
  routed to the NPU; the power removal's cold ROM entry routes it back.
- The host refuses to launch, and the monitor refuses entry, when the
  watchdog is enabled.
