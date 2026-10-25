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
  enable. The startup rows give the sequence with domain 4 powered first and
  the shared SRAM routed to the NPU ([routing design](npu-sram-route.md)),
  with the latency changes below.

## Startup latency, change by change

Boot to READY in the host model with domain 4 OFF at entry, after each
change on `work/npu-production-1025`:

| Change | Commit | Boot to READY | Calls |
| --- | --- | --- | --- |
| Routing, power first (before the latency work) | `75b20c7b` | 12,001 µs | 13 |
| The owner verifies the supplies at its first step, without the 5,160 µs settle | `67b0f60e` | 8,001 µs | 9 |
| The bootstrap blocks until the power service's notification or the settle's last tick, and spins the remainder | `44043ed3` | 5,628 µs | 4 |
| DCDC2 published before DCDC4; PLL_NPU starts at once with an ON domain | `a280fb65` | 4,628 µs; 4,615 µs with domain 4 ON | 4 |

The 2 ms vendor settle stays and ends exactly. The PMIC provider moves one
register per transaction, so the 12 verification reads cannot merge into a
multi-byte transfer without a provider change; that change is not made. The
CPU-side NPU setup has nothing to move into the settle. The adapter builds
the MMU tables before the scheduler starts. The convolution images also
build both command streams then; the runtime images build a model's stream
when a client loads it through the active service, after OPEN. The bootstrap
stream's integrity check stays immediately before the device fetches it.
- Runtime, client and slot counts come from `checks/npu-session.py` in QEMU
  with two Cortex-A55 cores, and from `checks/npu-runtime-bounds.py` over that
  evidence.
- Image counts come from `checks/npu-image-checks.py`, which builds the
  single-core, all-core, accelerator and board images and runs the four image
  checks and `checks/npu-board-providers.py`.

## Results

| Metric | Base | Branch | Target | Met |
| --- | --- | --- | --- | --- |
| Boot to READY, scheduler start to READY, domain 4 OFF at entry | No board image | 4,628 µs, 5 ticks as the model rounds: the owner reads DCDC2 at its first step and publishes it verified at 2.0 ms; the call it wakes runs the pass and the domain-4 request; the call that the row's ON answer wakes runs the enable table, LOCK and the local open table, ending at 2.63 ms; the call at the last tick before the settle ends spins to its end, 2 ms later, reads back and returns READY | ≤ 5 ms | Yes |
| Boot to READY, domain 4 ON at entry, as both boards read at boot | No board image | 4,615 µs: PLL_NPU starts while the owner adopts the row | ≤ 5 ms | Yes |
| Second start, PLL_NPU and domain 4 adopted | Power removal | 4,414 µs, 6 writes, 3 LOCK samples, no PLL, gate-control or PCK600 write | ≤ 5 ms | Yes |
| Startup at 852 MHz: cold enable; rate change from a running 492 MHz PLL | No profile | 4,628 µs; 4,615 µs with 5 PLL writes in the vendor order | ≤ 5 ms | Yes |
| Startup calls | 9 harness polls | 4, each advancing a stage; the bootstrap otherwise blocks on its notification or the settle's last tick | 4 | Yes |
| Startup register reads, LOCK reads excluded, owner's domain-check reads included | 90 | 61: 56 by the sequencer and 5 by the owner's domain check; 70 LOCK samples besides. Counting the native write adapter's read of each of the 13 written words, 74 device reads. 852 MHz adds the SID read | ≤ 61 | Yes |
| Startup register writes | 18, of which 6 to PCK600 | 13, one of them to the routing word, none to PCK600; the model's write callback admits only `CCU+0xb00`, `+0xb04`, `+0x1920`, `+0x2a0` and SYSCTRL `0x03000008` | ≤ 13, none to PCK600 | Yes |
| Register-lock acquisitions in startup | 7 | 4 | 4 | Yes |
| Counter reads in startup | 31 | 20, including those of the three wait computations | ≤ 16 when advancing, one per wait | Yes |
| PMIC traffic caused by startup | 12 register reads, 6 bus-state reads, 5,160 µs wait | None. The owner's verification makes 12 single-register read transactions, 6 per rail, with no settle; startup waits only for the 6 of DCDC2 | None | Yes |
| Bytes hashed by startup polls, accelerator image | 20.7 MB; the call graph of `npu_convolution_startup_poll` reaches `digest` and `hash_block` | 0; its call graph reaches 29 functions and no digest function, nor do the startup and GPU callbacks it calls through pointers | 0 | Yes |
| GPU/PCK managers in an image with the NPU | 1 in the accelerator image | 1 in the accelerator image; 1 in the board image | 1 | Yes |
| `sizeof(struct npu_session)`, single-core image | 608 bytes | 456 bytes | ≤ 456 bytes | Yes |
| Source lines, code + header: startup | 445 + 131 | 408 + 79 | ≤ 260 + 60 | No |
| Source lines: platform tables | 223 + 96 | 19 + 28, the fabric validation only | ≤ 90 + 30 | Yes |
| Source lines: PLL table | 204 + 109 | Folded into the sequencer as statics | ≤ 60 + 25 | Yes |
| Source lines: clock adapter | 128 + 19 | Deleted | Deleted | Yes |
| Runtime parent, board image | No board image | 8,388,608 bytes at `0x48000000`, 2 MiB aligned | 8,388,608 bytes | Yes |
| Largest free extent after admission | Not modeled | 796,917,760 bytes; the only pool callers are the CE session, USB2 and `board_application_start`, so LOAD and CLOSE leave the extents unchanged | ≥ 8,388,608 bytes; unchanged | Yes |
| Requests, replies and client tick waits per inference | 1, 1; one `vTaskDelay(1)` per tick of waiting | 3 jobs: 3 requests, 3 replies, 0 tick waits; a STATUS and a LOAD before ready are answered not-ready | 1, 1, 0 | Yes |
| Copies per tensor byte | No board image | Input 1 memory copy plus PIO, output 1 memory copy plus PIO, counted through `apps/npu-board/usb.c` over 451,584 input bytes; a READ of mimus-linear-q's 2,816-byte output fits the image's 4 KiB transmit FIFO, which `npu-board-providers.py` reads from the linked TinyUSB class | 1 plus PIO each way with the image's TinyUSB configuration | Yes |
| Cache lines maintained per job | 19,442 | 19,442 | 19,442 | Yes |
| Bytes zeroed per job | 1,093,632 | 1,093,632 | 1,093,632 | Yes |
| Integrity-check bytes read per job | 1,509,856 | 1,509,856 | 1,509,856 | Yes |
| G, P, H, W, ShuffleNetV2 slot | 1.44, 5.41, 2.52, 8.22 ms | 1.15, 4.33, 2.01, 6.58 ms | 1.15, 4.33, 2.02, 6.58 ms | Yes |
| Wakes while idle | Service worker one per 1,000 ms; bootstrap and client 0 | Bootstrap and thermal monitor each wait at most 25 ms; client 0; service worker one per 1,000 ms | Independent supervision of a frozen monitor; client 0 | Yes |
| Worker steps and idle-register reads, four running jobs | 1,040 steps, 1,060 reads over 12 ticks | 1,014 steps, 1,034 reads over 11 ticks; bound 1,070 | Reads ≤ steps + 4 × (ticks + 1) + 8 | Yes |
| USB0, 64 KiB response | 13 ticks | 13 ticks, 128 packets, at most 10 packets and 189 µs per tick | 13 ticks | Yes |
| One ShuffleNetV2 inference over USB0, upload to output read | No board image | About 40 ticks, estimated: 150,528 input bytes are 294 high-speed packets, 30 ticks at 10 packets per tick; the 6.58 ms slot, 7 ticks; one tick for each of SUBMIT, READ and RETURN | No target; measured on the board | Not measured |

