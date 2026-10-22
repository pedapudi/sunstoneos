# Measurements: mimus linear layers on the CPU or GPU in the OS image

These measurements check the [OS image's placement of mimus linear
layers](../recognition-dispatch.md) against the budgets in its
[design](recognition-dispatch.md). They compare the integration base
(`audit/integration`, commit `c45a76e`) with the commit that last changed this
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
  AArch64 except the patched recognizer, which stopped at its assertion that
  mimus's arena is at most 12 MiB: `apps/mimus/recorded.h` declares 16 MiB.
  With a copy of that header declaring 12 MiB placed in the build directory,
  the recognizer compiled and linking stopped at the five functions that other
  owners provide. Linking the same objects with empty definitions of those
  five functions gave an image for the link check only.
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
| Linear stage, regression bound | 14.894 s | At most 0.82 s on the A55 pair and 0.42 s on the A76 pair, restated from 0.79 s and 0.39 s | 0.812 s and 0.414 s in the time model | Within the restated bound, which awaits the panel's acceptance; 2.8% and 6.2% over the first bound |
| Linear stage, goal | 14.894 s | 0.15 s | Not reached by any modeled lever; best 0.163 s on the A76 pair | No |
| Exact sums against a CPU-only reference | CPU path | Equal, nominal and with each failure | 0 of 12,923,134 outputs differ on each table; nominal, refused job, refused OPEN, missing tag | Yes for sums; float read-out and text need the GPU kernel's check |
| Calls on the GPU; jobs | 0; 0 | 204; 204 | 204; 204 on both pairs | Yes |
| Table rows above the worker's job bound | None | 0 | 0 of 586 rows on each table; every placed row is the bound rounded down to 32 | Yes |
| Subsampling jobs; padded rows | None | 20; 8 | 20; 8 | Yes |
| Placement work per call | None | 2 loads, 1 compare | `adrp`, `ldr`, `cmp`, `b.hs`, `adrp`, `add`, `ldr`, `ret`: 2 loads, 1 compare | Yes |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | The dispatcher copies nothing; the hook's copies need the GPU kernel's check | Pending |
| Subsampling codes written; lines cleaned | 7,032,960; 0 | 7,032,960; 219,780 (8.9 ms at 494 MHz) | 7,032,960; 219,780 (8.9 ms) | Yes |
| Recognizer wake-ups; IPIs; tick waits per job | None | 1; 0; 0 with INTID 95, else up to one 4 ms wait | Recognizer, worker and INTID 95 on one core; 204 × 4 ms = 0.82 s at most without INTID 95 | Pending the GPU runtime |
| Arena peak | 1,761,840 bytes | At most 2,784,048 bytes | Needs the mimus port's 224-row producer | Pending |
| mimus arena within the 12 MiB span | 16 MiB | At most 12 MiB, asserted at compile time | The assertion compiles with 12 MiB and fails with 16 MiB or a base moved by 4 KiB; `recorded.h` still declares 16 MiB | Pending the mimus port |
| GPU idle per subsampling gap | None | At most 1 ms on the A55 pair, 0.25 ms on the A76 pair | Clean of one block 0.449 ms and 0.111 ms; production time needs the mimus port | Pending |

**Jobs.** Every bench call fits one job under the worker's bound, so the
204 placed calls are 204 jobs. The C hook model and the Python time model
both count 204.

**Linear stage.** On the A55 pair the modeled 0.812 s is 0.579 s of GPU
compute, 0.157 s of read-out shared by both cores, 0.064 s of cache
maintenance (596,989 cleaned and 988,060 invalidated lines) and 0.012 s of
submit, wake and hand-off. With one job in flight nothing but the CPU
channels of a split call overlaps a job. On the A76 pair 0.366 s of GPU
compute for 62.9% of the MACs dominates the 0.414 s. The first bound of
0.79 s and 0.39 s assumed overlapped two-job calls; under that model the
best split points gave 0.813 s and 0.407 s, so no placement recovers it. No
regression against the CPU path exists.

**Table.** On the A55 pair every tensor is GPU-only. A channel split moves
read-out that both cores share in a GPU-only call onto the recognizer core:
the feed-forward first linear takes 6.25 ms GPU-only and 7.95 ms with
16 CPU channels.

## Other host results

| Item | Base | Current |
| --- | --- | --- |
| Default OS images with equal section sizes and loadable bytes, apart from the 32-byte build identity | 4 of 4 | 4 of 4 |
| Resident image `.text` / `.rodata` / `.data` / `.bss` | 845,088 / 41,321 / 3,168 / 1,481,568 B | Same |
| Translations in `mimus_board_pins` for the arena | 4,096 | 4,096 without the option; 5,120 with it |
| Read-only rule refusals: one writable, unreadable or Device page at the first, middle or last of 1,024 pages | None | All 9 cases refused |
| Job with a missing tag | None | The rest of the call and every later call on the CPU, exact sums, no `board_fail` |
| Supervision: reply never comes | None | Watchdog expiry 6.02 s after the wait begins, 4 s after the last renewal |
| Supervision: two jobs at the worker's deadline | None | Renewal continues |
| Longest input, 820 frames | None | 277 calls, 408 jobs on the A55 pair with up to 4 per call; 311 on the A76 pair with up to 3; no zero-row job and no job above the worker's bound |
| Configuration with the option | None | Refused without the resident lifetime or either constant; configures with both, with and without the microphone |
| Link check with the option | None | INTIDs 71 and 95, entries `mimus`, `thermal`, `gpu-linear`; refuses a recognition core of 6 with INTID 95 on core 2 |

## Conclusion

Every count, the lookup cost, the job bound and the exact-sum comparison
meet their budgets. The modeled linear stage meets the restated regression
bound and misses the first one by 2.8% and 6.2%; the restated bound needs the
performance panel's acceptance. Against the 14.894 s CPU baseline on one A55
it is 18 and 36 times shorter. Default images are unchanged. Copies, the
arena peak, the subsampling gap and the wake-up counts need the GPU kernel,
the mimus port and the GPU runtime before they can be measured.
