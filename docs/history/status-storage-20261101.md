# Storage and autonomous card boot: historical status record 2026-11-01

This log holds the prose evidence of [the current status](../status/storage.md) as it stood on the date above. It is not updated. Board results that have queue entries are in the records `docs/evidence/storage.jsonl`.

**Status: implemented.**

Native SD identification, bounded reads, backup-first
updates, slot loading and cold entry have software evidence. Neither current
board has populated eMMC or UFS. Keep the failed UFS probe disabled.

**Board-qualified scope:** Returning microSD PIO reads at 50 MHz, four-bit,
3.3 V complete. One 4 KiB DMA read and one sentinel-prefilled 64 KiB read match
same-card PIO references and restore controller, clocks, pins and saved RAM
before normal ROM return. The sixteen-descriptor 64 KiB transfer takes
3,332 microseconds; it does not measure sustained throughput.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `sol-resume-20261028/port-3-3/` binds the 4 KiB DMA comparison and return.
- `sol-sd-warm-packet-1029/sentinel-physical-result.json` binds all sixteen
  4 KiB chunks, shutdown and return for the 64 KiB comparison.
- `resume-sd-completion-1025/snapshot/` covers CMD11 and clock-restart software
  timing, including both completion bits and the post-delay deadline.

The read core supports negotiated legacy and high-speed modes and descriptor
reads up to 256 sectors. The repeated two-client service and high-speed backup
path use 64-sector DMA reads; legacy profiles use PIO. Writes use sector PIO
and card-status confirmation, followed by authenticated readback. Uncertain
completion retains memory and device ownership. No physical write is qualified.

SDR104 at 150 MHz, CMD11, a PF-bank 1.8 V switch and a 64-delay CMD19 sweep
are implemented and host-modeled. This lifetime requires whole-board power
removal after success or post-CMD11 failure, so it is blocked without physical
recovery. SDR50 and DDR50 remain unimplemented. Sustained throughput, broader
extents/cards, card replacement, removal/recovery and voltage timing remain open.

The [read-only FatFs port](../filesystem.md) supports FAT16/FAT32/exFAT in host
models, retains a possibly active failed-read buffer and makes no later call.
No native image links it. GPT, multiple volumes/clients and filesystem writes
remain unsupported. Optional eMMC software is host-tested; absent media must
not be probed. Applicable SPI storage and boot conflicts still need qualification.

Stage 2 verifies slot headers and SHA-256 extents, selects the newest valid
slot and rejects stale board/CPU grants before watchdog arming. It stamps fresh
epochs into the OS session. Static secondary OFF is held through cold loading.
The monitor uses 32,368 bytes with 400 bytes before its fixed report. Host
backup/write/readback/restore and session checks pass under
`~/.cache/sunstoneos/build/sd-boot-1031/`; cold-card evidence is under
`resume-card-os-1026/` and `resume-stage2-sd-1026/`.

Physical installation, autonomous OS boot, cold controller/supply state,
reset retirement and power-loss recovery remain unqualified. Packer licensing
and exact source equivalence remain unresolved. Any physical card write needs
its full host backup and the approved target-bound path in [goal section 4](../goal.md).
No NVMe write is permitted without user-designated scratch storage.

See [storage evidence](../storage.md), [MMC](../mmc.md),
[SD DMA](../designs/sd-dma.md), [updates](../storage-update.md),
[cold entry](../cold-entry.md) and [slot layout](../boot-slots.md).
