# PCIe and NVMe

**Status: implemented.**

Shared-resource root-port preparation, COMB1 PHY,
direct-endpoint configuration, BAR/iATU setup, endpoint DMA COPY and bounded
NVMe reads have host/model evidence. All three applications request the vendor
Gen3 default and accept negotiated x1 Gen1 or Gen2 fallback. Normalized REF
admission accepts the boards' 26 MHz DCXO with the unchanged vendor
internal-100-MHz PHY sequence.

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

Evidence under `~/.cache/sunstoneos/build/pcie-1031/` includes vendor/native
traces, sanitizer, linked-image and QEMU checks. These cover negotiated
Gen1–Gen3 and normalized reference admission. QEMU supplies CPU and RAM;
PCIe effects and DMA visibility are modeled. The frozen image index is
`images/README.md` under that directory. Images remain host-only until complete
PCIe/USB retirement, interrupt restoration and watchdog return meet the shared
probe contract. No board run is queued.

The function client scans functions 0–7, refuses bridges, assigns below-4-GiB
BARs, supports polling/INTx/MSI-X and D0/D3hot. NVMe reads one admitted 4096-byte
block and stops the controller/port with PERST held. Faults retain queue/data
pages and port leases. Queue memory above 4 GiB is refused.

Endpoint COPY needs an identified endpoint's source-backed DMA completion and
data-before-status ordering. BME clear, BAR access and MSI alone do not prove
drain; identity mappings do not isolate hostile endpoints. No endpoint supplies
that authority. Bridges/switches, ASPM, root DMA and physical Gen3/fallback
qualification remain open. NVMe writes require user-designated scratch
media and coordinator approval.

See [PCIe](../pcie.md), [endpoint test](../pcie-test-application.md),
[application](../pcie-application.md) and [NVMe](../pcie-nvme.md).
