# Returning probes acquire secondary CPU ownership

**Status: host-tested.** The acquisition in `platform/a733/probe.c` is checked
by linked instruction models. Physical transitions and repeatability are not
qualified. The contract covers returning, single-core diagnostics.

The shared probe acquires all seven secondary CPUs after session, watchdog and
architectural checks, before cache maintenance. It accepts acknowledged static
or dynamic OFF as input and establishes static OFF with GIC wake disabled. It
retains that state until the ROM reset request. An active core refuses before
any policy write, because entry cannot shut down an unknown owner.

## Dynamic disable needs an explicit handshake

The vendor BL31 clears the low policy nibble of nine CPUIDLE words at binary
offset `0x4e88`, which preserves dynamic-enable bit 8. Its CPU_ON path waits
for OFF, programs per-core mode and request, waits for ON and clears both
requests. CPU_OFF disables that core's GIC wake. The A733 BSP TF-A path
corroborates those addresses and operations.

Arm DEN0051E sections 3.4.3 and 4.1.1 define the completion condition.
Disabling dynamic transitions takes effect after any in-flight transition, and
PWSR acknowledges the change. Entry waits for dynamic status disabled and
actual OFF before it checks PWPR readback. A rejected transition or a timeout
parks with WDT0 armed and the first raw failure retained. Verifying policy
before the acknowledgment would reject a delayed acknowledgment.

The [CPU source manifest](../../boot/a733/stage2/cpu-sources.json), [CPU
comparison](../reference-backsolve-cpu.md) and [vendor monitor
inventory](../vendor-firmware/tf-a-monitor.md) pin the source identities,
binary offsets and licenses.

## Only secondary-core registers change

Preflight reads all seven identities, architectures, capabilities, policies,
operating modes, status words and software requests before the first write.
It admits only matched policy and status `0/0` or `0x100/0x100`, operating
mode zero and clear software requests. Either inherited GIC wake setting is
accepted.

Acquisition writes zero only to the dynamic policy words at
`0x07053000..0x07059000`, one page apart, each completed with DSB SY. PWSR may
temporarily report OFF or ON with dynamic enable still active; only zero
establishes completion, and PWPR verification follows. Entry then sets bit 1 of
the per-core hotplug control at `0x07050204..0x0705021c`, preserving every
other bit and completing with DSB SY. The held-state predicates and their
short-circuit read order are those of `secondaries_off()`.

No write targets CPU0 at `0x07052000`, the cluster page at `0x07051000`, a
power domain, a rail, a clock or a reset. The `PROBE_POLICY` RTC breadcrumb
records `core << 12 | inherited_policy` before each core's normalization.
Later progress overwrites that stage, so the diagnostic words keep a separate
summary: with FAULT clear, ELR marker `0x50505531` identifies ESR as the
inherited dynamic-OFF core mask and FAR as the completed static-OFF core mask.
Bits 1-7 identify the secondary cores; other inherited policies are zero. A
real first fault replaces these words with its raw inputs before it publishes
FAULT.

One 100 ms absolute counter deadline, capped by the session deadline, bounds
acquisition. A 65,536-sample limit bounds a stopped counter. These limits
bound availability and are not measured hardware latency. Acquisition runs once
at entry and adds no recurring I/O or scheduler work.

## Ownership stays held until the ROM reset request

The weak acquire hook implements normalization. Its code runs before memory
testing and stays outside memtest's protected recovery pages. The weak release
hook returns READY without writes. The protected return closure contains the
held-state checks, fault path and release hook within its four-page
reservation. All data and instruction-cache maintenance precedes the optional
release call; the ROM tail contains no cache instruction.

A subsequent multicore consumer publishes its target entry before it releases
the held policy; the resident startup path implements that handoff. Nonresident
launch images are not established compatible with retained static OFF.
Consumers selecting the common owner remove private strong owner hooks and
build-time replacements for the shared runtime.

Peripheral reset and gate normalization belong to the block's owner before its
first dependent access; common entry has no peripheral ownership authority. The
camera owner controls VIDEO_IN and ISP reset sequencing, preserves shared
resources and establishes drain before reset, which a global reset from common
entry cannot supply.

## Validation

Linked instruction checks model delayed PWSR acknowledgment, stale PWPR,
in-flight wake, changing requests, invalid last-core identity, deadlines and a
stopped counter. They check all seven cores, the exact register write scope,
preservation of the hotplug upper fields and every held-state predicate.
CPU-origin cases keep their first sampled inputs. Memtest checks verify
protected placement, relocation closure, stack bounds and beacons.

| Item | Status | Record |
| --- | --- | --- |
| Acquisition on linked instructions | host-tested | [probe-entry-normalization.jsonl](../evidence/probe-entry-normalization.jsonl), `probe-entry-normalization-host-models` |
| Physical transitions and acquisition latency | open | `probe-entry-normalization-physical-proof` (not run) |

## Open work

1. Run audio control, a scan-only memory probe and audio control on port 3-1
   without a replug. Each run passes common entry and retains the inherited
   policies it normalizes. This measures acquisition latency on hardware.
2. Camera reset and gate normalization stay with the camera owner.
