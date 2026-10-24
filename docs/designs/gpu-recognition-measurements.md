# Measurements: mimus linear layers on the GPU from the integrated OS image

These measurements check the [GPU recognition design](gpu-recognition.md)
against its budgets. They compare the integration base, commit `959bccd`
(`bringup` merged into the design commit), with the commit that last changed
this document. All values come from host builds, QEMU and host models; no
board or GPU was used.

## Method

- **Images.** The base commit was exported with `git archive`. Both trees
  were configured with the shared configuration script and built with `-j 6`
  under `~/.cache/sunstoneos/build/gpu-recognition/`. Four default OS
  variants were built at each commit: supervised, resident, resident with the
  I2S microphone and resident with the Bluetooth headset. `llvm-objcopy-21 -O
  binary` gave the loadable bytes and `llvm-size-21 -A` the section sizes. The
  GPU linear image was built at the change only, resident, with the pinned
  GPU firmware and the bench models (`~/.cache/sunstoneos/inputs/mimus-bench`),
  for the A76 pair, twice: as the tree builds it today, without the engine's
  hook header, and with a copy of `checks/gpu-recognition-hook.h` on the
  include path and the engine's `SetExactSumHook` replaced by a linker
  symbol, which is the image once the engine has its hook.
- **Session in QEMU.** `checks/pvr-mimus-service.py` built a two-core
  FreeRTOS image for QEMU's `virt` machine. It constructs the session with
  the image's `a733_gpu_reserved_prepare` and `a733_gpu_mimus_prepare` over
  the production pools, runs the kernel ring through
  `platform/a733/gpu_memory.c` and delivers completions on SPI 95 of QEMU's
  GIC through `gicv3_spi.c`. A software firmware model, reused from
  `checks/pvr-gpu-service.c`, checks each kick's 96-byte compute command and
  timeline UPDATE; a model task on the second core writes exact sums and tags
  after the duration `placement.py` models for each job on the A76 table.
  An EL0 client task runs OPEN and the bench recording's 204 jobs. Weights
  are synthetic bytes at the real tensor offsets, the output region stays
  writable at EL1 so the model can store into it, and GPU registers are model
  callbacks, whose accesses were counted and costed at two `DSB SY` each, as
  `gpu_backend.c` issues them. Counts come from traced runs with one
  instruction per translation block (`-d exec,nochain -dfilter` over all 154
  `dsb sy` sites and the counted function entries, `-icount shift=0`), split
  into per-job windows at a marker function, and from `--wrap` counters.
  The traced bench runs divide by the 204 jobs; their worker wake-ups use the
  instruction counter as the guest clock, and a run at real time is given
  beside them.
- **Engine contract.** `checks/gpu-recognition-engine.py` built the hook, the
  dispatcher and a model of the mimus engine for the host with
  AddressSanitizer and UndefinedBehaviorSanitizer, without the hook header
  and with it.
- **Dispatcher, mapping checks, pools and gate.** `checks/os-image-dispatch.py
  --configure-from` the resident build ran the placement tables, the C
  dispatcher model over the bench call list with exact sums and over the
  longest accepted input, the mapping-check, stop-detail and pool-replay
  cases, the recognizer patch chains and the CMake gate cases.
- **Time model.** `apps/os/placement.py --summary`, issue-bound and with
  `--load-cycles 200` and `400`.
- **Other GPU images.** The existing `pvr-gpu-service.py` (cases 1 to 6 on
  the service, native-memory and asynchronous-power models),
  `pvr-gpu-irq.py`, `pvr-gpu-freelist-service.py` and
  `pvr-compute-service.py` checks, and the GPU render image's link, at the
  change. `checks/device-memory-capacity.py` ran its QEMU golden comparison,
  including the read-only output view, and scanned the GPU linear build's
  images.

The base commit has no GPU path in the OS image, so its rows below are the
CPU-only image or the design's stated baseline.

## Results against the design's budgets

