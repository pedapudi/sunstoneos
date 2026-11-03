# Design: DMA address-width rules and the path above 4 GiB

Status: **host-tested** for DMA0, the system IOMMU, the native camera, NVMe and xHCI. No transfer above 4 GiB has run on hardware.

[docs/dma-address-width.md](../dma-address-width.md) is the single record of the per-master rules, the named limits, the submit checks and the changes other drivers must make. Each owner checks its buffer against its master's limit when it admits the buffer, before any device access and before any table or descriptor byte is written; no routing table or bounce buffer exists.

The DMA0 conflict scan walks the channel array by pointer. A descriptor chain that crosses the end-of-chain marker `0xfffff800` is refused in `a733_dma_build`, after mapping validation, the conflict scan and the quiet-channel status reads and before any descriptor byte, cache operation or register write. On a channel that is not quiet the result is `A733_DMA_RECOVERY`; on a quiet channel it is `A733_DMA_INVALID`.

Qualified figures (host model and instruction emulation; record `dma-address-width-host-emulation` in [the evidence file](../evidence/dma-address-width.jsonl)):

| Figure | Value |
| --- | --- |
| AArch64 instructions per successful submit (copy, four-segment copy, FIFO) | 1,121; 2,795; 990 |
| AArch64 instructions per refused bad-chain submit (quiet, busy channel) | 710; 667 |
| Width checks added to GPU, NPU, IOMMU or crypto job paths | none |
| IOMMU leaf words per 307,200-byte plane | 75 |

Open work: the NVMe refusal and the camera's fixed IOVAs are consumer changes recorded in [dma-high-memory.md](dma-high-memory.md); hardware evidence for the rules is listed there.
