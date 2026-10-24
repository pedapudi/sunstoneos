# NPU board image: measured budgets

Status: host-modeled. Every number below comes from a host model, QEMU or
Unicorn run of the linked images, or from the source; no board was used. The
budgets are the ones in [npu-board.md](npu-board.md).

## Conditions

- Base: the integrated tree before the sequencer, with the link fix applied so
  the single-core images build. Its images are in
  `~/.cache/sunstoneos/build/npu-board/base-images`.
- Branch: this design as committed, with `primary.patch` applied.
- Startup counts come from `checks/npu-startup-host.c`, which runs the
  sequencer against the actual PMIC provider, rail ledger, clock registry,
  GPU/PCK manager and power-domain owner over a register model. The PMIC bus
  runs at 400 kHz, the tick is 1 ms and PLL_NPU reports LOCK 200 µs after its
  enable.
- Runtime, client and slot counts come from `checks/npu-session.py` in QEMU
  with two Cortex-A55 cores, and from `checks/npu-runtime-bounds.py` over that
  evidence.
- Image counts come from `checks/npu-image-checks.py`, which builds the
  single-core, all-core, accelerator and board images and runs the four image
  checks and `checks/npu-board-providers.py`.

## Results

| Metric | Base | Branch | Target | Met |
| --- | --- | --- | --- | --- |
| Boot to READY, scheduler start to READY | No board image | 11 ticks (11.0 ms): the owner sets its done bit at 8.39 ms; the call at that point runs the pass, the enable table, LOCK, the local open table and the row request, ending at 8.62 ms; the row reads ON at 9 ms; the settle ends at 11 ms | ≤ 12 ticks | Yes |
| Startup calls that advance a stage | 9 harness polls | 4 of 12 calls; the other 8 return PENDING | 4 | Yes |
| Startup register reads, LOCK reads excluded | 90 | 60 (130 reads, of which 70 are LOCK samples) | ≤ 60 | Yes |
| Startup register writes | 18, of which 6 to PCK600 | 12, none to PCK600; the model's write callback admits only `CCU+0xb00`, `+0xb04`, `+0x1920` and `+0x2a0` | ≤ 12, none to PCK600 | Yes |
| Register-lock acquisitions in startup | 7 | 3 | 3 | Yes |
| Counter reads in startup | 31 | 24: 16 in the four calls that advance, one in each of the 8 PENDING calls | ≤ 16 when advancing, one per PENDING call | Yes |
| PMIC traffic caused by startup | 12 register reads, 6 bus-state reads, 5,160 µs wait | None | None | Yes |
| Bytes hashed by startup polls, accelerator image | 20.7 MB; the call graph of `npu_convolution_startup_poll` reaches `digest` and `hash_block` | 0; its call graph reaches 29 functions and no digest function, nor do the startup and GPU callbacks it calls through pointers | 0 | Yes |
| GPU/PCK managers in an image with the NPU | 1 in the accelerator image | 1 in the accelerator image; 1 in the board image | 1 | Yes |
| `sizeof(struct npu_session)`, single-core image | 608 bytes | 456 bytes | ≤ 456 bytes | Yes |
| Source lines, code + header: startup | 445 + 131 | 356 + 100 | ≤ 260 + 60 | No |
| Source lines: platform tables | 223 + 96 | 45 + 47 | ≤ 90 + 30 | Code yes, header no |
| Source lines: PLL table | 204 + 109 | 10 + 31 | ≤ 60 + 25 | Code yes, header no |
| Source lines: clock adapter | 128 + 19 | Deleted | Deleted | Yes |
| Runtime parent, board image | No board image | 8,388,608 bytes at `0x48000000`, 2 MiB aligned | 8,388,608 bytes | Yes |
| Largest free extent after admission | Not modeled | 796,917,760 bytes; the only pool callers are the CE session, USB2 and `board_application_start`, so LOAD and CLOSE leave the extents unchanged | ≥ 8,388,608 bytes; unchanged | Yes |
| Requests, replies and client tick waits per inference | 1, 1; one `vTaskDelay(1)` per tick of waiting | 3 jobs: 3 requests, 3 replies, 0 tick waits | 1, 1, 0 | Yes |
| Copies per tensor byte | No board image | Input 1 (`tud_vendor_read` into the input window), output 1 (`tud_vendor_write` from the output window); 451,584 input bytes reached the window in the client case | 1, 1 | Yes |
| Cache lines maintained per job | 19,442 | 19,442 | 19,442 | Yes |
| Bytes zeroed per job | 1,093,632 | 1,093,632 | 1,093,632 | Yes |
| Integrity-check bytes read per job | 1,509,856 | 1,509,856 | 1,509,856 | Yes |
| G, P, H, W, ShuffleNetV2 slot | 1.44, 5.41, 2.52, 8.22 ms | 1.15, 4.33, 2.01, 6.58 ms | 1.15, 4.33, 2.02, 6.58 ms | Yes |
| Worker wakes while idle | 0 | 0; the worker blocks on its notification | 0 | Yes |
| Worker steps and idle-register reads, four running jobs | 1,040 steps, 1,060 reads over 12 ticks | 746 steps, 766 reads over 10 ticks | Unchanged behaviour: one step per tick of execution and one read per step | Yes |
| USB0, 64 KiB response | 13 ticks | 13 ticks, 128 packets, at most 10 packets and 189 µs per tick | 13 ticks | Yes |

The worker step counts follow QEMU's timing: an earlier branch run of the
same case took 1,056 steps over 25 ticks. Each run keeps one step per tick of
execution and one idle-register read per step.

The source-line budget is missed. The register pass, which the base kept in
the platform file, is now in the sequencer (`pass`, `peri0` and their tables, 65 lines),
and the platform header carries the step, result and register-callback types
that the PLL table and the sequencer share. The three files together hold
411 lines of code, against 410 in the three targets, and 178 header lines
against 115. With the clock adapter and the PLL native file, the base held
1,455 lines in the files this design replaces or deletes; the branch holds
655 in their place, counting the merged native allowlist.

## Image checks

| Check | Images | Result |
| --- | --- | --- |
| `npu-application-clock-reference.py` | Single-core, accelerator, board | Pass, six cases each |
| `npu-stop-native.py` | Single-core, all-core, accelerator, board | Pass, 14 cases each |
| `npu-single-core.py --retention-only` | Single-core | Pass, three cases |
| `accelerator-providers.py` | Accelerator | Pass |
| `npu-board-providers.py` | Board | Pass |

On the base, the retention check stopped at its third case and the provider
check returned reason 5; [npu-board.md](npu-board.md) gives both causes and
their fixes.

## Not measured

Boot to READY on the board, the PLL_NPU LOCK time, the INTID 97 delivery per
job and the USB0 transfer rate need the board, and are planned board steps.
