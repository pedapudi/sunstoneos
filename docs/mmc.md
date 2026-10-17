# Read-only SD and eMMC

The native [storage driver](../drivers/storage/sunxi_mmc.c) implements bounded
card identification and reads through the A733 SD/MMC controller. SD cards use
SDC0 at `0x04020000`. The [platform binding](mmc-platform.md) selects SDC2 at
`0x04022000` or SDC3 at `0x04023000` for eMMC, with SDC2 as the default.
The caller selects the card type explicitly. SDC1 remains owned by the
[SDIO driver](sdio.md).

This component is implemented and passes the source and controller/card model
checks below. Physical card identification and reads remain unqualified. The
[existing storage probe](storage.md) timed out on eMMC CMD1. That observation
establishes a failed command, without determining whether eMMC is populated.
This implementation has issued no physical commands.

## Supported profile

The controller transfers data through its FIFO with programmed I/O. Each poll
consumes at most sixteen 32-bit FIFO words. There is no allocation, DMA mapping,
interrupt dependency or filesystem requirement. Read buffers can be unaligned.
The caller supplies an absolute deadline in monotonic microseconds.

The initial profile uses one data line, single-data-rate signaling and a
100–400 kHz card clock. The platform must admit an even 200–800 kHz module clock;
the controller divides that rate by two. Identification and subsequent reads
retain this rate. The voltage request is a nonzero subset of the OCR bits for
3.2–3.4 V, consistent with independently admitted supplies.

| Card profile | Identification and capacity |
| --- | --- |
| SD v2 SDSC | CMD8 echo `0x1aa`, CMD55/ACMD41, CID, assigned RCA, CSD, selection, status and CMD16 for 512-byte reads; byte addressing, at most 2 GiB |
| SDHC/SDXC | Same SD v2 identification; CSD structure 1 and OCR sector-addressing flag must agree; up to 2^32 sectors, retained as a 64-bit count |
| eMMC v4.x–5.1 | Explicit CMD1 capability query and bounded readiness polling, CID, assigned RCA 1, CSD, selection, status and a 512-byte EXT_CSD read |

CID is the card identification register. RCA is its relative card address. CSD
and EXT_CSD describe capacity and operating modes. OCR is the operating
conditions response. The driver retains CID and CSD as four words, with card
bit 127 in word 0 bit 31. Controller response registers are read in reverse
register order for these long responses.

The eMMC profile accepts CSD specification version 4, known EXT_CSD revisions
0–8 and structures 0–2. Sector addressing requires revision 2 or later and a
nonzero little-endian `SEC_COUNT`. Byte addressing uses the CSD formula and
refuses a conflicting capacity above 2 GiB in EXT_CSD. The active partition
must be the user area, the active data sector must be 512 bytes, and bus width,
high-speed timing and command-queue mode must retain their legacy values.

Only CMD17 and CMD18 expose media data. One request reads 1–8 whole 512-byte
blocks. Admission checks the entire range against capacity and the 32-bit
command argument before any controller access. SDSC/eMMC byte addressing also
checks the last requested block against byte-address overflow. The driver has
no write, erase, partition-switch, generic-command or automatic fallback API.
CMD16 changes the volatile read block length; it does not write media contents.

## Ownership and completion

The [public contract](../include/sunxi_mmc.h) requires a platform binding to
acquire and retain exclusive ownership of the card, controller and every
register writer. It must establish cold native card state, settled rails,
clock/gate/reset accessibility, correct pins and independent recovery. The
command queue must be disabled and drained, with no outstanding commands or DMA.
These are physical prerequisites. Model callbacks establish none of them.

For SD, physical presence must come from the board's active-low PF6 card-detect
pin. The controller's DAT3 status is insufficient. The platform must retain
card-detect pin configuration and card/pin/rail ownership. For eMMC, population
and accessibility require independent evidence. A failed command never causes
an automatic protocol change or a declaration that a card is absent.

Cold preparation checks the controller revision, command-idle state, data/card
busy state and internal DMA enable. It resets the local controller, selects
CPU FIFO access and disables interrupt and DMA enables. It configures one-bit
width, legacy timing, sample-FIFO use, disabled HS400, normal CRC timing,
software sample delay and drive phases. Clock-update commands bracket output
clock enable. The platform remains responsible for unmodified control fields
and reset-state admission, including disabled command queuing and encryption.

An SD readiness retry consists of CMD55 followed by ACMD41. An eMMC readiness
retry uses CMD1 after its capability query. Busy OCR responses permit another
readiness attempt after at least one millisecond, within the original deadline.
Response timeout or another command error immediately retains a terminal fault.
SD v1 cards that do not implement CMD8 are outside this initial profile.

A successful read requires all of the following:

1. Exactly the requested FIFO bytes, a successful command response and command
   completion.
2. Data-transfer completion. Multiple-block reads also require automatic CMD12
   completion and its successful response from `RESP1`.
3. A cleared command-load bit, idle controller/card busy state, an empty FIFO
   and no late controller error.
4. A subsequent CMD13 response reporting both ready-for-data and transfer state.

The automatic stop argument is explicitly zeroed before CMD18. The manual gives
its register a reset value of `0xffff`; inherited contents cannot select the
argument. CMD12 is sent by the controller after the finite transfer. The driver
does not issue a second stop command after a failed transfer.

