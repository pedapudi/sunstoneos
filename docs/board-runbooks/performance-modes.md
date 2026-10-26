# High and low performance modes in one OS lifetime

Status: **runbook only**; nothing here has run, and no set is frozen. A set
frozen from it needs the approval records below and a root approval of each
image's identity before its claim.

## What the run tests

The supervised OS image with the performance-mode service
([design](../designs/performance-modes.md)) boots in high, runs its NPU jobs,
then switches to low and back to high while each block's rate and voltage are
read back, and runs one recorded-audio recognition in each mode. Every switch
goes through USB0 operation 8 from the supervised launcher; the device is not
released between them.

## Prerequisites

1. **CPU top points.** Steps 1 to 7 of
   [the CPU top-point plan](../designs/cpu-top-opp.md#board-steps-on-port-3-3)
   have passed on port 3-3, one step at a time. They are the CPU-B relock, the
   CPU-L park, the first DCDC3 write, the top points of CPU-B and CPU-L with
   the DSU, the all-core workload at the top points, and B1 with the OS image
   at the top points. This run makes the same changes in one lifetime and in
   both directions, so it adds nothing that those steps have not each shown.
2. **OS NPU profile.** One supervised run of the OS NPU profile has returned
   a matched NPU report (operation 6), so the NPU session itself is known to
   work in the OS image.
3. **Merged sources.** The CPU top-point branch and this branch are merged,
   and the integration gate and `checks/perf-mode.py` pass on the merged
   revision.

## Images and records

- **Image A** (this lifetime): the supervised OS image with
  `A733_A76_ADMITTED`, `SUNSTONE_A733_OS_CPU_CLOCK`, `SUNSTONE_A733_OS_PERF`
  (boot mode high) and `SUNSTONE_A733_OS_NPU` with the default catalogue
  entry and two jobs. Its manifest must record `perf_modes: true` and
  `perf_boot_mode: "high"` and `perf_operation: 8`.
- **Session.** The CPU grant names both clusters, 1794 MHz for CPU-L and
  2002 MHz for CPU-B, and 64 changes. Its approval record has schema
  `a733-os-perf-modes-approval-v1`, names port 3-3, the image's SHA-256, the
  session's epoch, generation and lease, the user's CPU, GPU and NPU clock and
  voltage approvals and their dates, and cites port 3-3's passed B1 record.
  The NPU extension carries the authority records of the OS NPU profile.
- **Launch.** `tools/check-a733-os-supervised.py ... --generations 3
  --perf-sequence high,low,high --fault-report`.

## Sequence and predictions

One cold lifetime on port 3-3: power removal, the cold DDR start, then
image A.

| Step | What runs | Prediction |
| --- | --- | --- |
| Boot | Thermal start, CPU inspection, the boot readback and the boot switch to high; the NPU starts at 852 MHz and runs its two jobs | `CLOCK` line before recognition 1: CPU-L 1794 MHz, CPU-B 2002 MHz, DSU 1274 MHz; report: mode high, 61 changes left; CPU-L PLL `0xf8804500`, CPU-B `0xf8804d00`, DSU `0xf8803100`; DCDC5 selector `0xb2` (1000 mV), DCDC3 `0xb7` (1050 mV) |
| Switch 1, high | Readback only: the image is already in high | Accepted as command 1, done, no CPU request, 61 changes left; NPU 852 MHz and stopped once its jobs ended; GPU 0 MHz and stopped, DCDC4 800 mV (no GPU image runs on port 3-3) |
| Recognition 1 | Recorded audio on the A76 pair | Transcript matches the reference; time as the CPU top-point step 7 measured |
| Switch 2, low | The stopped NPU to 492 MHz, the DSU to 910 MHz, CPU-L to 1196 MHz then DCDC5 to 800 mV, CPU-B to 1404 MHz then DCDC3 to 800 mV | Done, mode low, 58 changes left; NPU PLL `0xf8305100` (or with bit 24 set); DSU `0xf8802300`, CPU-L `0xf8802e00`, CPU-B `0xf8803600`; both selectors `0x9e`; the report's elapsed time under 50 ms |
| Recognition 2 | Recorded audio at low | Same transcript; time 1.2 to 1.45 times recognition 1, at most the ratio 2002 ÷ 1404 |
| Switch 3, high | The NPU to 852 MHz, DCDC3 to 1050 mV then CPU-B to 2002 MHz, DCDC5 to 1000 mV then CPU-L to 1794 MHz, then the DSU to 1274 MHz | Done, mode high, 55 changes left; the words of the boot row |
| Recognition 3 | Recorded audio at high | Same transcript; time within 5% of recognition 1 |
| Final status | One readback | Mode high; every owned block read; thermal NORMAL throughout, hottest sensor below 90 °C |

The NPU report (operation 6) after the capture is matched, with both jobs
completed at 852 MHz. The fault report before the capture is that of the
earlier lifetime.

**What each departure would mean.**

- **A switch refused as thermal or as busy**: the image is working as
  designed; the record names the reason. Stop the sequence and record the
  temperatures; do not repeat the run in this lifetime.
- **A switch stopped at a CPU domain with the step expired or refused**: the
  CPU clock driver's own checks refused the change; the readback shows the
  domain's state. Compare it with the matching CPU top-point step.
- **No answer after a switch**: a CPU or NPU change failed with an unknown
  state and the board failed. Remove power. The step's block, from the last
  report, names the change to diff against the reference.
- **Recognition 2 not slower**: the CPU-B rate did not change, or the
  recognition is limited by something other than the A76 clock; the CLOCK
  lines and the report decide which.

## NPU work at low

The OS NPU service runs one session per lifetime, before the first switch, so
image A runs NPU jobs only at 852 MHz. NPU work at 492 MHz needs a second
lifetime with **image B**, built as image A with
`SUNSTONE_A733_OS_PERF_BOOT=low`. Its predictions: the boot switch lowers the
CPU domains to the low row's words, the NPU starts at 492 MHz and its report
is matched with the same output digest as image A's, and
`--perf-sequence low,high,low` gives the switch rows above in the other
order. This lifetime may follow the first only after the first passed.
