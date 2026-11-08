# The failure window of a returning session: every actor and every path to a stop

Status: audited on the host for the session of the NPU recognition (granted: the thermal service, the CPU clock grant, the NPU). Branch
`work/npu-encoder-chain-1107`. The model is `checks/os-failure-step.py`.

A failure return starts when `failing` is set by the claim (`failure_claim`, `apps/os/cpu-return.c`) and ends when the cores reach the boot ROM.
[failure-return.md](failure-return.md) describes the sequence. In that window a path that reaches `board_fail`, `fail_close`, `platform_exit` or
`a733_smp_exit_privileged` closes admission (`board.fatal`), and from then on only the watchdog ends the session. The record of the first four
board runs of the NPU failure test was one such path per run. This list is every actor that runs in the window, read from the source, with each
path to those four calls and what it does in a failure return.

## What the session grants, and what the model leaves out

The session of `apps/mimus-board` with the NPU pack is `profile npu, services thermal, cpu_grant, npu` (`tools/session.py build`): no USB
host, storage, audio, headset, radio, GPU, DMA or performance grant. The model contains the actors that exist in that session and leaves out the
rest, for these reasons, each read from the source:

- **Tasks that are not created.** `apps/board/main.c` creates the Type-C, power and USB2 tasks only for a host workload and the radio tasks only for a
  radio workload; the SD owner (`apps/os/sd_owner.c`), the DMA service (`drivers/dma`), the GPU service, the audio and headset tasks, the performance
  sessions and the large-region loader exist only when the manifest or the grant asks for them, and none does.
- **Liveness reporting.** `apps/os/services.c` demands progress reports only in the resident lifetime and from services the session grants.
  A returning session reads `board_live()` and nothing else (`bounded()` in `cpu-return.c`).
- A different grant (a GPU session, a USB host session) needs its owners added to the model and to this list before its failure return is trusted.

## The actors of the window

Verdicts: safe (cannot fire in a failure return, with the reason), fixed (fired in a board run, repaired), needs (not yet shown safe).

