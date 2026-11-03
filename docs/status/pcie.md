# PCIe and NVMe

**Status: implemented.**

Shared-resource root-port preparation, COMB1 PHY, direct-endpoint configuration, BAR and iATU setup, endpoint DMA COPY and bounded NVMe reads have host and model evidence. All three applications request the vendor Gen3 default and accept negotiated x1 Gen1 or Gen2 fallback. Normalized REF admission accepts the boards' 26 MHz DCXO with the unchanged vendor internal-100-MHz PHY sequence.

**Software evidence:** host, emulator, source.

**Board-qualified scope:** None.

No endpoint or NVMe device is available.

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| Root-port preparation, COMB1 PHY, BAR and iATU setup | host-tested | none | [pcie.md](../pcie.md) |
| Endpoint DMA COPY and bounded NVMe reads | host-tested | none | [pcie-nvme.md](../pcie-nvme.md) |
| Physical link, endpoint power, interrupts, recovery | open | none; no endpoint is available | open work 1 |

## Open work

1. Qualify the link, endpoint power, DMA visibility and confinement, interrupts, power transitions and recovery once an endpoint is available.
2. Keep the images host-only until PCIe and USB retirement, interrupt restoration and watchdog return meet the shared probe contract.
3. Cover bridges and switches, ASPM, root DMA and physical Gen3 and fallback qualification.

## Evidence

Component documents: [PCIe](../pcie.md), [NVMe](../pcie-nvme.md) and [the PCIe application](../pcie-application.md).

Board results are records in `docs/evidence/pcie.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-pcie-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/pcie.jsonl`.
<!-- evidence-table:end -->
