# RAM-only board tests ranked by risk

This plan orders the physical tests that can run on the attached Orange Pi
Zero 3W without writing storage. It is a plan only. None of these tests may run
until the user confirms that the board can be reconnected if it loses its USB
connection. Today it cannot, so a single lost connection ends board access until
further notice.

The board on host USB port `3-1` currently enumerates as the Allwinner boot ROM's
USB recovery device (FEL, vendor `1f3a`, product `efe8`). That observation
comes from the host's USB device list and involved no communication with the
board.

## How a test can lose the connection

Every test below runs from RAM and preserves both microSD cards. They differ in
how the board gets back to the boot ROM's USB recovery mode afterward:

- **The boot ROM keeps the USB controller.** Commands that only talk to the
  boot ROM, or that run code which returns to it, leave the ROM's USB stack in
  control. A hang inside the code still loses the connection, because the
  ROM cannot answer until the code returns.
- **The EL3 monitor forces a return.** The FreeRTOS images run under a small
  EL3 monitor. A secure timer fires after a deadline and the monitor resets the
  CPU back into the boot ROM's recovery mode. The forced-stall case of the
  physical kernel check demonstrated this return on 2026-10-15. It cannot recover
  from a stalled bus access, which blocks the CPU before the timer can act.
- **Native code takes the USB controller.** Images with a native USB device or
  host stack reprogram the controller that the boot ROM uses. The board then
  re-enumerates. If the native stack fails, or the handback fails, the host sees
  nothing until the board is power-cycled.

Bus stalls are the main risk that the monitor cannot cover. Reading a register
in a block whose clock, reset or power is off can stall the CPU indefinitely.
Every test must therefore read only registers whose access conditions are
established, as the board operations skill requires.

## Ranked tests

Each test assumes the previous ones passed in the same session. Each runs once,
under the port lock in `tools/usb-access.py`, with a frozen image and an
evidence directory chosen in advance. Any transport failure stops the session;
the rules forbid a retry.

| Rank | Test | What it establishes | How it can fail |
| --- | --- | --- | --- |
| 1 | Boot ROM identity query through `tools/usb-access.py` | The board answers recovery-mode commands, the SoC ID is `0x1903`, and host permissions on port `3-1` work | Only if the ROM's USB stack is already wedged; no board code runs |
| 2 | Returning SRAM probe (`boot/a733/launch-survey.S` through `tools/run-launch-survey.py`) | Code runs from SRAM and returns to the ROM; saved SRAM bytes are restored | A fault in the 32-bit probe before it returns |
| 3 | DDR initialization and two-page check (`tools/run-ddr.py`) | The pinned boot0 DDR routine trains 12 GiB of LPDDR5 and two 4 KiB pages read back correctly | DDR training hangs inside opaque vendor code with no timer to force a return |
| 4 | Single-core FreeRTOS kernel check (`tools/check-a733-kernel.py --mode kernel`) | Timer preemption, queues and FP context on the boot core, then a monitor return | A bus stall; otherwise the forced return covers a hang |
| 5 | Forced-stall recovery case (`--mode timeout`) | The monitor's timed return to the ROM still works with the current build | The return itself; this is the recovery path every later test relies on |
| 6 | Boot-clock report and the returning thermal application (`apps/thermal-returning`) | The CPU clock that every timing record must name, and live die temperatures before any sustained load | Reading a thermal or clock register whose bus clock is gated |
| 7 | Accepted all-core first-task diagnostic, frozen ELF `762237cd…` | Re-confirms all eight cores reach their first FreeRTOS task and shut down, on this board, today | A secondary core can hang during power-up, outside the boot core's monitor return |
| 8 | Full returning eight-core workload (frozen in `all-core-task-workload-20261019/frozen/`) | Task bodies, floating point, SGIs and the CPU0 timer on all cores: the main unqualified multicore step | As in rank 7, plus scheduler faults under load |
| 9 | Native USB device channel (`tools/check-a733-usb-returning.py`) | The native USB0 driver can carry results and hand the controller back | Any native USB failure leaves the board unreachable until power-cycled |

### Why this order

- Ranks 1 to 5 re-establish the conditions that every later test depends on:
  identity, code execution, DRAM and the forced return. They repeat tests that
  passed before, so their expected results are known.
- Rank 6 is needed before any performance work. The goal requires every timing
  record to state the clock, and the thermal reading decides whether sustained
  multicore load is safe.
- Ranks 7 and 8 produce the evidence that most unfinished work waits for:
  multi-worker mimus, concurrent workloads and the resident OS all depend on
  sustained eight-core scheduling.
- Rank 9 comes last because it is the only test whose failure cannot be
  recovered without a power cycle. The device paths that report through USB0
  depend on it, so it is the gate for all peripheral qualification.

## Bundles for the memory, GPU and NPU paths

[tools/prepare-board-bundles.py](../../tools/prepare-board-bundles.py)
prepares a frozen bundle and a queue submission for each new memory, GPU and
NPU path ([design](../designs/board-bundles.md)). It never contacts the board,
never submits and never claims. The coordinator supplies each row's
preparation inputs in one JSON file and submits the result. The tool runs
every preparation module in a sandbox with no USB device, no network and a
read-only file system outside its new run directory under
`~/.cache/sunstoneos/evidence/`. Each row has one of three recovery classes:

