# Resident OS entry from the USB boot ROM

Status: the bounded resident test and runner are host-tested. The static-OFF
procedure is proposed and unimplemented. No resident board run is qualified.
The [autonomous card design](autonomous-card-os.md) remains the card-boot contract.
This procedure supplies its secondary CPU hold when the host loads a resident
image through FEL, the boot ROM's USB loader.

## Static OFF precedes native entry

The proposed SRAM launcher validates the request, session framing, image identity,
host-armed WDT0, zero FEL flag and all three zero redirect fields before changing
any CPU policy. It checks the CPU configuration
bus gate, then preflights every secondary power page before its first write.
Each page must identify PCK-600, PPU architecture 1.1, static and dynamic OFF/ON
support, one operating mode, no emulation and no operating-mode policy or lock.
Both hotplug requests must be clear. Policy and status must match at zero or
`0x100`, with the core OFF.

The launcher writes static policy zero for cores 1 through 7. It requires policy
readback zero, dynamic acknowledgment disabled and actual OFF before setting
that core's GIC wake-disable bit. It preserves unrelated control bits and
rechecks every held core before publishing the resident contract. A failure
after the first write retains the hold and parks under WDT0. The procedure
never restores dynamic policy on refusal.

The acknowledged hold remains until the existing mailbox owner publishes the
target's entry and architecture. The resident startup then restores policy
`0x100`, waits for dynamic acknowledgment and performs the existing hotplug
request sequence (`platform/a733/cpu_start.c`). It releases one core at a time.

Implementation requires extraction of the policy sequence from
`boot/a733/stage2/cpu.c`, with its two-sided MMIO barriers. The stage-2 cache
cleanup remains specific to cold-card ownership. The AArch32 launcher requires
stack-local state because its linker script forbids writable static sections.

The candidate hold occurs after image upload. A secondary wake can flush private
cache lines into the shared cache before static OFF is acknowledged. The existing
FEL publication evidence does not establish safety for that interval. Admission
requires either exclusion before upload or an independent predecessor proof
that excludes those writers. This evidence gap blocks power-state implementation
and a resident submission; the policy helper alone cannot close it.

## Reference sequence and implementation boundary

The [cold CPU design](cold-cpu-static-off.md#source-contract) and
[source manifest](../../boot/a733/stage2/cpu-sources.json) pin Arm DEN0051E
sections 3.4.3 and 4.1.1, the PCK-600 identification register, and the A733
vendor BL31 CPU power map and hotplug sequence. Disabling dynamic transitions
takes effect after any in-flight transition; actual OFF and acknowledgment are
therefore separate checks. Vendor CPU_ON publishes entry before requesting ON.

SunstoneOS adds identity and capability checks, a preflight of all seven cores,
bounded acknowledgment, GIC wake exclusion and retained failure. The shared
policy helper must preserve the stage-2 register sequence. The returning launcher
does not call it. The resident launch requires coordinator approval and an
immutable bounded-test marker in the ELF.

## Time and size budgets

The hold has one 100 ms counter deadline and a 65,536-sample backstop for a
stopped counter, matching stage 2. These are availability limits, not measured
transition latency. WDT0 is already armed in mode `0x41`, configuration 1 and
output 2. The implemented resident launcher timestamps immediately before one
WDT0 CTRL restart, verifies mode, configuration and output, and bounds publication
and handoff to one millisecond. The proposed hold precedes that restart.
Native startup retains its one-second deadline and initial contract expiry four
seconds after the restart timestamp. Physical expiry remains unqualified.

The implemented resident launcher occupies 2,500 bytes of its 3,072-byte
reservation ([record](../evidence/resident-fel.jsonl),
`resident-fel-host-contract`). The proposed hold has 572 bytes of remaining
space. Host checks must measure that bound and the stage-2 32 KiB initialized-image boundary after
the shared sequence is extracted. The bounded test uses the existing
hello-compute application, whose manifest permits two seconds of work and a
one-second progress window. A diagnostic build stops renewal after completion;
ordinary resident builds continue supervising services after completion.

The diagnostic supervisor runs on core 0. It requires healthy selected services
and a successful application result before stopping renewal. Its intentional
exit suppresses the synthetic maintenance failure note. Real application,
service and processor failures retain RTC records and cannot qualify as the
expected stop. The runner reads no DRAM after a reset.

The EL1 watchdog renewal check requires BOOT input low even though resident
EL3 admission permits it high. The FEL proposal retains that live prerequisite;
autonomous card boot needs a source-backed resolution of the discrepancy.

The run prediction, worked 60-second session budget, reset observations and
stop conditions belong in [the evidence record](../evidence/resident-fel.jsonl).
The coordinator integrates the runner changes into the daemon's checkout before
host admission and submission. No worker opens a USB device.

## Approval and remaining work

Three design reviewers assess simplicity, hardware performance and correctness
before power-state implementation, then review the code. The coordinator must
approve static OFF before a run and approve the exact frozen board proposal.
The cache predecessor remains an unresolved correctness prerequisite.
No card write is part of this procedure. Card installation requires a separate
allocation, backup, exact write extent and storage-write approval.
