# USB0 failure attribution for resident queue entry 1794468954726206112-8d88da24

This host audit concerns the resident hello-compute image with SHA-256
`28155e9b4b9df1d901123437d8d5dfd200bca6d2e3f1c6339eb54383668352db`.
The [physical record](application.jsonl) retains core 1 at EL1 calling
`apps/os/usb0.c:stop()`, followed by core 0 EL3 stop `0x0111`, value `0x5fff`.
It records watchdog reset, return to FEL and lost DRAM. The runner correctly
rejects this as successful diagnostic completion.

The reader is assumed to know C, the USB device stack and the resident launch
contract. FEL is the boot ROM's USB loader. The audit uses host files only;
it establishes no current board state.

## The retained address identifies the common failure function

In the frozen ELF, `stop` starts at `0x41093860` and calls `board_fail` at
`0x4109386c`. The retained return address `0x41093870` follows that call.
Every caller of `stop` therefore produces that address and value zero.
The RTC record contains neither the caller of `stop` nor the provider's
failure fields, rail snapshots or application completion flag.

The following tables enumerate every direct call of `stop`. They include
the shared header compiled into the USB0 owner.

## Direct guards in apps/os/usb0.c

| Caller | Condition that calls stop | What this record establishes |
| --- | --- | --- |
| `tud_vendor_rx_cb` | Interface is not zero | No interface number is retained. The frozen runner sends no native vendor request. |
| `tud_vendor_tx_complete_cb` | Interface is not zero | No callback arguments are retained. |
| `tud_vendor_tx_cb` | Interface is not zero, board is not live, completed bytes exceed `sent-transferred`, or a successful application frame loses export health | No transfer counters are retained. The liveness predicate is examined below. |
| `os_request`, result operation | Flags or argument are nonzero, count is zero, or count exceeds `SS_HOST_PAYLOAD_BYTES` | Requires a received vendor request; the frozen runner sends none. |
| `os_request`, operation 5 | Any flags, offset, count or argument is nonzero | Same request prerequisite. |
| `os_request`, operation 6 | NPU is granted and any flags, offset, count or argument is nonzero | The frozen session grants no NPU. |
| `os_request`, operation 7 | CPU clocks are granted and flags are nonzero, count is zero, or count exceeds 1024 | The grant exists; a vendor request is still required. |
| `os_request`, operation 8 | Performance is enabled and count is nonzero, command exceeds `OS_PERF_SWITCH`, a non-switch command has offset or argument, or offset contains undeclared selection bits | The frozen session has no performance grant. |
| `exchange`, pending output | Board is not live | No direct comparison with contract expiry. |
| `exchange`, successful application output | Export health is negative | Requires an application response, which the runner does not request. |
| `exchange`, received successful reply | Export health is negative | Same response prerequisite. |
| `exchange`, receive | TinyUSB returns zero bytes or more than requested | Requires received vendor data; no receive counters survive. |
| `exchange`, complete request | `os_host_offer` refuses the request | Requires a complete vendor request. |

`board_usb0_task` skips USB ownership only for the returning lifetime.
Resident execution starts the providers and TinyUSB before publishing USB
readiness. Hello-compute waits for that readiness even though its manifest
declares no USB device. Application completion does not normally end USB0
ownership or resident supervision.

## Direct guards in apps/board/usb0_service.h

| Caller | Condition that calls stop | What this record establishes |
| --- | --- | --- |
| `a733_usb_prepare` | Provider health is negative or board is not live | The provider health reason is not retained. |
| `a733_usb_poll_enable` | Board is not live | No direct comparison with expiry. |
| `a733_usb_trace` | Board is not live | Trace event arguments are discarded. |
| `a733_usb_assert` | TinyUSB or the platform calls its assertion hook | The assertion site is discarded. |
| `tusb_time_delay_ms_api` | Delay exceeds 150 ms or board is not live | The delay argument is not retained. The selected DCD's delays fit the bound. |
| `usb0_service_start`, loop entry | Board is not live | No direct comparison with expiry. |
| `usb0_service_start`, provider step | `a733_native_usb_step` returns negative | Startup stage and provider result are not retained. |
| `usb0_service_start`, stack initialization | `tusb_init` returns false | No stack result is retained. |
| `usb0_service_pass` | Board is not live | This check precedes each service pass. |

## Provider refusals include several independent startup guards

`platform/a733/native_usb.c` supplies the callbacks behind the shared header.
Pending and busy results delay or retry; they do not call `stop`.

| Provider guard | Failure condition |
| --- | --- |
| Step entry | The board liveness callback refuses. |
| Step 0, rail acquisition | Result is neither busy nor pending. |
| Step 1, rail poll | Result is neither busy, pending nor ready. |
| Step 1, PMIC identity | Result is neither busy nor ready. |
| Step 2, each PMIC submission | Result is neither busy nor pending. |
| Step 3, each PMIC result | Result is neither busy, pending nor complete; or snapshot rail differs; either page is nonzero; chip differs from 3; revision is nonzero; decoding or enabled state differs from 1; or programmed voltage differs from the session. |
| Step 4, PMIC reap | Result is neither busy nor complete. |
| Later steps, USB platform poll | Result is neither pending, busy nor prepared. |
| Runtime health entry | Wrong task, unpublished preparation, uninitialized or moved USB object, or latched USB fault. |
| Runtime provider authentication | Board not live, wrong task, retained register lock, rail index differs from 4, wrong clock or rail owner, invalid clock token, rail token not ready, or PMIC lifetime not ready. Busy metadata retries. Liveness is checked again after authentication. |

