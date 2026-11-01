# PCIe and NVMe

**Status: implemented.**

Shared-resource root-port preparation, COMB1 PHY,
direct-endpoint configuration, BAR/iATU setup, endpoint DMA COPY and bounded
NVMe reads have host/model evidence. Applications select Gen1.

**Board-qualified scope:** None.

No endpoint or NVMe device is available.
Link, endpoint power, DMA visibility/confinement, interrupts, power transitions
and recovery remain unqualified.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `pcie-pd-rail-native-20261018/` binds corrected BLDO1 PD-bank and DCDC2
  core ownership with separate fitted-board analog-supply admission.
- `pcie-endpoint-20261019/root-integration.json` models one 4096-byte COPY
  with CPU0 INTID185 MSI and copied output.
- `sol-nvme-msix-1028/` covers optional NVMe MSI-X vector-zero wakeup;
  validated completion-queue entries remain the completion authority.

The function client scans functions 0–7, refuses bridges, assigns below-4-GiB
BARs, supports polling/INTx/MSI-X and D0/D3hot. NVMe reads one admitted 4096-byte
block and stops the controller/port with PERST held. Faults retain queue/data
pages and port leases. Queue memory above 4 GiB is refused.

Endpoint COPY needs an identified endpoint's source-backed DMA completion and
data-before-status ordering. BME clear, BAR access and MSI alone do not prove
drain; identity mappings do not isolate hostile endpoints. No endpoint supplies
that authority. Configurable 5/8 GT/s targets have host checks; applications
still use 2.5 GT/s. Bridges/switches, ASPM, root DMA, application Gen2 selection
and physical Gen2/Gen3 remain open. NVMe writes require user-designated scratch
media and coordinator approval.

See [PCIe](../pcie.md), [endpoint test](../pcie-test-application.md),
[application](../pcie-application.md) and [NVMe](../pcie-nvme.md).
