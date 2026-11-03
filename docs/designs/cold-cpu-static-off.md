# Cold CPU ownership through static OFF

Status: **implemented; host-tested** by focused software checks. Physical cold-card startup is unqualified; the returning controls recorded in [the CPU status](../status/cpu.md) show static OFF policies on the board. This is a normal cold-card boot contract, not a returning diagnostic. Non-Arm writer exclusion is an independent prerequisite.

The stage2 loader owns all secondary cores before SD DMA and slot writes. An OFF sample alone is insufficient if the power controller can wake the core afterward. The owner uses the PPU protocol to acknowledge static OFF, retains that policy through loading, and releases it only after the resident OS has published a valid secondary entry. It does not assert CORE_RESET.

## Source contract

The A733 manual names interrupt 244 PCK600_CPU/CPUIDLE. The pinned A733 TF-A port maps core PPU status at `0x07052008 + core*0x1000`. Vendor BL31 uses those status words, the same hotplug requests and nine policy words one page apart. PCK-600 identifies itself with IIDR product `0x0b6`, implementer `0x43b`; PPU AIDR `0x11` identifies architecture 1.1. Runtime identity and capability checks precede policy writes, because a register value resembling a PPU is insufficient.

[Arm DEN0051E](https://documentation-service.arm.com/static/5f873177f86e16515cdb6d99), sections 3.4.3 and 4.1.1, supplies the ordering: disabling dynamic transitions takes effect after any in-flight transition, and PWSR acknowledges the change. Policy writes do not interrupt transitions, and a denied transition reverts policy. Admission therefore needs matching policy readback, dynamic status disabled and actual OFF, under a fixed deadline. This contract, identified at the source-mapped instance with other writers excluded and hotplug requests clear, is the ownership boundary.

The A55 TRM 100442_0200_00 section A4.8 describes private-cache flushing and removal from coherency during normal powerdown. The [A76 r4p1 TRM 100798_0401_00](https://documentation-service.arm.com/static/5f562083235b3560a01e03bc), sections A4.6.2 and A4.9, specifies automatic L1/L2 flush and coherency removal on OFF. A wake may finish before static acknowledgment, and final OFF is required even then. Its cache writes may reach DSU L3 after boot0's earlier cleanup, so stage2 completes a fresh clean/invalidate through the documented hierarchy after all secondaries are held OFF.

## Admission and handoff

The implementation is `boot/a733/stage2/cpu.c`, called from `loader.c`, with the resident release in the resident-only path of `platform/a733/cpu_start.c`.

1. The existing remote-writer check runs before CPU or SD mutations. The loader checks local EL3/A55/cache-off entry and the documented three-level CLIDR hierarchy, and confirms the CPU configuration bus is enabled before accessing core pages.
2. Preflight of every core 1 to 7, without writes: PCK/PPU identity, required static and dynamic OFF/ON support, one operating mode, no emulation, no operating-mode policy or lock, matched OFF policy/status `0/0` or `0x100/0x100`, actual OFF and clear software start requests. Unknown profiles refuse before mutation. Geometry must match the fitted A55 32 KiB L1D, 128 KiB L2 and 1 MiB DSU L3.
3. Under one 100 ms absolute counter deadline, write static policy 0 on each core and wait for policy 0, dynamic-status 0 and actual OFF. An in-flight wake must finish and return to actual OFF; timeout or denial retains the attempted ownership. A cap of 65,536 samples also stops a stopped counter from looping forever. The limits are availability bounds, not claimed hardware latency.
4. With each core stably OFF, set and verify the GIC wake-disable bit, preserving unrelated fields. Recheck every held policy, request and status before memory admission.
5. Clean/invalidate the primary-visible L1D/L2 and shared DSU L3 with the architectural set/way sequence, under the same deadline, then DSB SY and ISB. This is justified only after all CPU and non-Arm writers have been excluded.
6. Static OFF is retained through all slot reads, hashes and session publication. Once a policy write has been attempted, any terminal stage2 failure parks with the CPU ownership retained and never uses the ordinary ROM return. A clean admission refusal before any policy write publishes status 10. A terminal failure after a policy write publishes status 12 with the original loader status in detail. SD stop/drain failure publishes status 11 and takes precedence.
7. The resident OS treats the held policy as part of its cold-entry ABI. The mailbox owner arms the target before power startup. The startup path checks held policy and wake-disable before its first RVBAR/architecture write; after verification and the publication barrier it restores dynamic policy `0x100`, waits for acknowledgment, and uses the hotplug ON/request cleanup. Only that target's policy is released. All shared entry data and the mailbox are ready for an immediate wake. The target continuation clears GIC wake-disable. Other image profiles keep their own path.

Only the two acknowledged OFF profiles are accepted; any other policy or mismatched status is refused. The native register allowlist admits the per-core policy word only for the resident cold profile. The resident OS's watchdog failure path may reset the machine; physical reset reliability is unqualified.

The pinned boot0 to single-monitor package is the predecessor contract. Stage2 starts from monitor bytes boot0 has already loaded, so this owner cannot prove those bytes were protected. Prior non-Arm writes, including the CE ROM handoff under separate audit, are outside its claim; the fresh cleanup protects subsequent stage2 loads only.

## Size and sources

Stage2 text must stay within the fixed 32 KiB boundary: loader, admission and the cold CPU helper build with `-Os`, SHA-256 and the SD driver with `-O2`. The combined image has 80 bytes of headroom ([record](../evidence/cold-cpu-static-off.jsonl), `cold-cpu-static-off-component-evidence`). The [component manifest](../../boot/a733/stage2/cpu-sources.json) pins the Arm manuals and points to the A733, boot0 and BL31 identities. The PPU identity accepts the documented PCK product and implementer with architecture 1.1; revision fields may differ.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| CPU owner cases against the combined stage2 | 23 pass | `cold-cpu-static-off-component-evidence` |
| Native address-permission checks | 41 pass | same |
| Combined stage2 text | 32,688 bytes, 80 below the boundary | same |

Cases cover both admitted OFF profiles, an in-flight wake, invalid identities and acknowledgments, changed requests, deadlines, a stopped counter, bounded cache cleanup and retained exits. System-register, cache and MMIO effects are modeled.

## Limitations and open work

1. Physical power transitions, DMA coherence, watchdog recovery and autonomous cold boot are not qualified.
2. The pre-stage2 predecessor (boot0, CE ROM handoff) and remote-writer requirements remain separate prerequisites.