USB platform I/O additionally requires the owner task to hold the register
mutex. It authenticates those providers before each native read or write.
Time, MMIO, hardware readback and token failures remain separate guards.
Failure after the platform acquires its register mutex first calls the native
retention callback, which records `board_fail` before returning to the USB
owner. Such a failure would retain that earlier caller, rather than `stop`.

The frozen session selects cold initialization, mode 1. Its baseline requires
PHY/reset bits `0xc0000000` at CCU `0x02003300` and device bits `0x01110111`
at `0x02003304` to be zero. The documented FEL state has `0x40000000` and
`0x01000100` respectively ([takeover contract](../designs/usb-session-takeover.md)).
This is an independent entry mismatch. Its retained-callback behavior prevents
attributing the observed `stop` address to that baseline without further data.
The existing ROM takeover mode requires explicit controller, interrupt and DMA
exclusion; the current cached-SMP session refuses that mode.

The frozen rail expectations are DCDC1/DC1SW2 at 3.30 V, DCDC9 at 1.20 V and
ELDO6 at 0.90 V. A [prior port-3-3 PMIC read](../power.md) reports DCDC9 at
1.24 V and ELDO6 at 0.80 V. Those are older programmed settings, not current
observations. This run's rail helper observes DCDC3 and DCDC5 only. A USB
snapshot mismatch would reach `stop` with the observed record shape.

## The resident timing fields do not select a failing USB guard

The contract diagnostic deadline is `UINT64_MAX`. Native initialization sets
the board deadline and USB provider deadline to `UINT64_MAX/24` microseconds.
`board_live` compares the counter against that deadline and `live_until`, and
tests the board fatal flag. It does not read `expires_at` or session seconds.
The provider deadlines use comparisons and bounded subtraction; the common
register mutex caps each scheduler wait at one tick.

No direct USB stop guard reads the lifetime word, watchdog mode or session
seconds. The lifetime selects the returning task bypass and USB product ID.
The supervisor owns the rolling four-second expiry and watchdog renewal.
The frozen mode `0x41` is the resident watchdog mode expected by that owner.

| Recorded time | Interpretation |
| --- | --- |
| RTC stop counter, 13.8148 s | `323785 * 1024 / 24000000`; counter time, not elapsed time since the launcher's arm timestamp. That timestamp is not retained. |
| Entire run, 24.845582 s | Includes reset, DDR preparation, rail helper and uploads; already less than the 55 s and 60 s bounds. |
| Launch stage, 9.627757 s | Includes uploads; does not measure native execution alone. |
| Native `exe` to first returned-ROM read, 5.896917 s | Host monotonic transaction starts `53593.123761007` and `53599.020678089`; includes watchdog recovery and enumeration. |

These timestamps cannot isolate a USB provider stage. They supply no evidence
of a 55-second or 60-second cutoff. A four-second expiry cannot directly make
the USB liveness comparison fail because it is not that comparison's operand.

## Linked cases demonstrate that the failure record is ambiguous

`checks/resident-linked.py` executes the frozen owner, native provider step,
failure recorder, supervisor and EL3 vectors. The modeled PMIC result is the
injected input. A PMIC-invalid result and a complete result with invalid
snapshot fields each produce identical retained USB failure words and
`0x01115fff`. Both cases match the physical failure words
`0xfa1b0400 / 0x80001053 / 0x41093870 / 0`.
No USB hardware register is accessed in either startup case.
The case does not identify which physical reply occurred.

Additional cases execute `board_live` at 13.8148, 17 and 67 counter seconds
under the actual resident deadline conversion. They remain live. A separate
case establishes successful completion, closes admission through the real
maintenance hook, and lets the USB liveness guard fail on core 1. It produces
`0x01115000` plus a USB failure. That ordinary race alone does not explain
the observed `0x5fff`; an additional fault could change this conclusion.

The host check passes thirteen cases on the unchanged frozen image. Results
are in `~/.cache/sunstoneos/build/card-boot-1111/usb0-stop-audit-linked/results.json`.
The old fixture supplied service timestamps and completion without executing
USB0 startup, so it could not reveal the missing provider observations.

## A nominal frozen resubmission requires a supported correction

No production guard is changed. The available record cannot distinguish the
pre-platform refusals, and neither fresh USB rail settings nor the complete
ROM takeover admission is available from this run. Changing a deadline guard
or weakening a voltage or cold-state guard would not follow from this evidence.

A narrower diagnostic can retain the failing USB guard, provider step and
PMIC result or snapshot mismatch in RTC before parking. Its prediction must
allow `0x5fff` while requiring the new reason record. That is a diagnostic
submission for coordinator review, not a qualified completion submission.
No new image is submitted and no board communication follows this audit.
