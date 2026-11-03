# Preserve uncertain supply observations before electrical admission

Status: **implemented and host-tested** (decoder and observing client). The
finite diagnostic image is **open** on the board: it awaits a qualified launch
wrapper. Physical voltage and fitted radio wiring remain unresolved.

The runtime PMIC decoder follows the pinned AXP8191 bootloader table. Its
DCDC8 and DCDC9 selectors disagree with the pinned Linux table from code 88
onward. Code 100 represents 1.8 V in U-Boot and 3.1 V in Linux. Both sources
identify the same selector register and seven-bit mask. Chip ID 3 and revision
0 do not establish which voltage table matches the fitted part. The
comparison is in [pmic-reference-reconciliation.md](pmic-reference-reconciliation.md).

## Decoding

For DCDC8 and DCDC9 the decoder retains the complete selector and enable bytes
and publishes `decoded=0` and `programmed_uv=0` for codes 88–127. Codes 0–87
keep their voltage interpretation, and other rails keep their profiles. The
decoder changes no rail setting, voltage point, clock, lease or I2C sequence.
Consumers that require a decoded voltage must refuse these uncertain codes.

The decoder runs once per completed snapshot with a budget of zero MMIO
accesses, locks, barriers, allocations, sleeps and extra copies.

## Observation through the existing provider

The finite diagnostic (`apps/power-supply-observe`) uses the production
runtime provider to observe DCDC1, DCDC8, DCDC9, BLDO5, CLDO1 and CLDO3
sequentially under one stable rail lease. Each snapshot is the six-read
sequence of page, chip, revision, enable, selector and page. The provider
performs no PMIC data writes. BLDO5's selector at register `0x2e` fills the
gap in the saved second-board capture.

The diagnostic has a 1.5-second client deadline, within the returning
monitor's two-second budget, that starts after task entry and counter
initialization; whole-image startup and reporting time are unmeasured. Each
snapshot makes six byte reads and one copied result, with at most one
outstanding request. Request reap precedes release of the client rail lease.
The provider retains its platform clock, pin and controller ownership. A
failed transfer retains the provider's resources and reports its stage, phase
and last status without reset or retry.

Observations establish programmed bytes and reference agreement only. Supply
admission additionally needs an authoritative transfer function for the fitted
PMIC and evidence of the fitted rail route, and readbacks establish neither.

## Validation

Host validation covers complete raw results, reserved and disputed selectors,
disabled outputs, request reap and retained faults, and the source comparison
of the decoder against both references. The OS-only, hello-compute and
external mimus baseline builds cover shared decoder linkage. Records:
[pmic-supply-observation.jsonl](../evidence/pmic-supply-observation.jsonl).

## Open work

1. Freeze the diagnostic image and run it on the board once the coordinator
   accepts the watchdog and breadcrumb launch path.
2. Any physical proposal needs the coordinator's approval after the board
   runner is qualified. Voltage or power-domain changes need their own design.
3. Thermal load transitions and E902 suspend/resume depend on processor
   lifecycle evidence and shared resource ownership; this diagnostic supplies
   no such proof.