| Metric | Baseline | Target | Result | Within budget |
| --- | --- | --- | --- | --- |
| Default OS images, loadable bytes | — | Identical apart from the 32-byte build identity | 4 of 4 identical apart from the identity; section sizes equal (resident `.text` / `.rodata` 861,216 / 41,609 B at both commits) | Yes |
| Other images that link `drivers/gpu/` | — | Existing host checks pass; changes only at the service points | `pvr-gpu-service.py` cases 1 to 6, `pvr-gpu-irq.py` 1 and 2, `pvr-gpu-freelist-service.py` 1 to 4 and `pvr-compute-service.py` 1 to 3 pass; the GPU render image links. The service changes at the twelve points the design lists | Yes |
| GPU linear image | Stopped at the arena assertion | Links; link check passes; `.text` + `.rodata` at most 1.75 MiB | Links and passes the link check both ways. With the hook header 1,012,576 + 188,953 = 1,201,529 B (1.15 MiB); without it 1,011,657 B, because nothing of the session is prepared. The resident default image has 902,825 B | Yes |
| Translations in `mimus_board_pins`, arena and view | 5,120 | 18 | 18: 6 for the arena, 12 for the view (EL0 read, EL0 write and EL1 write on the first and last page of each block) | Yes |
| Slot write to kick in the session component | No path | 0 translations, hashes, allocations and tick waits; at most 13 compares | 0 translations (the window's 18 are all in `gpu_memory.c`); no hash, allocation or tick wait on the path; compares not counted | Yes, compares unmeasured |
| Worker steps per job | No path | 1 from slot write to kick, with one mask and re-enable and one firmware-queue check | 1 to the kick (1 mask, 3 critical sections, 1 queue send, 33 transport and 4 maintenance `DSB SY`); 2.00 per job in all with jobs that complete at once, 2.10 with modeled durations | Yes |
| `DSB SY` per job, maintenance | — | 6 | 6, in three cache calls; every cache operation on the output region is clean-and-invalidate (204 per bench run) | Yes |
| `DSB SY` per job, transport | Compute path: 16 per submit, 11 per event + 5 per drain | 59 | 57 with jobs that complete at once (`gpu_memory.c` 37, mask and re-enable 6, client ring 4, GPU register accesses 10); 58.9 with modeled durations | Yes |
| `DSB SY` per timed wake; per firmware event | — | 19; 11 | 18.0; 11.0 | Yes |
| Queue traffic per job | No path | 2 wake messages and 1 record: 6 queue operations | 2 sends, 1 send from the interrupt handler and 3.0 to 3.1 receives: 6.0 to 6.1 | Yes |
| Worker wake-ups per recording | No path | 2 + ⌈job time / 4 ms⌉: 633 on the A76 table | 429 in the traced run (204 job wakes, 204 interrupts, 21 expired waits), equal to 2 + ⌊job time / 4 ms⌋, because the completion interrupt ends the last partial wait; 463 at real time | Yes, below the target |
| Cache lines at OPEN; per job | Dispatcher design: 294,912 at OPEN | 64 per job, program and context page | 5,056 lines in 47 calls at OPEN: 2,880 for the session's 45 pages (64 each) and 2,176 for the firmware's own 34 pages; 5,129 per job, as the time model's 1,045,865 lines over 204 jobs | Yes |
| Tag area at boot | — | 1,024 lines per boot | `identity.c` zeroes and publishes 64 KiB, which is 1,024 lines; the step runs at EL3 on the board only and was not run on the host | Not measured |
| Hang detection | None | 1 s plus at most one 1 ms tick after the kick | LOST 1,000,751 µs after the kick, then submit returns -1 | Yes |
| Pool pages for the GPU | 0 | At most 80 write-back and 52 uncached, after USB2 and audio | 79 write-back (34 firmware, 21 payload, 24 table) and 51 uncached (45, 6), in the QEMU construction and in the replay with USB2 and 10 audio pages first | Yes |
| Linear stage, bench recording (model) | 14.894 s on one A55 | 0.535 s, bound 0.54 s (A76); 1.147 s, bound 1.15 s (A55) | 0.5351 s, 45.4% on the GPU; 1.1476 s, 90.2%; 0.665 s and 0.817 s on the A76 pair with 200-clock and 400-clock loads | Yes (model) |
| Recognition, PCM to text, bench recording (model) | 19.476 s on one A55 | Goal 0.4435 s, 0.05 times the audio | 0.818 s (0.092 times) on the A76 pair, 3.439 s on the A55 pair | **No**, as the design states |

## Other host results

| Item | Result |
| --- | --- |
| Session cases in QEMU | All 12 pass: bench with exact sums (5,870,398 sums equal to int64 CPU sums), bench with pattern sums, jobs that complete at once, one firmware event per job, refused jobs, failed wake send, missing tag, hang, context reset, OPEN only, an unexpected timeline value before a submit (LOST, service failed, later submit -1) and a failed completion clean-and-invalidate (the same) |
| Engine model | Passes without the hook header (no OPEN) and with it: SiLU and GLU sinks with `n_gpu < N` and `n_gpu = N`, int32 and int64 sums, a residual call and calls of four jobs give bit-identical outputs; a call whose ordinal names another 176 x 176 tensor is refused at `begin` and runs on the CPU; refused, lost, incomplete and wrong-sum jobs keep the outputs identical; reporting CPU columns to the GLU sink first changes the output |
| Dispatcher model | Both tables equal the design's; 204 placed calls and jobs; exact sums; stop-detail cases, in which only a wait for OPEN or a job's record turns a silent recognizer into a GPU loss; mapping rules at both strides refuse a bad page at each checked position; longest input 4 jobs per call at most on the A55 pair, 2 on the A76 pair |
| Configuration gate | Refused in the supervised lifetime and with a supplied `A733_GPU_OUTPUT_EL0_READ_ONLY`; configures resident, with and without the microphone |
| QEMU golden mapping comparison | `checks/device-memory-capacity.py` passes, including the read-only output view |
| `gpu_memory.c` per job, no target | 27.0 translations (design reading: about 47), 24.0 `external()` scans (about 26), 26.4 clock samples, each one counter-clock lock acquisition in the image (about 12); 25.0 critical sections and 5.24 from the interrupt handler |
| Steps by kind | The kick step 32 `DSB SY` and 4 for register accesses; the step that posts the record 14 and 6 |
| Validation cost (estimate) | The hook's comparison of every sum costs about 1.9 s per bench recording on the A76 pair and 15 s on the A55 pair at two cycles per MAC, until the board items let it go |

## Conclusion

Default images are unchanged apart from their build identity, the GPU
linear image links within its code budget, and every budget of the GPU
session that the host can measure is met: 18 translations at application
start, one worker step to the kick and two per job, 57 transport and 6
maintenance barriers per job, 18 per timed wake and 11 per firmware event,
six queue operations, 429 worker wake-ups per recording, a hang detected 1 s
and 0.75 ms after the kick, and 79 write-back and 51 uncached pool pages. The
recognition goal of 0.05 times the audio duration is not met by any modeled
split, as the design states. No board has run any of this.