The worker step counts follow QEMU's timing; earlier branch runs of the same
case took 746 steps over 10 ticks, 1,016 over 12 and 1,056 over 25. The check asserts the
read bound for every job.

The source-line budget is missed for the sequencer. It holds the register
pass, which the base kept in the platform file (`pass`, `peri0` and their
tables, 65 lines), the PLL_NPU enable table and its predicates, and the local
open and stop tables with their apply helper, each with the comment that cites
its source. The platform module keeps only the fabric validation that the
session validators share. Together the sequencer and platform files hold 427
lines of code, against 410 in the three targets, and 107 header lines against
115. With the clock adapter and the PLL native file, the base held 1,455 lines
in the files this design replaces or deletes; the branch holds 601 in their
place, counting the merged native allowlist.

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

## Direct FIFO validation on 2026-10-24

The previous board-client result counted two memory copies per tensor byte
in either direction. The dedicated hardware-FIFO configuration reduces this
to one copy plus PIO, with the same three runtime requests, three replies and
zero client tick waits over 451,584 uploaded bytes. The transmit FIFO remains
4 KiB and the largest catalogue response remains 2,880 bytes.

Evidence is under `~/.cache/sunstoneos/build/npu-resume-1024/`:

- `board-client/result.json`: the runtime and protocol case in two-core QEMU.
- `usb-high-final-2/result.json` and `usb-full-final/result.json`: the final
  linked image's MUSB PIO, RX/TX ring wrap, odd addresses, aggregate completion,
  terminal zero-length packets and early RETURN ordering.
- `providers/result.json`: provider construction and linked FIFO sizes. This
  result predates only the final PIO word-assembly edit; the provider and
  vendor-class code are unchanged.
- `dcd-staging-command.json`, `dcd-staging.log` and `pio-word-emission.txt`:
  compilation with the ordinary endpoint-staging configuration and inspection
  of the final PIO word and byte loops.

The image builds as `root/sunstone-a733-npu-board.elf`; the native USB records
bind its SHA-256. These checks model hardware and establish no physical USB
rate, NPU arithmetic, interrupt delivery, cache visibility or recovery.