| Position in this plan | Row | Class | Runner | Queue timeout |
| --- | --- | --- | --- | --- |
| After rank 7, before rank 8, once an independent watchdog record exists | Memory qualification, one run per bundle of the three-step map chain | Monitor | Memory profile through `tools/a733-returning-host.py` | 1,200 s |
| After rank 9, first board session | NPU service in the OS image | Terminal | `tools/check-a733-os-supervised.py` | 970 s plus the uploads |
| After rank 9, next board session | GPU service in the OS image | Terminal | `tools/check-a733-os-supervised.py` | 970 s plus the uploads |
| After rank 9, next board session | GPU and NPU jobs under the memory and power arbiter | Terminal | `tools/check-a733-accelerators-supervised.py` | 930 s plus the uploads |
| After rank 9, next board session | Recognition dispatcher in the OS image | Terminal | `tools/check-a733-os-supervised.py` | 970 s plus the uploads |
| Last in a connection window, after the ranks pass | GPU and NPU hang reset; voltage and clock changes; display output | Not yet bundled | None | None |

- **Monitor.** The EL3 monitor's secure timer forces the return to the boot
  ROM. Before it records a Monitor bundle, the tool checks the bundle's
  `image.elf` with `nm`. It refuses an image with no symbol table, without
  `monitor_entry`, or with a defined symbol from a USB stack, the shared USB0
  loop or secondary-core power-up (`dcd_`, `hcd_`, `tud_`, `tuh_`, `usb0_`,
  `a733_cpu_start_`, `a733_cpu_secondary_`). A memory image must also define
  `monitor_renewal_cap`, the capped deadline renewal that forces a return at
  most 22 s after entry. The submission's prerequisite is
  `independent-watchdog-rom-return`. No record qualifies for it yet, so the
  memory runs wait: the monitor timer cannot recover from a stalled first
  access to untested DDR.
- **Terminal.** The supervised runner keeps the port lock after every outcome
  until whole-board power removal is confirmed, so each Terminal test ends the
  board session. Its prerequisites are the runner's own `PREREQUISITES`. Its
  timeout is the worst case of one attempt. That sum covers the USB churn
  guard's 615 s wait, the FEL request bounds of the SRAM backups, of every
  bundle file's upload and readback and of the SRAM restores, the capture
  window, and 60 s of margin. An existing OS-image bundle of 22.9 MB gives 2,451 s. The tool
  refuses a row whose bound exceeds the queue's 3,600 s limit.
- **Excluded.** The test needs an accessory, a voltage or clock change, or an
  intentional hang. The tool prepares no bundle.

Memory runs require ranks 1 to 7 earlier in the same power cycle, with rank 3
reporting `dram_mib` equal to 12,288 and rank 6 reporting the boot clock. Rank
7 re-runs a frozen ELF that has passed, so it comes first. Placing memory
before rank 8 is a judgement: a non-linear address decode that turns a
tested-DDR write into a monitor write is less likely than a secondary-core
hang under load. A memory run record chains to the next run only when it shows
an image-initiated exit, every field present, alias-probe, address-line and
page-signature passes, no external abort and no expired DMA0 deadline. Any
other record retains the board. No further upload of any kind follows,
including rank 8 and the Terminal rows, until the user reviews that record.
After a DMA0 expiry, uploads also wait until a FEL read shows DMA0 quiet.

Terminal tests require ranks 1 to 5 and rank 9 earlier in the same power
cycle. NPU comes first because GPU firmware writes memory on its own
([remote writers](../remote-writers.md)), so a GPU failure has more causes to
separate. Each Terminal record must report, per job, the completions observed
by interrupt, those observed by the one-tick fallback and the client
wake-ups. The counts are measurements, not pass conditions; zero fallback
completions over a session is the evidence that would remove the tick wait in
the GPU and NPU services.

Status: the tool is **implemented** and checked on the host by
[checks/board-bundles.py](../../checks/board-bundles.py); no bundle of these
rows has been prepared from real inputs. The tool refuses every row until its
owner supplies what the row needs:

- **Every row.** The owning package must name the admission values that
  identify the row's build. Until then the row's `admission` is empty and the
  tool refuses it, so an image admitted for another path cannot be prepared
  under this row.
- **Memory qualification.** The memory qualification profile
  `apps/memtest/check.py` and the capped renewal handler do not exist yet.
- **NPU service, GPU service and recognition dispatcher in the OS image.**
  `apps/os/prepare.py` admits only the mimus, thermal and audio OS builds.
  `drivers/npu/npu_service.c` and `drivers/gpu/pvr_gpu_service.c` do not
  count interrupt-observed completions, fallback-observed completions and
  client wake-ups separately, and `tools/check-a733-os-supervised.py` does
  not report them or declare `COMPLETION_COUNTS`.
- **GPU and NPU jobs under the arbiter.** The memory and power arbiter is
  designed but not implemented. The two services lack the same counts, and
  `tools/check-a733-accelerators-supervised.py` does not report them or
  declare `COMPLETION_COUNTS`.

## Tests that need a reconnect budget or an accessory

These risk the board's connection or need an accessory, so they run after the
ranked tests pass on that board and last in a connection window, or wait for
the accessory:

- any test that writes a microSD card, including the persistent-boot package
  and the stage-2 loader, after a host backup of that card;
- intentional watchdog expiry;
- intentional GPU or NPU hangs, including hang-reset tests;
- raising the CPU clock or changing any PMIC voltage;
- every peripheral path that reports through the native USB0 channel, until
  rank 9 passes;
- camera, display, microphone, headset, PCIe endpoint and access-point tests,
  which need accessories that are not attached.