| Actor | In a failure return | Paths to a stop | Verdict |
| --- | --- | --- | --- |
| Claim (`failure_claim`: `ss_fail` SVC, EL0 fault stub, unexpected-interrupt handler, `platform_panic`) | publishes `failing`, the result record and the time; no task API | returns 0 (so `board_fail` follows) only when `board.fatal` is already set, when the completion cutoff has passed, or when `closing` was set by a normal return; otherwise 1 | safe |
| Application tasks (main, workers, NPU owner) | the main task is suspended by `ss_fail`; the others continue; a second `ss_fail`, an EL0 fault or `ss_finish` finds `failing` set | `failure_claim` returns 1 at its first line; `ss_finish` refuses with `ss_fail(0x756)`, which is the same | safe |
| NPU owner (the application's encoder task) | closes the queue when `ss_live()` is -1, at a safe point, and parks | `fault()` of the layers library if the SDK refuses (`ss_fail_value`, handled as the line above) | safe (and required: without it the return waits 3 s and ends at the watchdog, 0x742) |
| NPU client (`npu-client.c`) | follows the ring: the owner's close ends its loop; waits for the stop; publishes its report | `RETAIN` sites 0x601 to 0x60c (progress or close refused), each a device failure | safe given the owner's close at a safe point: after the close no job is in flight, so `retirement_fault` finds no late interrupt; the same code path as a normal return after `ss_npu_queue_close` (board run `1794126613592469548-a5cce9e2`) |
| NPU bootstrap, service worker, power owner (`npu.c`, `drivers/npu`) | stops the engine after the close | `watch()` refusals (board not live, `npu.failed`, `now >= npu.end`, a STOP gate past its end), `retirement_fault`, `fatal()` on a hardware error | safe: `board_live` is a consequence there, never a first cause; the gate is allowed until UINT64_MAX once all 31 thermal channels were seen, and a STOP is only set by the thermal level or `os_npu_close` |
| Thermal owner (`thermal.c`) | keeps reading the sensors; after the clock owner lowered CPU-B it checks the devices | `fault()` on a CRITICAL reading or a driver fault (a real temperature fault, same as in the session), `platform_exit(6)` at four sites in the return branch | fixed: the devices-not-ready exit (line 381, board run 4) now waits during a failure return (`os_cpu_return_thermal_step`); the other three exits need `mask()` or `live()` to fail, which are consequences of a stop that already happened |
| CPU clock owner (`cpu.c`, called by the thermal owner on every pass) | lowers CPU-B to boot0's 1014 MHz, publishes it, then does nothing | `REFUSED` sites: 230/260 driver retained or invalid, 234 request refused, 242 report not decoded or an id still held (all hardware or driver errors, and the same as in a normal return, which also may start in any phase), 237 `return_request` and not COMPLETE | fixed: 237 fired on a pass after the lowering was done (board run 5); the owner returns at once when `return_done` is set (`os_cpu_return_clock_pass`) |
| Supervisor (`os_cpu_return_supervise`, task on core 0) | restarts WDT0, checks the failure time, waits for the devices, then releases the PMIC stop, the thermal done flag and the terminals | `bounded()` 0x736/0x744/0x8xx (board not live: a consequence), `fail(0x742)` (3 s), `fail(0x741)` (devices not ready) | fixed: 0x741 now applies only to a normal return; a failure return waits |
| PMIC owner (`board-pmic`, `pmic_stop` branch of the supervisor) | `a733_power_service_step`; stops after the supervisor sets `pmic_stop` | `fail(0x739/0x73a/0x73b)` if its locks, slots or borrowed ids are not idle; `fail(0x741)` at line 232 | safe: `pmic_stop` is set only after `thermal_done`, which is set only after the clock owner's lowering and the devices' retirement; at that point no clock or rail request is outstanding; a PMIC fault is a hardware error as in a normal return |
| Terminals (`terminal`, one per core, highest priority) | clean caches, restore the interrupt controller, return to the ROM | `fail(0x737)` if woken out of order or the devices are not ready | safe: woken last by the supervisor after the devices are ready |
| USB0 owner (`board-usb0`, `usb0_service.h`) | serves the host channel (the result is read after the return) | `stop()` on a provider or stack error, on `!board_live()`, on `app_success` and a dead export | safe: no host-channel error is produced by the failure; `app_success` is false in a failure return; `!board_live()` is a consequence |
| Board watchdog task (`supervise` in `apps/board/main.c`) | calls the return supervisor; restarts WDT0 | `board_fail_at(BOARD)` if `!board_live()` (a consequence) | safe |
| Interrupt handlers: thermal (`os_thermal_irq`), NPU (IRQ 97), GIC, timer | thermal masks its source and notifies the owner; NPU marks its service | the NPU handler records a crumb and marks the service failed on a hardware error; an unexpected interrupt takes the claim or exits (`a733_smp_exit_privileged(4)`) | safe: none is raised by the failure; an unexpected interrupt is a defect in any return |
| `tlbi-issuer`, large-region loader, `bg_load`, control tasks, perf sessions | idle or not created in this session | none reachable | safe (not active) |
| Hooks: stack overflow (`vApplicationStackOverflowHook` calls `platform_panic`), tick and idle hooks | the OS defines no idle, tick or malloc-failed hook | stack overflow of a task: `platform_panic` is a stop | safe: the failure adds no stack use to a task beyond the normal return; unchanged |

## What the model contains

`checks/os-failure-step.py` and `.c` model: the claim (flags only), the supervisor (`os_cpu_return_failure_step`), `bounded()`
(`os_cpu_return_bounded_detail`), the application's NPU owner (closes within 100 ms or never), the NPU client (reports 40 ms after the close;
a stop from outside takes its retained path), the thermal owner (`os_cpu_return_thermal_step`), the CPU clock owner (`os_cpu_return_clock_pass`),
a device failure at an arbitrary time, and the PMIC and terminal gates as the order of the supervisor (a return completes only when the devices are
retired and the thermal owner is done). The actors marked safe above are not modelled because they have no path in the window; each reason is a
line of the table.

Four stops of the NPU failure test are reproduced by mutants of the model: the supervisor's stop of the NPU (0xa8b), the thermal owner exiting
(0x744), the clock owner repeating its return branch (0x744), and a failure return that stops at once (0x741).

## Residual risk

- The NPU client, bootstrap and service worker were read, not modelled: a failure return depends on the application closing at a safe point. An
  application that never closes ends at the 3 s limit with 0x742, as designed.
- The thermal owner does not wake on its own interrupt during the wait if the source is masked; it reads on its 25 ms schedule.
