# Returning probes acquire secondary CPU ownership

Status: implemented and host-checked. Physical transitions and repeatability
remain unqualified. This contract covers returning, single-core diagnostics.

The shared probe acquires all seven secondary CPUs after session, watchdog and
architectural checks, before cache maintenance. It accepts acknowledged static
or dynamic OFF as input and establishes static OFF with GIC wake disabled.
It retains that state through ROM return. An active core refuses before any
policy write, because entry cannot shut down an unknown owner.

## The vendor sequence needs an explicit dynamic-disable handshake

The vendor BL31 clears the low policy nibble of nine CPUIDLE words at binary
offset `0x4e88`. That operation preserves dynamic-enable bit 8. Its CPU_ON path
waits for OFF, programs per-core mode/request, waits for ON and clears both
requests. CPU_OFF disables that core's GIC wake. The A733 BSP TF-A path
corroborates those addresses and operations.

Arm DEN0051E sections 3.4.3 and 4.1.1 define the missing completion condition.
Disabling dynamic transitions takes effect after any in-flight transition;
PWSR acknowledges the change. Entry waits for dynamic status disabled and actual
OFF before checking PWPR readback. A rejected transition or timeout parks with
WDT0 armed and the first raw failure retained.

The [CPU source manifest](../../boot/a733/stage2/cpu-sources.json),
[CPU comparison](../reference-backsolve-cpu.md) and
[vendor monitor inventory](../vendor-firmware/tf-a-monitor.md) pin the source
identities, binary offsets and licenses. The older stage2 helper's policy-first
verification order is unsuitable for a delayed acknowledgment.

## Only secondary-core registers change

Preflight reads all seven identities, architectures, capabilities, policies,
operating modes, status words and software requests before the first write.
It admits only matched policy/status `0/0` or `0x100/0x100`, operating mode zero
and clear software requests. Either inherited GIC wake setting is accepted.

Acquisition writes zero only to dynamic policy words at
`0x07053000..0x07059000`, one page apart. Each write completes with DSB SY.
PWSR may temporarily report OFF or ON with dynamic enable still active; only
zero establishes completion. PWPR verification follows that completion.
Entry then sets bit 1 of per-core hotplug control at
`0x07050204..0x0705021c`, preserving every other bit and completing with DSB SY.
The original strict held-state predicates and short-circuit read order remain.

No write targets CPU0 at `0x07052000`, the cluster page at `0x07051000`, a power
domain, a rail, a clock or a reset. The `PROBE_POLICY` RTC breadcrumb records
`core << 12 | inherited_policy` before each core's normalization. A fault
retains its raw rejected register and address independently.

One 100 ms absolute counter deadline, capped by the session deadline, bounds
acquisition. A 65,536-sample limit also bounds a stopped counter. These limits
bound availability; they do not claim measured hardware latency. Acquisition
runs once at entry and adds no recurring I/O or scheduler work.

## Ownership stays held through ROM return

The weak acquire hook implements normalization. Its code runs before memory
testing and stays outside memtest's protected recovery pages. The weak release
hook returns READY without writes. The protected return closure contains the
held-state checks, fault path and release hook within its four-page reservation.
All data and instruction-cache maintenance precedes the optional release call;
the ROM tail contains no cache instruction.

A subsequent multicore consumer must publish its target entry before releasing
the held policy. The resident startup path implements that handoff. Historical
nonresident launch images are not established compatible with retained static
OFF. Consumers selecting the common owner remove private strong owner hooks
and build-time replacements for the shared runtime.

Peripheral reset and gate normalization belongs to the block's owner before
its first dependent access. Common entry has no peripheral ownership authority.
In particular, the camera owner controls VIDEO_IN and ISP reset sequencing.
It must preserve shared resources and establish drain before reset; a global
reset from common entry cannot supply that proof.

## Validation separates modeled behavior from physical proof

Linked instruction checks model delayed PWSR acknowledgment, stale PWPR,
in-flight wake, changing requests, invalid last-core identity, deadlines and a
stopped counter. They check all seven cores, exact register write scope,
preservation of hotplug upper fields and every original held-state predicate.
CPU-origin cases retain their first sampled inputs. Existing memtest checks
verify protected placement, relocation closure, stack bounds and beacons.

Three independent reviewers inspect design and code; linked memory-test and
audio images receive separate inspections. The full integration gate and the
check index cover software integration. Acquisition latency remains unmeasured
on hardware. The physical proof requires audio, camera control and audio on one
port without a replug, with common entry passing each time. A later camera
reset refusal stops that sequence and remains with the camera owner.
