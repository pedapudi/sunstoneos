# NPU domain OFF and restart diagnostic

The startup sequencer implements a bounded domain-4 OFF/ON cycle after a
successful controller close and local stop. Host models cover the provider,
startup tables and ownership ledger. Physical domain-4 power cycling and
inference after that cycle remain unqualified.

## Close, stop, power off, then reopen

The caller completes controller END, output copy and reap, idle checks and
CLOSE before stopping the startup provider. CLOSE disables engine interrupts
and translation with exact readback; the service then retires GIC interrupt
97 and parks. The caller must prevent new service clients throughout restart.

`a733_npu_startup_stop()` closes software register admission, disables the
module and bus gates, asserts core, AXI and AHB resets and routes shared SRAM
back to the CPU. Only the bound bootstrap task can request the power cycle.

Call `a733_npu_startup_power_off()` until it returns READY. It checks STOPPED,
closed software admission and the complete stopped register state before
queuing OFF on the existing counted row. The power service initializes and
checks the vendor delay words before its first OFF command. It drops the
row's rail pin only after timely completion. Domain 1 still refuses OFF.

A released row does not prove physical OFF: another counted row can keep the
domain ON. The sequencer separately requires coherent policy and status zero,
plus the existing AHB, MBUS and PPU clock checks. A competing live row or a
closed master gate therefore causes terminal retention. Domain 4 receives no
GPU gate exemption.

After OFF READY, call `a733_npu_startup_power_restart()` until READY. It
queues ON using the same row identity and a fresh request generation. The
power service repins that row through its existing ledger. The sequencer
requires coherent policy and status eight and the full gate checks, then
reuses the ordinary stopped restart, adopted PLL qualification and local
opening. It advances the remaining startup steps internally. The caller
must reopen and configure the controller, republish translation tables and
bootstrap, and require a fresh interrupt before submitting another job.

Both APIs return PENDING during service waits or temporary provider
contention. Shared-provider BUSY leaves the request unchanged. RETAINED is
terminal; the caller stops feeding the watchdog and performs no retry.
Failures preserve remaining clock, rail, PCK, row, buffer and register-lock
ownership, including uncertainty after a failed unpin. A healthy second
CLOSE and local stop can return normally with domain 4 still ON.

## Preserve the operating point and bound each transition

The cycle changes PCK domain policy and its counted row pin only. DCDC2
remains at the inherited 800 mV; the selected PLL_NPU rate and shared leases
remain owned. It changes no PMIC selector, rail enable or PLL rate. The finite
hardware proposal selects the vendor's 852 MHz operating point.

The power owner requires each acknowledgement strictly before 10,000 us from
the policy write and before its existing session deadline. A status read
that crosses either bound fails. Startup also checks the original absolute
session deadline before accepting either transition. Retained ownership has
no expiry that could authorize another caller to use uncertain resources.
The existing PLL qualification, 20 us settle and 2 ms local settle remain.
The finite diagnostic retains its bootstrap and job budgets of 1 s each and
its total session budget of 20 s.

The startup fields `power_off_request_us`, `power_off_ack_us`,
`power_on_request_us` and `power_on_ack_us` use counter microseconds.
Request timestamps precede service processing; acknowledgement timestamps
follow the sequencer's physical policy/status and gate checks. Their
difference measures request-to-observation latency, including scheduling.
It does not measure the owner's policy-write-to-acknowledgement interval.
The `power_off_policy`, `power_off_status`, `power_on_policy` and
`power_on_status` fields record the coherent low nibbles accepted by those
checks. Finite diagnostics record breadcrumbs for OFF request, OFF accepted,
ON request, ON accepted and restart READY.

## Vendor ordering permits local stop before OFF

The pinned VIP2 source calls `pm_runtime_put()` before `npu_clk_uninit()` in
`vip_drv_device_platform.c`, lines 433–436. The runtime header defines put
with `RPM_ASYNC`, lines 460–462, so the call does not wait for domain OFF.
Runtime work can complete the PCK transition after local clocks stop.

The pinned Allwinner `pck600_domains.c` callback writes the policy nibble,
executes DSB SY and polls status for at most 10,000 us, lines 104–124.
It accesses no engine register or local clock. Its independent PPU clock
must remain available. Delay initialization is at lines 126–142 and 269.
These sources are pinned by revision and hash in
[the source manifest](../../sources.lock.json) and audited in
[the NPU reference record](../../docs/reference-backsolve-npu.md).

The vendor clock initialization precedes synchronous domain resume on ON.
SunstoneOS preserves its existing, physically observed startup order:
confirm domain ON before local engine opening. That earlier evidence does
not establish successful restart after domain loss.

## Host coverage and physical evidence gap

`checks/npu-startup-host.c --power-cycle` exercises repeated complete cycles,
fresh row generations, a competing live row, late, stalled and invalid
acknowledgements, stopped-state and software-window guards, lost ownership,
partial reads, failed unpin, closed master gates and session expiry.
It tests contention in all three shared providers before and during both
requests and checks that policy writes occur with local clocks stopped.
Clock, rail and PCK identities stay fixed; PLL and PMIC write counts stay
unchanged across the cycle.

`checks/power-domains-pck.c` covers domain-4 delay-word I/O and readback
failures, exact transition deadline boundaries, clock failure and regression,
plus successful OFF/ON. The existing GPU transition and delay checks remain
applicable. The domain-4 OFF regression fails against the original provider
and passes with the extended counted release.

Clang 21 builds with AddressSanitizer and nonrecovering UndefinedBehaviorSanitizer
pass the power-cycle, existing startup and provider modes. Source inspection
and host models establish software behavior only. Physical acceptance still
requires a frozen image and Git revision, retained breadcrumbs and counter
measurements, coherent OFF then ON, controller bootstrap, a fresh interrupt
and matching inference after restart. The coordinator runs that bounded
RAM-only diagnostic under the host-armed watchdog without automatic retry.
