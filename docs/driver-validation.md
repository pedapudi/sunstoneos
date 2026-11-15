# Validate driver completion and timing

Driver authors and check authors need the device's register specification and
the vendor's programming sequence. A successful write does not establish that
the hardware has applied it. The CPU PLL permits its multiplier to read back
the previous value while UPDATE remains set. Verifying that multiplier first
can latch a fault during a valid transition.

1. **Wait before verifying.** After requesting a change, wait for its documented
   completion signal, then verify the resulting state. Use a deadline. Name the
   condition beside the wait: UPDATE clears, LOCK asserts, BUSY clears, READY
   asserts, or a specified minimum settle interval expires. A posted-write
   barrier completes the CPU-side transaction; a device completion signal
   establishes device progress.
   Start a minimum hold after its triggering write. A clock rounded down to
   whole microseconds needs an extra unit before it can prove that hold elapsed.
2. **Model the permitted delay.** Keep old readback fields visible until the
   documented update completes. Separate the write request, its acknowledgment
   and subsequent readiness. Use explicit fixture time and the shared
   [C](../checks/register_latency.h) or [Python](../checks/register_latency.py)
   deferred register effect. Exercise delayed and stuck completion. Removing
   the driver's wait must make a check fail for the intended reason.
3. **Exercise the workload's transitions.** Use the workload's entry points and
   resource ownership. Raise and lower rates; park and restore clocks; stop and
   restart engines. Repeat timing-dependent paths with different completion
   delays. One successful pass does not qualify them. Host models establish
   software behavior; physical qualification requires recorded board runs.
4. **Retain the first fault's origin.** Record the check that latched the fault,
   the register or operation, observed and expected values, completion status
   and deadline inputs. Preserve that record when later operations refuse to
   proceed. A later refusal alone cannot identify the original failure.
5. **Establish a control first.** Run the same image and input with the new
   subsystem disabled before diagnosing inside it. Compare the first divergent
   checkpoint. A failing control leaves startup or shared resources unresolved.
6. **Keep the vendor's write ordering.** Complete device writes with DSB before
   the next dependent write or poll. Preserve every vendor inter-write delay.
   CPU boot0 waits 20 µs between writing PLL N and requesting UPDATE; removing
   that interval can leave the old N after UPDATE clears. Start the interval
   after the completed write, and account for timer quantization. DSB completes
   the architectural transaction; an interconnect may acknowledge Device-nGnRE
   writes early, so DSB does not replace device status or settle requirements.
   Model posted writes separately from device application and readiness.
7. **Use only general registers in interrupt code.** In the A733 images that
   `checks/irq-general-regs.py` walks, IRQ entry does not save the FP and SIMD
   registers (`configSS_IRQ_SAVES_FP` is 0 through the CMake option
   `SUNSTONE_IRQ_FP_UNSAVED`), so an interrupt that changes one corrupts the
   interrupted task's float state. Every other build keeps the save, including
   a hand-built board image configured without the option. Every function an interrupt reaches is
   compiled with `-mgeneral-regs-only`: the driver's ISR, everything it calls,
   `vApplicationIRQHandler`, the scheduler service and the panic path. The flag
   also keeps the compiler from using SIMD registers for an inlined `memcpy`,
   `memset` or structure copy. An ISR must not call newlib's `printf` family,
   `memchr` or other library routines that use SIMD registers, and must not
   contain FP or SIMD assembly. This holds for camera, USB, NPU and every
   other device class; put float work in the task that the ISR notifies.
   [`checks/irq-general-regs.py`](../checks/irq-general-regs.py) walks each
   image linked with 0 from its EL1 vector table and fails on any FP or SIMD
   instruction it reaches, apart from the port's guarded task context save
   and restore, and on any reached unit compiled without the flag.
   Application sources (`add_sunstone_application`) are built without the
   flag, so an application task can use NEON; every task has its own FP
   context. The walk leaves out the application's init hook and main, which
   only boot code and the application task run, and refuses any other
   application function it reaches that uses FP or lacks the flag.

Select checks under [the validation policy](validation.md). Keep hardware
safety checks when consolidating fixtures. Submit eligible physical tests
through [the board runner](board-runner.md) and use
[the probe contract](board-probe-contract.md) for native diagnostics.
The [driver completion audit](driver-completion-audit.md) lists the register
sites, references and fixture evidence across the driver tree.
