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
   barrier establishes ordering; a device completion signal establishes progress.
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

Select checks under [the validation policy](validation.md). Keep hardware
safety checks when consolidating fixtures. Submit eligible physical tests
through [the board runner](board-runner.md) and use
[the probe contract](board-probe-contract.md) for native diagnostics.
