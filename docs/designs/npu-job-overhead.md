# Removing the fixed CPU cost of an NPU job, and the default NPU operating point

Status: **implemented; not run on a board.** The ShuffleNetV2 run of
`board-session-20261025-12/port-3-3` ([full-model design](npu-full-model.md))
spent 56 ms of CPU time before each job's doorbell, 13–14 µs between the
counter start and the doorbell, and about 17 µs between the interrupt and the
recorded completion. This design removes that work as the vendor driver does,
and makes the NPU run by default at the vendor's operating point for the
chip's speed bin.

## Where the 56 ms goes

The board records place the 56 ms between the entry of the job publication
and the counter start: 55.8–56.1 ms in all four jobs, the same at 492 and
852 MHz, so CPU work and not the NPU. The source shows what that publication
did for every job:

- `unchanged()` hashed the descriptors and coefficients (917,504 bytes) and
  the job command stream, and scanned the 512 KiB command buffer's padding
  (`drivers/npu/npu_convolution.c`);
- `a733_npu_memory_publish` translated every page of every buffer with `AT`
  (1,065 translations) and cleaned or cleaned and invalidated the input, the
  1,093,632-byte pool and the output, 19,520 cache lines
  (`drivers/npu/a733_npu_memory.c`).

`checks/npu-job-cost.py` runs the image's own constructor, SUBMIT and job
callbacks in Unicorn and counts them. On the set 16 image the job publication
executes 49.6 M instructions, 920,616 bytes hashed, besides 1,065 translations
and 19,520 cache lines. Its hashing loop is about 98% of those instructions;
at the board's 56 ms that is about 1.1 ns per instruction. The SUBMIT send
runs 368,016 instructions (the pool clear and the input copy), which matches
its measured 405 µs at the same rate.

## Changes

1. **Prepared once, at open.** The vendor prepares a network once and, per
   run, flushes only the input and invalidates only the output
   (`examples/vpm_run/vpm_run.c:2628`, `:2672-2704`; `vip_lite.h:600-601`).
   A job publication now hashes nothing: the model copy, descriptors,
   coefficients and both command streams are checked at the bootstrap
   publication and again at CLOSE, which refuses a changed byte and retains
   the session. Around a job, only the READ and WRITE buffers are translated
   and maintained. A new buffer role, SCRATCH, is device-private memory that
   the bootstrap publication cleans and invalidates once; the finite image's
   pool takes it and is cleared once, at preparation. The retained runtime
   still clears its pool for every job.
2. **Plain register access.** The vendor reads and writes registers with
   `readl` and `writel` (`vip2/os/linux/vip_drv_os.c:383-430`). The adapter
   now checks the register page's mapping once, at initialization, and each
   access is a plain device access with `readl`/`writel` ordering: DMB OSHLD
   after a read, DMB OSHST before a write (`include/a733_mmio.h`). The
   controller's DSB SY stays before the doorbell and before output is read.
3. **Two-write trigger.** The vendor starts a job with `+0x654`, a barrier and
   `+0x3A4` (`vip2/vip_drv_hardware.c:882-893`). The configuration, idle and
   event checks run before the bootstrap, at every completion (idle) and at
   CLOSE, not before each trigger.
4. **One event read at completion.** The vendor's handler reads `+0x010` once
   (`vip_drv_device_driver.c:382`). Completion now reads the event word once
   and the idle word once; the configuration check moves to CLOSE. An event
   raised after that read stays pending: the interrupt profile samples it
   between jobs, the next completion attributes it to that job, and CLOSE
   refuses it. A second completion event for one job is not expected from
   the command stream, which raises one event at its end.

## Measurements in the models

| Quantity, per ShuffleNetV2 job | Before | After |
| --- | --- | --- |
| SUBMIT send instructions | 368,016 | 94,578 |
| Job publication instructions | 49,620,091 | 105,809 |
| Bytes hashed per job | 920,616 | 0 |
| Address translations, publication + finish | 1,065 + 1,065 | 39 + 39 |
| Cache lines, publication + finish | 19,520 + 17,152 | 2,432 + 64 |
| Native register read | 74 instructions, 1 `AT`, 1 DSB SY | 31 instructions, no `AT`, 1 DMB |
| Register accesses, job publication to doorbell (QEMU model) | 9 | 4 |
| Register accesses, doorbell to finish, one completion poll | 8 | 4 |
| Client SUBMIT to DONE reply, convolution QEMU model | 522–566 µs | 182–221 µs |

The first six rows come from `checks/npu-job-cost.py` on the set 16 image and
on this branch's image; the access counts include the two cycle-counter reads
on each side. CLOSE now hashes about 2 MB, about 110 M instructions or
roughly 0.12 s at the board's rate, once per session.

## Board predictions