The result becomes available only after the final monotonic deadline check.
Callbacks cannot reenter admission, polling or result collection. A successful
`take` returns the complete buffer to its owner and permits another read.
Controller/card/platform ownership remains retained while the core is ready.

A failed callback may have taken effect. Timeout, CRC, removal indication,
invalid card state, failed stop or lost admission freezes the core immediately.
The borrowed buffer and all platform leases remain retained. Received-byte
counts are diagnostic; partial or complete-looking data from a fault is
untrusted. Result collection on a fault copies diagnostics without releasing
anything. Recovery, hotplug replacement and disposal require an external
owner with independent evidence; this API does not implement them.

## Sources and evidence

The A733 user manual, revision 1.00, defines the SDC0–2 registers in section
11.3.8. Section 11.3.6.5 requires both automatic-command completion and
data-transfer completion for CMD18. Section 11.3.8.7 assigns the automatic
CMD12 response to `RESP1`; section 11.3.8.22 defines its argument register.
Sections 11.3.8.15–16 define raw errors and busy/FIFO status.

The pinned manual PDF has SHA-256
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
Its [published source](https://gitlab.com/tina5.0_aiot/product/docs/-/blob/d0dc4d615be00b76e3a0818d0818eff8f69ffa15/A733/Hardware%E7%A1%AC%E4%BB%B6%E7%B1%BB%E6%96%87%E6%A1%A3/%E8%8A%AF%E7%89%87%E6%89%8B%E5%86%8C/A733_User%20Manual_V1.00.pdf)
is pinned at that revision. The source cache holds the PDF and its text
extraction under `camera-research/`.

The reference kernel is `orangepi-xunlong/linux-orangepi` at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`:

| Source | Used behavior |
| --- | --- |
| [drivers/mmc/core/sd.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/sd.c) | CSD structure, capacity and addressing |
| [drivers/mmc/core/mmc.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/mmc.c) | eMMC CSD and EXT_CSD capacity/revision handling |
| `include/linux/mmc/mmc.h` | Card status bits and EXT_CSD byte offsets |
| `bsp/drivers/mmc/sunxi-mmc.c` | Explicit zero automatic-stop argument in `sunxi_mmc_set_a12a` |
| `bsp/drivers/mmc/sunxi-mmc-v5p3x.c` | Controller version, legacy sampling phases, HS400 and CRC controls |

The pinned bootloader revision
`b791be842935b27268ae3d00e943a9075495f30a` supplies programmed-I/O and response
ordering in `drivers/mmc/sunxi_mmc.c`. Its
`drivers/mmc/host/sunxi_mmc_host_tm4.c` supplies drive/sample-delay programming.
The driver uses the source's legacy drive-delay selection of one for both
command and data. Physical timing qualification is still required for each slot.

Run the focused check with sources and evidence outside the repository:

```bash
python3 checks/sunxi-mmc.py \
  --sources "$HOME/.cache/sunstoneos/sources" \
  --evidence /absolute/new/mmc-evidence-directory
```

The check pins each source by hash and executes unchanged Linux CSD decoder
bodies in an external reference harness. Original source license notices remain
in that generated artifact. Linux code is absent from the native runtime.
The native capacity decoder uses 64-bit arithmetic for the maximum SDXC
capacity, which overflows the pinned reference's 32-bit capacity field.

The source/model batch passed 120 CSD capacity comparisons and 40 EXT_CSD
sector-count fixtures. It also passed SD/eMMC identification, single/multiple-block
reads, address/range refusal, incomplete completion flags,
failed automatic stop, card-state errors and retained partial reads. It also
passed CRC, response-timeout, removal, admission, uncertain-write, monotonic-time
and callback-reentry cases under AddressSanitizer and UndefinedBehaviorSanitizer.
A subsequent focused check passed explicit zero automatic-stop arguments for
both SD and eMMC. Only that changed behavior and the affected native object
were rebuilt after the original batch.

Preserved evidence is under
`~/.cache/sunstoneos/evidence/mmc-read-20261016/`. The original source/model
batch is in `batch1/`; the final automatic-stop check is in
`auto-stop-argument/`. Their snapshots retain executed binaries, source inputs,
reference code and commands. The final AArch64 object contains 6,460 bytes of
text/read-only data and zero static writable storage. Its largest direct stack
frame is 848 bytes during initialization; that is not a bound on callback stack
usage. This compile establishes native buildability, without physical execution.

## Remaining storage coverage

The native platform binding implements clock, rail, pin, reset and presence
ownership for SDC0, SDC2 and SDC3. Its cold physical admission and independent
recovery prerequisites remain required before a bounded identification/read
attempt. SDC0 card voltage and PF6 configuration must be observed. SDC2 population and the existing
CMD1 timeout require new evidence before another physical attempt.

SD v1 identification, SDUC, wider buses, higher rates, voltage switching,
tuning, DMA, interrupts, write/erase operations, eMMC boot/alternate partitions,
4 KiB sectors, command queuing, hotplug/recovery and power management remain
outside the implemented profile. UFS remains tracked separately in
[storage.md](storage.md). Completing this component does not complete board
storage support or the full OS goal. Performance qualification follows
functional hardware coverage under the [validation policy](validation.md).
