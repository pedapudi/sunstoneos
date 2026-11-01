# Returning probe secondary CPU ownership

Status: implemented, host-checked and approved by three independent reviewers.
Physical control and audio runs remain pending.

The single-core audio probe needs secondary CPUs held OFF during whole-hierarchy
cache maintenance. CPU1 policy reads `0x100` after the runner's watchdog reset.
That is acknowledged dynamic OFF only if PWSR also reads `0x100`; it allows
hardware-initiated wake. The returning probe therefore saves acknowledged OFF
policies, temporarily holds static OFF and restores the original policies.

## Reference comparison

- `checks/board-runner-probe.S` records entry and parks. Its expected result is
  watchdog reset, not a normally returned native-probe receipt.
- The memtest worked example links the shared probe and has the same static-OFF
  prerequisite. Its documentation and host admission establish no physical run.
- `apps/cpu-runtime-qualification/runtime.c` accepts actual OFF and clear software
  requests, saves configuration, starts controlled workers and retires their
  cache ranges individually. It does not read PWPR. Its successful return does
  not establish exclusion for the generic probe's whole-hierarchy maintenance.
- Vendor BL31 `8af16b86` at file offsets `0xc320`, `0xc350` and `0xc35c` binds
  per-core status and software-start requests. Its policy helper at `0x4e88`
  clears bits 3:0 while preserving dynamic-enable bit 8. It does not acquire
  static OFF. The matching A733 TF-A source `5bc9605`, function `sunxi_cpu_off`
  in `sunxi_native_pm_ncat2.c`, clears the power-mode request and disables GIC
  wake; `sunxi_pwr_domain_on` waits actual OFF before software start.
- Arm DEN0051E sections 3.4.3 and 4.1.1 define the stronger ownership operation:
  clear PWR_DYN_EN, wait for PWSR to acknowledge dynamic disable and actual OFF,
  then verify PWPR. A policy write cannot interrupt an in-flight transition.
  The existing stage2 CPU owner implements this protocol for cold boot.

Source URLs, hashes and licenses are pinned in `sources.lock.json`,
`docs/vendor-firmware/tf-a-monitor.md` and `boot/a733/stage2/cpu-sources.json`.
These are host-side references; this change incorporates no vendor binary code.

## Fixed operation and budgets

The shared probe gains two weak hooks for its current returning consumer.
Their defaults preserve the existing already-owned entry contract. The audio
image implements acquisition after session, watchdog and architectural entry
validation, before the existing secondary-OFF check and first cache operation.
The session deadline is initialized before acquisition. The image implements
release after final cache cleanup and receipt preparation, before watchdog
disarm, the completion sentinel and ROM return. Move the ROM tail's local I-cache
invalidation before release too. No cache operation follows release.

Preflight every secondary without writes. Require known PCK-600 identity,
architecture 1.1, existing capabilities, PMER zero, matching PWPR/PWSR of zero
or `0x100`, clear software-start requests and GIC wake already disabled.
These retain the existing contract except for permitting an owned transition
from acknowledged dynamic OFF. Save original policy and request/control words
outside EL1 BSS on dedicated monitor cache lines. Do not change control or mode,
RVBAR, CPU0, cluster/DSU policy, resets, clocks or rails.

For each dynamic policy, write zero with DSB before polling. Wait for PWSR zero
before reading PWPR for verification. Recheck the unchanged wake-disable and
clear software requests. All seven secondaries must be held before cache work.
Each per-core operation writes only that core's PWPR. A CPU1-only fixture permits
only address `0x07053000`; aggregate ownership may write all seven core policies.

Keep static OFF until device and interrupt retirement, final whole-cache cleanup
and receipt preparation finish. Restore only policies that changed, with DSB and
matching PWSR completion before PWPR verification. Preserve the inherited vendor
CPU_OFF profile: GIC wake remains disabled and software requests remain clear.
The inherited vendor CPU_OFF state is a predecessor condition supplied by the
existing reset/ROM path, not authority created by these snapshots. Restoring it
ends static ownership. No CPU entry address or architecture word changes.
Recheck actual OFF and saved
controls, then complete the existing uncached EL3 return tail. Dynamic policy
restoration ends the probe's static ownership; no memory reuse or cache operation
occurs after that handoff. This does not qualify every possible external wake.

Acquisition and release each have one aggregate 100 ms deadline and a 65,536-poll
cap. A monotonic counter check rejects backwards time; the poll cap handles a
stopped counter. Neither loop feeds the watchdog. Existing WDT0 and the 20-second
session remain unchanged. Record failed site, observed value and register address
before parking. Any failure after a write attempt retains board ownership and parks for watchdog
recovery: no rollback, automatic retry, successful receipt or normal ROM return.

## Host checks and review gate

Execute linked native instructions with delayed PWSR acknowledgment and stale
PWPR readback until completion. Cover matched dynamic OFF, unchanged static OFF,
in-flight ON then final OFF, denial, missing acknowledgment, late completion,
stopped/reversed counter and changed requests. Verify fault origin and first-fault
retention. Check acquire/restore ordering around actual cache maintenance and WDT
writes. Verify CPU1-only writes and byte-identical neighboring PPU/cluster pages;
then verify all-seven save, hold and exact restoration. Compare modeled elapsed
ticks and register writes before/after; these are not physical latency evidence.

Three independent reviewers assess this design before implementation and the
same three review code and checks before any run. Build one ELF with separate
control, internal-loopback and overrun inputs. Submit control first; dependent
runs require its normal return. If the queue daemon is stopped, retain exact
commands and end the turn without waiting or polling.

## Evidence

All fourteen CPU ownership cases pass against the final linked image under
`~/.cache/sunstoneos/build/audio-1031/policy/cpu-final/`. The return path executes
18,944 data-cache operations and one instruction-cache invalidation before
release. Fourteen existing native entry, restoration and constructor cases pass
under `policy/native-final/` in the same build root. Default weak hooks remain
inside memtest's protected recovery closure; its link, relocation, stack and
layout checks pass under `policy/memtest-closure/`.

The counter model advances 240 ticks per counter read. Static, CPU1-only and
all-seven cases consume 48,520, 75,880 and 240,040 modeled ticks, with zero, two
and fourteen policy writes respectively. These counts characterize the fixture,
not physical transition latency. The two aggregate 100 ms limits remain fixed.
Panel decisions and artifact hashes are under `policy-review/` in the build root.
