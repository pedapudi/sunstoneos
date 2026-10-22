# Measurements: mimus linear layers on the CPU or GPU in the OS image

These measurements check the [OS image's placement of mimus linear
layers](../recognition-dispatch.md) against the budgets in its
[design](recognition-dispatch.md). They compare the integration base
(`audit/integration`, commit `c45a76e`) with the commit that adds this
document. All values come from host builds, the host check
`checks/os-image-dispatch.py` and the time model in `apps/os/placement.py`;
no board or GPU was used.

## Method

- **Images.** The base commit was exported with `git archive` and both
  commits were configured with the shared configuration script and built with
  `-j 6` under `~/.cache/sunstoneos/build/recognition-dispatch/`. Four default
  OS variants were built at each commit: supervised, resident, resident with
  the I2S microphone and resident with the Bluetooth headset. `llvm-size-21
  -A` gave the section sizes and `llvm-objcopy-21 -O binary` the loadable
  bytes.
- **GPU option.** A resident build with `SUNSTONE_A733_OS_GPU_LINEAR`, both
  build constants and the bench models compiled every changed source for
  AArch64. Linking stopped at the five functions that other owners provide.
  Linking the same objects with empty definitions of those five functions gave
  an image for the link check only.
- **Dispatcher and time model.** `checks/os-image-dispatch.py` generated the
  A55-pair table at 494 MHz and the A76-pair table at 2,002 MHz from the bench
  models (`~/.cache/sunstoneos/inputs/mimus-bench`). It ran
  `checks/os-image-dispatch.c`, built with AddressSanitizer and
  UndefinedBehaviorSanitizer, over the bench recording's call list
  (222 encoder frames) with exact integer sums, and over the longest accepted
  input (820 frames) with output coverage only. It disassembled
  `os_dispatch_place` for AArch64 and ran the CMake gate cases against the
  resident base build's cache.

The base commit has no placement path, so its rows below are the CPU-only
image: the profile in [mimus.md](../mimus.md) or the design's stated
baseline.

## Results against the design's budgets

| Metric | Baseline | Target | Result | Within budget |
| --- | --- | --- | --- | --- |
| Linear stage, regression bound | 14.894 s | At most 0.79 s on the A55 pair and 0.39 s on the A76 pair | 0.814 s and 0.407 s in the time model | No; 3.0% and 4.4% over |
| Linear stage, goal | 14.894 s | 0.15 s with the levers in the design | Not reached by the model | Board item |
| Exact sums against a CPU-only reference | CPU path | Equal, nominal and with each failure | 0 of 12,923,134 outputs differ on each table; nominal, refused job, refused OPEN | Yes for sums; float read-out and text need the GPU kernel's check |
| Calls on the GPU; jobs | 0; 0 | 204; 382 | 204; 381 on both pairs | Yes; see below |
| Subsampling jobs; padded rows | None | 20; 8 | 20; 8 | Yes |
| Placement work per call | None | 2 loads, 1 compare | `adrp`, `ldr`, `cmp`, `b.hs`, `adrp`, `add`, `ldr`, `ret`: 2 loads, 1 compare | Yes |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | The dispatcher copies nothing; the hook's copies need the GPU kernel's check | Pending |
| Subsampling codes written; lines cleaned | 7,032,960; 0 | 7,032,960; 219,780 (8.9 ms at 494 MHz) | 7,032,960; 219,780 (8.9 ms) | Yes |
| Recognizer wake-ups; IPIs; tick waits per job | None | 1; 0; 0 with INTID 95, else up to one 4 ms wait | Recognizer, worker and INTID 95 on one core; 381 × 4 ms = 1.52 s at most without INTID 95 | Pending the GPU runtime |
| Arena peak | 1,761,840 bytes | At most 2,784,048 bytes | Needs the mimus port's 224-row producer | Pending |
| GPU idle per subsampling gap | None | At most 1 ms on the A55 pair, 0.25 ms on the A76 pair | Clean of one block 0.449 ms and 0.111 ms; production time needs the mimus port | Pending |

**Jobs.** The design derived 382 jobs as 399 − 17. The GPU kernel design
counted 399 jobs with 19 subsampling blocks of 240 rows, each split into two
jobs. With 20 blocks of 224 rows, one job each, the count is
399 − 38 + 20 = 381. The C hook model and the Python time model both count
381.

**Linear stage.** The modeled time is the GPU's 0.580 s, the exposed
read-out of each call's last job and per-job work. On the A55 pair the
exposed read-out is 0.183 s. The design used 0.155 s, half of the 0.311 s
read-out, but with jobs on 32-row boundaries a 222-row call splits into 96
and 126 rows. On the A76 pair the GPU branch dominates: 0.375 s of GPU work
for 63.6% of the MACs, plus cleans, completions and the last job's
read-out. No regression against the CPU path exists; the misses are against
the design's estimates.

**Table.** The design expected 48 CPU channels for the feed-forward first
linear and 64 for the output head on the A55 pair. The time model, which
includes the recognizer core's read-out as the design requires, chose 16 and
32. At 48 channels the recognizer core computes 3.35 ms of CPU channels
before it can read out the first job, and the call takes 7.36 ms against
6.16 ms at 16.

## Other host results

| Item | Base | Current |
| --- | --- | --- |
| Default OS images with equal section sizes and loadable bytes, apart from the 32-byte build identity | 4 of 4 | 4 of 4 |
| Resident image `.text` / `.rodata` / `.data` / `.bss` | 845,088 / 41,321 / 3,168 / 1,481,568 B | Same |
| Translations in `mimus_board_pins` for the arena | 4,096 | 4,096 without the option; 5,120 with it |
| Slot rule refusals: one writable, unreadable or Device page at the first, middle or last of 1,024 pages | None | 9 of 9 refused |
| Supervision: reply never comes | None | Watchdog expiry 6.02 s after the wait begins, 4 s after the last renewal |
| Supervision: two jobs at the worker's deadline | None | Renewal continues |
| Longest input, 820 frames | None | 277 calls, 455 jobs on the A55 pair with up to 3 per call; 454 on the A76 pair with up to 2; no zero-row job |
| Configuration with the option | None | Refused without the resident lifetime or either constant; configures with both, with and without the microphone |
| Link check with the option | None | INTIDs 71 and 95, entries `mimus`, `thermal`, `gpu-linear`; refuses a recognition core of 6 with INTID 95 on core 2 |

## Conclusion

Every count, the lookup cost and the exact-sum comparison meet their
budgets. The modeled linear stage exceeds the design's regression bounds by
3.0% and 4.4% for the two stated causes; against the 14.894 s CPU baseline
on one A55 it is 18 and 37 times shorter. Default images are unchanged. Copies, the arena peak, the
subsampling gap and the wake-up counts need the GPU kernel, the mimus port
and the GPU runtime before they can be measured.
