# Cold CPU ownership through static OFF

Status: implemented, software validation in progress. This is a normal cold-card boot contract, not a returning diagnostic. Non-Arm exclusion remains an independent prerequisite. No physical operation is established by this design.

The stage2 loader must own all secondary cores before SD DMA and slot writes. An OFF sample alone is insufficient if the power controller can wake the core afterward. The owner uses the existing PPU protocol to acknowledge static OFF, retains that policy through loading, and releases it only after the resident OS has published a valid secondary entry. It does not assert CORE_RESET.

## Source contract

The A733 manual names interrupt 244 PCK600_CPU/CPUIDLE. The pinned A733 TF-A port maps core PPU status at `0x07052008 + core*0x1000`. Vendor BL31 uses those status words, the same hotplug requests and nine policy words one page apart. PCK-600 identifies itself with IIDR product `0x0b6`, implementer `0x43b`; PPU AIDR `0x11` identifies architecture 1.1. Runtime identity and capability checks must precede policy writes. A register value resembling a PPU is insufficient.

[Arm DEN0051E](https://documentation-service.arm.com/static/5f873177f86e16515cdb6d99), sections 3.4.3 and 4.1.1, supplies the ordering: disabling dynamic transitions takes effect after any in-flight transition, and PWSR acknowledges the change. Policy writes do not interrupt transitions. A denied transition reverts policy. Admission therefore needs matching policy readback, dynamic status disabled and actual OFF, under a fixed deadline. This standard contract, identified at the source-mapped instance with other writers excluded and hotplug requests clear, is the ownership boundary.

The A55 TRM 100442_0200_00 section A4.8 describes private-cache flushing and removal from coherency during normal powerdown. The corresponding [A76 r4p1 TRM 100798_0401_00](https://documentation-service.arm.com/static/5f562083235b3560a01e03bc), sections A4.6.2 and A4.9, specifies automatic L1/L2 flush and coherency removal on OFF. A wake may finish before static acknowledgment; final OFF is required even in that case. Its cache writes may reach DSU L3 after boot0's earlier cleanup, so stage2 must complete a fresh clean/invalidate through the documented hierarchy after all secondaries are held OFF.

## Fixed admission and handoff

1. Preserve the existing remote-writer check before CPU or SD mutations. Check local EL3/A55/cache-off entry and the documented three-level CLIDR hierarchy. Confirm the CPU configuration bus is enabled before accessing its core pages.
2. Preflight every core 1–7 without writes: PCK/PPU identity, required static and dynamic OFF/ON support, no emulation, no operating-mode policy or lock, matched OFF policy/status `0/0` or `0x100/0x100`, actual OFF and clear software start requests. Unknown profiles refuse before mutation.
3. Under one 100 ms absolute counter deadline, write static policy 0 on each core and wait for policy 0, dynamic-status 0 and actual OFF. An in-flight wake must finish and return to actual OFF; timeout or denial retains the attempted ownership. A finite poll cap also prevents a stopped counter from looping forever. The 100 ms limit and 65,536-sample cap are availability bounds, not claimed hardware latency.
4. With each core stably OFF, set and verify the existing GIC wake-disable bit while preserving unrelated fields. Recheck every held policy/request/status before memory admission.
5. Clean/invalidate the primary-visible L1D/L2 and shared DSU L3 with the existing architectural set/way sequence, under the same deadline. Validate cache geometry before operations. Preserve the selector and complete with DSB SY/ISB. This is justified only after all CPU and non-Arm writers have been excluded.
6. Retain static OFF through all slot reads, hashes and session publication. Once a policy write has been attempted, any terminal stage2 failure parks with the CPU ownership retained; it must not use the ordinary ROM return. Preserve the separate SD stop/drain before parking if SD has been acquired.
7. The resident OS treats the held policy as part of its fixed cold-entry ABI. Its existing mailbox owner arms the target before power startup. The resident-only startup path checks held policy and wake-disable before its first RVBAR/architecture write. After verification and the publication barrier, it restores dynamic policy `0x100`, waits for acknowledgment, and uses the existing hotplug ON/request cleanup. Only that target's policy is released. All shared entry data and its mailbox must be ready for an immediate wake. The existing target continuation clears GIC wake-disable. Other image profiles keep their existing path.

The pinned boot0 → single-monitor package is the predecessor contract. Stage2 starts from monitor bytes that boot0 has already loaded; this owner cannot retrospectively prove those bytes were protected. Prior non-Arm writes, including the CE ROM handoff under separate audit, remain outside its claim. The fresh cleanup protects subsequent stage2 loads only.

No new caller-supplied authority field or saved arbitrary policy is needed: only the two acknowledged OFF profiles are accepted. Any other policy or mismatched status is refused. The native register allowlist must admit the per-core policy word only for the resident cold profile.

## Scope and validation

Implementation ownership: stage2 CPU admission/helper and failure integration; resident-only `cpu_start.c` release steps; the corresponding board admission allowlist; component source pins and reference guidance. Session stamping and slot metadata stay with their existing owners. The NPU owner changes only loader `enter()` and will supply its final commit before the combined build.

The accepted combined stage2 text is 32,560 bytes, leaving 208 bytes below the fixed 32 KiB boundary. The new owner cannot fit in that margin. Use source-specific `-Os` for stage2 loader and admission/control code, retaining `-O2` for SHA-256 and the SD driver. Compare final linked size and the affected existing instruction-model counts; do not enlarge the reservation. No baseline rebuild precedes the reviewed implementation/configuration.

Focused checks will execute actual linked PPU admission against modeled identity, policy, delayed acknowledgment, wake-before-ack, denial, changed requests and deadline/counter failures. They will verify no memory admission before ownership, no ROM return after a policy write, bounded cache geometry, and resident release after entry publication. Reuse the existing SD and session cases outside changed branches. Build the final stage2 and one affected resident OS profile; no inference or performance sweep.

## Source identities and bounded failure reports

The [component manifest](../../boot/a733/stage2/cpu-sources.json) pins the Arm
manuals and points to existing A733, boot0 and BL31 identities. The PPU identity
accepts the documented PCK product and implementer with architecture 1.1; revision
fields may differ. Admission requires P-channel configuration, one operating
mode, static/dynamic OFF and ON, disabled emulation and the fixed unlocked
policy. Geometry must match the fitted A55 32 KiB L1D, 128 KiB L2 and 1 MiB
DSU L3 before the first cache operation.

The loader publishes status 10 for a clean CPU admission refusal before any
policy write. After a policy write, terminal failure publishes status 12 with
the original loader status in detail and parks. SD drain failure retains its
existing status 11 and takes precedence. Neither retained path redirects to
ROM. On successful entry, the resident OS owns the held policies. Its existing
watchdog failure path may reset the machine; physical reset/reboot reliability
remains unqualified.

## Component evidence and integration boundary

The focused evidence is under
`~/.cache/sunstoneos/evidence/cold-cpu-static-off-1026/`. Twenty owner cases cover
static acknowledgment, an in-flight wake, admission failures, changed requests,
deadlines, stopped counter, bounded cache cleanup and retained loader exits.
Three additional native cases cover already-static OFF and mismatched policy
acknowledgments. Resident startup and mailbox checks cover immediate arrival
while the target remains parked, zero writes on missing ownership, and terminal
retention. System-register, cache and MMIO effects are modeled.

The component stage2 image occupies 32,624 bytes after the A733 applicability
correction and static-profile addition. Its final combined NPU session extension
and resident eight-worker image require the coordinated integration build.
Source review and these component checks permit that build; they do not establish
physical power transitions, DMA coherence, watchdog recovery or autonomous boot.
