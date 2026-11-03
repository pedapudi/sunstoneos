# Removing the fixed CPU cost of an NPU job, and the default NPU operating point

Status: **implemented and host-tested**; the per-phase job times have no board result of their own (open work 1). The default operating point is **qualified on the board** at 852 MHz and 800 mV on both boards.

A job's CPU work is limited to what the vendor driver does per run, and the NPU runs by default at the vendor's operating point for the chip's speed bin. The ShuffleNetV2 run in the [full-model design](npu-full-model.md) showed 56 ms of CPU time before each job's doorbell, 13-14 µs between counter start and doorbell, and about 17 µs between interrupt and recorded completion ([records](../evidence/npu-job-overhead.jsonl), `npu-job-overhead-cost-breakdown`).

## Mechanism

1. **Prepared once, at open.** The vendor prepares a network once and, per run, flushes only the input and invalidates only the output (`examples/vpm_run/vpm_run.c`, `vip_lite.h`). A job publication hashes nothing: the model copy, descriptors, coefficients and both command streams are checked at the bootstrap publication and again at CLOSE, which refuses a changed byte and retains the session (`unchanged()` in `drivers/npu/npu_convolution.c`). Around a job, only the READ and WRITE buffers are translated and maintained. The buffer role SCRATCH is device-private memory that the bootstrap publication cleans and invalidates once; the finite image's pool takes it and is cleared once, at preparation. The retained runtime clears its pool for every job.
2. **Plain register access.** The vendor reads and writes registers with `readl` and `writel`. The adapter checks the register page's mapping once, at initialization, and each access is a plain device access with `readl`/`writel` ordering: DMB OSHLD after a read, DMB OSHST before a write (`include/a733_mmio.h`). The controller's DSB SY stays before the doorbell and before output is read.
3. **Two-write trigger.** The vendor starts a job with `+0x654`, a barrier and `+0x3A4`. The configuration, idle and event checks run before the bootstrap, at every completion (idle) and at CLOSE, not before each trigger.
4. **One event read at completion.** The vendor's handler reads `+0x010` once. Completion reads the event word once and the idle word once; the configuration check is at CLOSE. An event raised after that read stays pending: the interrupt profile samples it between jobs, the next completion attributes it to that job, and CLOSE refuses it. The command stream raises one event at its end, so a second completion event for one job is not expected.

CLOSE hashes about 2 MB, about 110 M instructions or roughly 0.12 s at the board's rate, once per session.

## Default operating point

The standing policy runs each block by default at the highest operating point the vendor uses for the chip's speed bin. The vendor driver caps the NPU rate at 1,008 MHz (`npu-vfgear` 0), lists only the table rows that have the bin's column and keeps the highest such row at or below the cap. Port 3-3 reads speed-bin word `0x496` and port 3-1 reads `0x495`: both are code 4 (vf0400), whose column has no 1,008 MHz row. Both boards therefore run at 852 MHz with DCDC2 untouched at 800 mV. The full table of bins and points is in the [record](../evidence/npu-job-overhead.jsonl), `npu-job-overhead-speed-bin`.

The register pass selects this point by default (`a733_npu_startup_rate`; `apps/npu-finite/build.py --rate-492`, `--rate-852` for the optional modes). For vf0201, vf0400 and vf0500 it runs 852 MHz at the 800 mV the power-domain owner verifies. It refuses every other bin before any write: their point needs a DCDC2 write, which no image performs, or the bin has no row.

## Figures

| Quantity, per ShuffleNetV2 job | Host model before | Host model after | Record |
| --- | --- | --- | --- |
| SUBMIT send instructions | 368,016 | 94,578 | `npu-job-overhead-model-after` |
| Job publication instructions | 49,620,091 | 105,809 | same |
| Bytes hashed per job | 920,616 | 0 | same |
| Cache lines, publication + finish | 19,520 + 17,152 | 2,432 + 64 | same |
| Client SUBMIT to DONE, convolution QEMU model | 522-566 µs | 182-221 µs | same |

The host figures come from `checks/npu-job-cost.py`, which runs the image's own constructor, SUBMIT and job callbacks in Unicorn and counts instructions. Doorbell to interrupt for ShuffleNetV2 is 2.91 ms at 852 MHz and 3.69 ms at 492 MHz.

## Limits and open work

- A change to a device-read byte between OPEN and CLOSE runs in the jobs in between; CLOSE finds it after the fact.
- No speed bin that needs a DCDC2 change can run; both boards are vf0400.
- The OS NPU profile and the NPU board image share the startup, controller and adapter; neither has a board run of its own with plain register access, the two-write trigger and the single event read.

Open work:

1. Board run of the per-phase predictions (submit about 0.1 ms; publication to counter start 0.15-0.6 ms; counter start to doorbell 1.5-4.5 µs; interrupt to completion 7.5-10 µs), `npu-job-overhead-board-prediction`.
2. Attribute the clock-independent 1.85 ms of a ShuffleNetV2 inference with bandwidth counters, larger NPU MMU pages and a changed MBUS clock, `npu-job-overhead-clock-independent-term`.
