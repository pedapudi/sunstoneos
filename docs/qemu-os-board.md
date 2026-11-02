# The virtual OS board

The virtual OS board runs the SunstoneOS image under QEMU `virt` with eight
cores, so that startup faults appear on the host instead of on an A733 board.
It lives in `platform/qemu-os` and is exercised by `checks/qemu-os-boot.py`
(see [the status record](status/qemu-os-boot.md)).

## Division of work

Nothing under `apps/os`, `apps/board` or the driver cores differs from the
A733 image. The A733 hardware and the stage-2 loader do not exist in QEMU, so
the board description supplies them behind the interfaces the OS already uses:

| Interface | Board's provider |
| --- | --- |
| Register accessors `include/a733_mmio.h` | `platform/qemu-os/a733_mmio.h` sends every plain access to the register model in `mmio.c`. The check compares the two headers line by line apart from the eight plain accessors |
| Thermal sensors (THS) | `mmio.c` models the update flags that clear by writing one and the data registers. Each sensor has a first-reading time, a period, a temperature and a slope from the scenario; the efuse words describe the factory calibration |
| PMIC and its bus | `mmio.c` models the S_TWI0 controller (START, address, byte and STOP events that finish after the documented bit times, with the controller's status codes) and the AXP register file that the power provider reads and writes. The provider driver is the A733 one |
| Power domains, clock and rail leases, pins | The A733 cores with their adapters; the adapters read inherited register state that the board seeds as boot0 leaves it |
| Sealed session and launch contract | The stage-2 loader's part is `kernel_main` in `board.c`. It stamps the contract's times, admits it with the OS's own admission and seals the session with `apps/board/session.c`. The check builds the session with `apps/os/host.py` and loads it at the address the loader uses |
| USB device transport | `board_usb0_task` in `board.c` reports a fixed state, enumerated and ready, a scenario-set time after its task starts, and carries no requests. A scenario fault makes it stall or stay silent |
| Watchdog WDT0 (resident lifetime) | `mmio.c` holds the interval that the loader arms; a write of `0x14af` to the control register restarts it. The board's monitor ends the run with status 5 when the interval passes |
| Shared peripheral interrupt controller | `gicv3_spi_*` in `board.c` hold an enable flag; the thermal interrupt never fires |
| NPU, GPU, Type-C, USB host, radio | Not linked or absent; `absent.c` names any call that reaches them. The session selects workload 2, which starts none of their tasks |

The counter of QEMU runs at 62.5 MHz. Each core writes 24 MHz to
`CNTFRQ_EL0` at startup, which the highest implemented level may do, so the
OS's division by 24 and its frequency checks run unchanged. Guest time then
passes 2.6 times faster than wall-clock time, and every duration in a log is
guest time.

## Scenario

The check loads a data page at `0x41e00000` (`struct qemu_os_scenario`): a
fault to inject, the first reading time, temperature and slope of each
sensor, the USB transport delay, when to end the run and a trace mask. Adding
a part with other behavior takes a new case in the scenario and a register
entry in `mmio.c`, with no change to the OS.

## Records

The OS reports its first board failure through the breadcrumb supervisor
calls of the returning images (`include/sunstone_crumb.h`). The board's vector
code prints that record as `FAILURE-RECORD group=... detail=...` and ends the
run 60 ms later, which stands for the watchdog reset that follows a failure on
the board. A supervising task that stops the session in that time adds its
stop line (`STOP status=... detail=...`).
Other console lines mark the stages: `BOOT`, `APP-MAIN` (the first instruction
of the application's main, with the OS's `ss_live()` and `ss_throttled()`
values), `THROTTLE-CLEARED` and `APP-FINISHED` with the result.

## Inherited stack register and the context-save check

Each core enters the kernel with an SP_EL0 that lies in the read-only code
block, as the first A733 launch of the owner scheduler found (0x41039f60). The
port saves a task context on SP_EL0, so any save taken before the first task
restore faults on this board as it does on the A733. The vectors in `start.S`
also check, at every synchronous exception or interrupt taken from a task
(EL1t or EL0t), that SP_EL0 is 16-byte aligned and lies in writable RAM below
the exception stacks. A violation prints `SP-EL0-VIOLATION` and ends the run
with status 6.

The check builds the same board with the owner scheduler
(`configSS_PARTITIONED_SCHEDULER`) for the hello and latency applications. The
board application changes the core affinity of its first task before the
scheduler starts. A negative control builds the scheduler without the refusal to
yield while the scheduler is not running and must end with the violation on core
0.

## Guest time

The board runs under QEMU's instruction counter (`-icount shift=0,sleep=off`, one thread), so guest time
advances with executed instructions, and idle time is skipped. A loaded host cannot stretch a service window or
a sensor deadline. With guest time on the host's wall clock, the resident cases failed at host load above about 60.
At the loader's first expiry the supervisor's stop and the watchdog expiry coincide; the silent-transport case
accepts either as the designed stop.

## Limits

The model checks protocol and sequencing, not electrical behavior or timing
on silicon. The OS CPU clock owner, the returning lifetime, newlib and the C++
runtime are not linked. The board description does not model unlisted
registers: they behave as memory.
