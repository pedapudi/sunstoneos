# Returning DCDC4 observation

Build `npu-supply.c` with the private `BB_SNAPSHOT_DCDC4` definition to observe
DCDC4 through the same returning provider used by the
[DCDC2 probe](NPU-SUPPLY.md). The definition changes only the selected rail,
task name and log labels. The default build remains DCDC2.

The DCDC4 variant acquires a shared rail token, waits for its one snapshot,
reaps the request and parks the provider before reporting success. It keeps
the 5,160-microsecond settle wait, 1.5-second session deadline and 2-second
returning monitor bound. The rail token is a software ledger entry; it does
not access GPU hardware. There is no domain row, PPU access, voltage write,
clock write or automatic DCDC2 transaction.

Reuse the DCDC2 probe's physical gate, route, pin, interrupt and exclusive
ownership prerequisites only through coordinator-confirmed continuity in the
same powered lifetime. Bind the current returning DDR/cache/survey records
and recovery policy separately. Earlier observations on another board do not
establish these facts.

After a validated normal return, require the line
`COMPLETE DCDC4 snapshot: reaped and provider parked` and preserve the raw
snapshot. The GPU's 800 mV configuration requires page-before and page-after
zero, chip 3, revision zero, enable register `0x10` bit3 set, and selector
register `0x15` masked by `0x7f` equal to `0x1e`. The decoded fields must report
enabled, decoded and 800,000 µV. Full enable and selector bytes may differ
from another board's bytes.

Successfully captured unexpected values return normally for host classification.
Provider, transfer, deadline and quiescence failures retain the existing failure
handling. A supply snapshot qualifies neither GPU power transitions nor identity
reads. It establishes programmed configuration; analogue voltage and settling
remain unmeasured.
