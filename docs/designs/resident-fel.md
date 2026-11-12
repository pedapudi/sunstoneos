# Resident OS entry from the USB boot ROM

Status: the resident entry, bounded completion test and runner are host-tested.
No resident board run is qualified. FEL is the boot ROM's USB loader; the
[autonomous card design](autonomous-card-os.md) defines cold card boot.

## Resident entry accepts the established FEL state

Under FEL, secondary cores are OFF, have never started and retain policy
`0x100`. Returning board runs qualify startup from this state through the
existing hotplug sequence. Resident FEL entry uses that same sequence. Its
upload and cache-publication interval has the same predecessor conditions as
returning entry and requires no additional static-OFF hold.

The resident startup checks each core before its first register write:

| Entry | Policy/status | Additional requirement | Startup |
| --- | --- | --- | --- |
| FEL | `0x100/0x100` | Clear hotplug requests | Existing returning hotplug sequence; no policy write |
| Cold card | `0/0` | Clear hotplug requests and GIC wake-disable | Publish entry, release static OFF, then hotplug |

An ON core, unknown policy or mismatched acknowledgment refuses before startup
writes. The explicit cold controller profile retains its strict static-OFF check.
`platform/a733/cpu_start.c` selects the release sequence from the verified entry
state. `apps/os/boot.patch` selects resident admission for the resident lifetime.

Stage 2 establishes static OFF before card loading, as described in the
[cold CPU contract](cold-cpu-static-off.md). Its policy-zero check and cache
cleanup remain part of cold boot. Resident OS entry selects its startup branch
from the observed policy; it does not infer whether the loader used FEL or the
card. FEL uploads use the qualified returning
entry assumptions and do not invoke the stage-2 ownership procedure.

## Reference sequence and implementation boundary

The [CPU startup reference](../cpu-start.md) and
[pinned sources](../../boot/a733/stage2/cpu-sources.json) identify the A733
vendor BL31 power map and hotplug sequence. CPU_ON publishes the reset entry
and architecture before requesting ON, then clears its software requests.
SunstoneOS binds the mailbox before this sequence, bounds acknowledgment and
retains resources on failure. Returning startup retains its register sequence.

Resident FEL admission adds exact policy/status checks for the inherited OFF
state. Resident cold entry retains `HELD_POLICY`, acknowledged static OFF and
GIC wake exclusion, then restores dynamic policy only after entry publication.
`checks/a733-cpu-start.py` covers both resident entry states, ON refusal,
retained failures and the returning writes against the pinned vendor helpers.

## The diagnostic retains its completion through reset

WDT0 uses mode `0x41`, configuration 1 and output 2, with zero FEL and redirect
fields. The launcher timestamps immediately before one CTRL restart and checks
readbacks and the one-millisecond publication bound. Native startup has a
one-second deadline; the initial contract expiry is four seconds after the
restart timestamp. The resident contract has no diagnostic deadline.

The diagnostic build uses hello-compute, healthy selected services and a
successful application result to stop renewal. Its supervisor runs on core 0
and suppresses the synthetic maintenance failure note for that intentional
exit. Real application, service and processor failures retain RTC records.
Ordinary resident builds continue supervision after application completion.
An immutable ELF marker identifies the completion diagnostic
(`os_resident_test_profile`).

The runner accepts only the marked diagnostic's exact retained stop. It reads
no DRAM after reset. The [prediction record](../evidence/resident-fel.jsonl),
`resident-fel-completion-proposal`, contains the time budget and expected result.
The frozen input manifest and exact host commands are in that record's evidence
directory. Submission waits for the coordinator to land the runner changes,
restart the daemon and instruct the worker to submit.

The current EL1 watchdog renewal check requires BOOT low. Its disagreement with
cold resident admission remains in the card design's open work.
