# Diagnose failure before NPU platform readiness

Observed on port 3-3: the startup-only ELF with SHA-256
`338d5df5d9c3739af66c23be86249e1ebd00f6b539fee858bac8a95fc6093c37`
returned through WDT0. The evidence is the `runs/01-startup-only/` directory
under the `npu-wdt-1031` build, especially `run/watchdog-report.json`,
`run/session.json`, `run/launcher-report-after-reset.bin` and `bundle/image.elf`.

Observed in that ELF: `llvm-addr2line-21 -f -i` resolves the call instruction
at `0x4100387c` to `bootstrap` in `apps/npu-convolution/native.c:425`.
The recorded caller, `0x41003880`, is its return address. Source line 91
is the shared `a733_npu_application_fail` recorder. Value 4 is the bootstrap
task's common failure reason. It does not identify a hardware fault code.

Observed in the client: retained value 9 records loss of liveness while
waiting for `a733_npu_application_ready()`. The client has not issued OPEN.
The first failure can arise from application health, allocation ownership,
the counter or deadline, platform startup, or service activation.
The old record cannot distinguish these paths. The controller's command
submission, interrupt completion and full-idle checks have not been tested
by this failure. The vendor and upstream sequence comparison does not
justify changing them from this evidence.

Observed timing: launcher start is counter `0x52204ee6`, or 57.410378 seconds
at the admitted 24 MHz frequency. The last retained timestamp is
`0x148e87 << 10`, or 57.480832 seconds. Retention followed launcher start
by about 70.45 milliseconds, with 42.7-microsecond breadcrumb resolution.
The host's 18.232 seconds starts after its watchdog-arm transaction and ends
after a new ROM instance passes access checks. Mode remained `0xb1`;
the image did not enter the EL3 stop that shortens the watchdog interval.

Inferred: the observed duration fits a 16-second watchdog interval plus
reset, ROM startup, USB re-enumeration and host observation time. Subtracting
16 seconds gives 2.232 seconds, but the record cannot partition that time
or determine the final restart instant. It does not demonstrate a watchdog
interval error or a 20-second application deadline expiry.

Implemented diagnostics preserve the existing hardware sequence and bounds:

- Bootstrap predicates record their own source line and instruction before
  the common failure reason. Values identify application state, counter
  failure, elapsed deadline, startup stage or activation result.
- Startup refusal and retention record the failing call site before cleanup.
  Failed existing register reads and comparisons retain their address offset
  and full 32-bit value. They perform no additional register reads.
- The shared returning transport selects the bundle's exact, hash-validated
  ELF for symbolization. It accepts both `board/image.elf` and `image.elf`.

Unknown: the original failing startup predicate. The next startup-only run
must either return `NPU_STARTUP=READY_CLOSED_STOPPED; inference_jobs=0`,
or identify the earlier predicate through the symbolized first-failure
record. A register offset is the low 16 bits of the physical address;
use the symbolized source expression to identify its register bank.
Retained value 9 can still follow that record and cannot overwrite it.
Do not run inference images until startup-only returns normally.
