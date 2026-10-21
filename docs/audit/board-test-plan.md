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

## Tests that must wait for physical recovery

These need the ability to power-cycle or reconnect the board, or an
accessory, and are excluded from this plan:

- any test that writes either microSD card, including the persistent-boot
  package and the stage-2 loader;
- intentional watchdog expiry;
- raising the CPU clock or changing any PMIC voltage;
- every peripheral path that reports through the native USB0 channel, until
  rank 9 passes;
- camera, display, microphone, headset, PCIe endpoint and access-point tests,
  which need accessories that are not attached.