| Phase, per job | Set 16 | Predicted |
| --- | --- | --- |
| Submit | 405–411 µs | about 0.1 ms |
| Publication to counter start | 55.8–56.1 ms | 0.15–0.6 ms, central 0.3 ms |
| Counter start to doorbell | 13.1–14.1 µs | 1.5–4.5 µs, central 3 µs |
| Interrupt to completion | 16.8–17.7 µs | 7.5–10 µs, central 9 µs |
| Doorbell to interrupt, ShuffleNetV2 | 2.91 ms at 852 MHz, 3.69 ms at 492 MHz | unchanged |

The publication prediction applies the measured 1.1 ns per instruction to
105,809 instructions and adds the 39 translations and the 2,432 lines of the
input clean. The two register predictions bracket whether the 1.6 µs per
access of set 13 was mostly the translation and DSB SY or mostly the bus: two
writes remain before the doorbell and two reads at completion, besides about
7 µs of worker wake and counter reads.

## The clock-independent 1.85 ms

Set 16's two rates give T(f) = C/f + M with C ≈ 902,000 NPU cycles and
M ≈ 1.85 ms per ShuffleNetV2 inference. M does not shrink with the NPU clock.
Three measurements can attribute it:

- **Bandwidth counters.** The vendor's bandwidth profiling reads total read
  and write counters at `+0x040` and `+0x044` around a job, and on-chip
  buffer counters at `+0x5C0` and `+0x5D0`
  (`vip2/task/vip_drv_task_debug.c:270-277`); its hang dump also names read
  and write burst, request and outstanding-transaction counters at
  `+0x058`–`+0x060` and `+0x43C`–`+0x444` (`vip_drv_debug.c:551-560`).
  Logged beside the cycle counters, bytes moved divided by M gives the
  achieved DRAM bandwidth: near the bus limit points to DRAM bandwidth; far
  below it, with many outstanding reads, points to latency.
- **NPU MMU.** The tables map 4 KiB pages (785 for this graph). Mapping the
  same buffers with larger pages and comparing M isolates the cost of the
  NPU's table walks.
- **MBUS clock.** The image fixes MBUS at 600 MHz (`CCU+0x588`). A run with
  MBUS at another rate, or with the NPU clock fixed and the DRAM clock
  changed, shows whether M follows the memory fabric.

## Default operating point

The standing policy runs each block by default at the highest operating point
the vendor uses for the chip's speed bin. For the NPU the vendor driver caps
the rate at 1,008 MHz (`npu-vfgear` 0, `vip_drv_device_platform.c:655-686`),
lists only the table rows that have the bin's column (`:736-790`) and keeps
the highest such row at or below the cap (`:703-734`). From the NPU table
(`sun60iw2p1-cpu-vf.dtsi` lines 2036–2140, 24 MHz columns):

| Speed bin, SID code | Vendor point |
| --- | --- |
| vf0000 (0x00), vf0200 (0x02), vf0401 (0x14) | 1,008 MHz at 960 mV |
| vf0300 (0x03), vf0501 (0x15) | 1,008 MHz at 900 mV |
| vf0201 (0x12), vf0400 (0x04), vf0500 (0x05) | 852 MHz at 800 mV |
| vf0100 (0x01), vf0101 (0x11), vf0600 (0x06) | no row |

Port 3-3 reads SID `0x0300624c` = `0x496` and port 3-1 reads `0x495`
(`board-session-20261025-4/port-3-1/bb9-read`): both are code 4, vf0400, whose
column has no 1,008 MHz row. The vendor driver therefore runs both boards at
852 MHz with DCDC2 untouched at 800 mV, and 1,008 MHz is outside the vendor
table for them.

The register pass now selects this point by default. For vf0201, vf0400 and
vf0500 it runs 852 MHz at the 800 mV the power-domain owner verifies. It
refuses every other bin before any write: their point needs a DCDC2 write,
which no image yet performs, or the bin has no row. 492 MHz and an explicit
852 MHz stay as optional modes (`a733_npu_startup_rate`;
`apps/npu-finite/build.py --rate-492`, `--rate-852`).

## Checks

`~/.cache/sunstoneos/evidence/npu-job-overhead-1025/scripts/` holds the focused,
interrupt and ShuffleNetV2 check scripts, which build the default image and
the 492 MHz mode. The controller host check covers the two-write trigger, the
pending event and the configuration change found at CLOSE; the convolution
model and `checks/npu-nn-pool-native.py` cover the mutation refused at CLOSE;
the startup, clock-reference, routing and supervised models read the boards'
speed-bin word.

## Limits

- A change to a device-read byte between OPEN and CLOSE runs in the jobs in
  between; CLOSE finds it after the fact.
- The per-access split between bus and barrier cost is not yet measured; the
  doorbell timestamp of the next run measures the remaining two writes.
- No speed bin that needs a DCDC2 change can run; both boards are vf0400.
