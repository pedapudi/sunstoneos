# Capture thermal readings after native recognition returns

Status: **implemented; host-tested**. The capture has no board qualification.

The returning CPU image retains its first recognition's thermal report in RAM. USB operation 7 exposes the thermal report in ordinary OS images, but the returning profile completes without starting USB. The host therefore reads the existing thermal window and reading storage after successful ROM return, before restoring uploaded RAM, so that physical recognition has captured sensor readings across its compute interval.

## How it works

`apps/os/cpu-return-thermal.py` locates both objects in the thermal producer's compilation unit from the exact image's DWARF, which supplies member offsets. The ELF symbol table independently confirms their addresses and sizes. Ambiguous definitions, missing debug information and incompatible geometry fail before launch. No native code, recognition arithmetic or return ABI is involved.

The window contains an immutable header, its first row index and a publication flag. The header fixes the row count when recognition ends. Later sensor samples may append rows; capture uses the published count rather than the later storage count. The decoder (`apps/os/thermal-report.py`, shared with USB capture) checks session identity, bounds and row ordering. Its completeness classification is separate from successful ROM return and RAM restoration, and incomplete thermal evidence does not establish qualification.

The host reads 33,816 additional bytes in two bounded ROM reads and adds no native allocation, sensor access, copy, synchronization or recognition latency.

## Acceptance for a longer-input case

One existing recording with eight workers and a matching host reference. Acceptance requires parity, complete thermal evidence, all secondary CPUs OFF, normal return and verified boot-rail restoration. A cool run establishes sampled operation across that finite load interval; it does not establish a thermal-trip transition or equilibrium under sustained load.

## Evidence and limitations

`checks/os-cpu-return-thermal.py` executes the linked native emitter and window publication in each frozen CPU and GPU ELF, with modeled time, board identity and policy values. It establishes the native writer and host reader contract. The physical longer-input attempt did not return; there is no longer-input transcription or thermal qualification. Build recipe, hash and result are in [the records](../evidence/cpu-return-thermal.jsonl) (`cpu-return-thermal-longer-input-attempt`).

## Open work

Rerun the longer-input case with a concrete correction for the nonreturn, after verifying build identities and current entry authority.
