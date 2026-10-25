# SD and eMMC identification and reads

The native [storage driver](../drivers/storage/sunxi_mmc.c) implements bounded
card identification and reads through the A733 SD/MMC controller. SD cards use
SDC0 at `0x04020000`. The [platform binding](mmc-platform.md) selects SDC2 at
`0x04022000` or SDC3 at `0x04023000` for eMMC, with SDC2 as the default.
The caller selects the card type explicitly. Both current boards have microSD;
neither has populated eMMC or UFS. SDC1 remains owned by the
[SDIO driver](sdio.md).

This component is implemented and passes the source and controller/card model
checks below. Physical card identification and reads remain unqualified. The
[existing storage probe](storage.md) timed out on eMMC CMD1. That observation
establishes a failed command, without determining whether eMMC is populated.
This implementation has issued no physical commands.

## Supported profile

The controller transfers data through its FIFO with programmed I/O. Each poll
reads the status register once and moves at most one 512-byte block, 128
32-bit words, as one burst. A read takes the FIFO fill level from status bits
27:17, or one word when the level reads zero but the FIFO-empty flag is clear.
A write takes the free space, 256 words minus the fill level, and nothing while
FIFO-full is set. The platform admits each burst once. There is no allocation,
DMA mapping, interrupt dependency or filesystem requirement. Read buffers can
be unaligned. The caller supplies an absolute deadline in monotonic
microseconds.

The manual describes the level field as the number of filled FIFO locations.
That interpretation has not been confirmed on the board. A level that overstates
the filled words would underrun the FIFO; the raw FIFO underrun/overflow bit 11
is a terminal error, so the read retains its buffer and leases instead of
returning data.

The initial profile uses one data line, single-data-rate signaling and a
100–400 kHz card clock. The platform must admit an even 200–800 kHz module clock;
the controller divides that rate by two. Profile zero keeps this rate for identification and subsequent reads. The voltage request is a nonzero subset of the OCR bits for
3.2–3.4 V, consistent with independently admitted supplies.

After confirming the enabled card clock, identification waits at least 1 ms
before CMD0. After observing CMD0 completion, it waits at least 2 ms before CMD8
or the eMMC CMD1 query. These intervals follow the pinned bootloader's
[mmc_go_idle implementation](https://github.com/orangepi-xunlong/u-boot-orangepi/blob/b791be842935b27268ae3d00e943a9075495f30a/drivers/mmc/mmc.c).
Polling remains nonblocking and checks the original deadline and retained
ownership during both waits. Clock activation and command completion each get
a fresh counter sample; the delays do not rely on caller scheduling intervals.
Six focused sanitizer cases check SD/eMMC progression, expiry during either
wait, lost ownership and counter overflow. Evidence is under
`second-board-20261018-3-3/cmd0-pacing/`. Card/controller responses are modeled;
physical identification remains untested.

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

The read API uses CMD17 and CMD18 to expose media data. One request reads 1–8 whole 512-byte
blocks. Admission checks the entire range against capacity and the 32-bit
command argument before any controller access. SDSC/eMMC byte addressing also
checks the last requested block against byte-address overflow. A separate
[bounded write primitive](mmc-write.md) requires explicit policy authority.
The default native platform entry points are read-only; only the bound
[storage update service](storage-update.md) can admit writes. There is no
erase, partition-switch, generic-command or automatic fallback API.
CMD16 changes the volatile read block length; it does not write media contents.

## SD can negotiate four-bit default and high-speed operation

The read-only application and the storage maintenance session each select a
typed SD profile before identification.
Profile zero preserves the original one-bit 400 kHz path. Profile one requires
four-bit default-speed signaling at a configured rate no greater than the CSD
`TRAN_SPEED` limit or 25 MHz. Profile two requires positive high-speed support
and selection, using four-bit 50 MHz signaling at the existing 3.3 V.
Unsupported capabilities retain a clear refusal. No uncertain command permits
fallback, another CMD0 or a media write. Profile three, UHS-I SDR104, is
described [below](#uhs-i-sdr104-switches-the-pf-bank-to-18-v-and-tunes-the-sample-delay).

After normal identification and transfer-state confirmation, profiles one and
two issue CMD55 with the assigned RCA and read the eight-byte SCR through
ACMD51. SCR structure must be known, with both one-bit and four-bit support.
CMD55/ACMD6 argument two selects four data lines. Only successful card completion
permits the host width write and readback. The next data command therefore uses
the same width at the card and controller.

Profile two additionally requires SCR specification version at least 1 and the
CSD switch command class. CMD6 check mode reads 64 status bytes and requires
function-group-one high-speed support without its busy indication. CMD6 switch
mode then reads another 64 bytes, requiring selected function one, support and
no busy indication. A busy or unsupported check is a terminal refusal under
this service's no-retry policy. Successful commands change volatile card mode;
they do not modify stored sectors.

The driver stops card output and waits for completed clock-update command
before changing the selected module through its bounded platform callback.
Fresh time and owner checks surround that callback. The platform preserves
shared parents and changes only its exclusively owned SDC0 module. After timing
and clock readback, the driver enables card output, waits for another completed
update and requires final CMD13 ready/transfer state. Only then is the card
published as READY for the existing sector-read API.

Negotiated SDR uses a stable 400 MHz PERI0 branch. The module divider is linear,
with N equal to one. Card clock is module clock divided by two. Profile one's
M is the ceiling of `400000000 / (2 × min(TRAN_SPEED, 25000000))`, restricted
to eight through 32; an unrepresentable cap is refused. Profile two uses M four.
Reported integer rates are the floor of those ratios, derived from configured
registers. They are not frequency measurements. For example, M eight produces
25 MHz card clock, M ten produces 20 MHz, and M fourteen produces a reported
14,285,714 Hz without exceeding a 15 MHz CSD cap.

The phase settings follow the pinned Linux v5p3x profiles: command drive at
180 degrees, data drive at 90 degrees for default speed and 180 degrees for
high speed. Command/data sample phases use 90 degrees for default speed and
180 degrees for high speed. New timing mode is selected with HS400 disabled;
physical timing margins and tuning remain unqualified.

Internal SCR/CMD6 transfers program eight- and 64-byte block sizes respectively.
Subsequent CMD17/CMD18 sector reads restore 512-byte blocks. All transfers retain
exact byte counts, command/data completion, FIFO-empty checks, error handling
and final card status. No public raw-command escape is added.

The source basis is the pinned Linux `mmc-read/linux-sd.c` SCR and switch
handlers, U-Boot `drivers/mmc/mmc.c` capability/width/switch operations and Linux
`sunxi-mmc-v5p3x.c` clock/timing code. A733 manual sections 4.1.6.128,
11.3.5.6 and 11.3.8 define module divisors, timing controls and controller
transfers. The original SD specification file was inaccessible offline, so
implementation claims use these cached Linux, U-Boot and manual sources.

## Ownership and completion

The [public contract](../include/sunxi_mmc.h) requires a platform binding to
acquire and retain exclusive ownership of the card, controller and every
register writer. It requires cold native card authority or the explicit first
read-only [SD CMD0 initialization policy](storage-read-application.md). Both
require settled rails, native signaling authority, clock/gate/reset accessibility,
correct pins and reviewed recovery. The command queue must be disabled and
drained, with no outstanding commands or DMA.
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

A card may set R1 OUT_OF_RANGE (bit 31) after an open-ended CMD18 whose
last block is the last user-area sector. SD physical layer 4.10 section 4.3.3
and JESD84-B51 section 6.8.3 tell the host to ignore it there, and the pinned
Linux block driver does so. For a CMD18 ending at the last sector, the driver
ignores bit 31 in the automatic CMD12 response and in the following CMD13. Every
other card error bit stays terminal. OUT_OF_RANGE in the CMD18 response itself,
in a CMD17 response, or after a read that ends earlier still faults.

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
| [drivers/mmc/core/block.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/block.c) | OUT_OF_RANGE ignored on open-ended multiple-block stop status (`mmc_blk_stop_err_bits`) |
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
both SD and eMMC. The current model also reports the FIFO fill level. An
8-block read now takes 16 core polls, 8 of them moving data; the previous
16-word step took 72 polls, 64 of them moving data. The model also covers a
level limited to five words, a level field that stays zero, and
OUT_OF_RANGE at the card end for SD and eMMC, with refusal cases elsewhere. Only that changed behavior and the affected native object
were rebuilt after the original batch.

Preserved evidence is under
`~/.cache/sunstoneos/evidence/mmc-read-20261016/`. The original source/model
batch is in `batch1/`; the final automatic-stop check is in
`auto-stop-argument/`. Their snapshots retain executed binaries, source inputs,
reference code and commands. The capacity/identification evidence remains
scoped to those unchanged paths. The [bounded-write validation](mmc-write.md)
covers the affected shared command path and current native object compilation.
Neither compilation nor a model establishes physical storage operation.

## Files are read through a FAT or exFAT volume

The [read-only filesystem port](filesystem.md) mounts a FAT or exFAT volume on
the card and loads whole files through this core. It passes a private 4 KiB
buffer to `sunxi_mmc_read`, polls until DONE or FAULT and copies data out only
after success. A fault keeps that buffer borrowed and stops the port. The core
itself is unchanged and has no filesystem knowledge.

## UHS-I SDR104 switches the PF bank to 1.8 V and tunes the sample delay

Profile three requests 1.8 V signalling and runs the card in SDR104 at
150 MHz. [The UHS-I design](designs/sd-uhs.md) derives the sequence from the
SD physical layer specification, the pinned Linux MMC core and the board
kernel's `sunxi-mmc`, pin-control and clock drivers, with file and line
citations. It is implemented and host-checked; no board has run it.

The board has no 1.8 V card regulator. SDC0's pins are the PF bank, which the
SoC powers from either `VCC-IO` (DC1SW2, 3.3 V) or `VCC18-PF` (CLDO5, 1.8 V).
Bit 0 of `GPIO_POW_VAL_SET_CTL` (`0x02000070`) selects the supply, and
`GPIO_POW_VAL` bits 11:10 report it. The card's VDD stays at 3.3 V.

After CMD8, the core asks the platform whether the switch is available. The
platform offers it only in adaptive PF mode with sensed and selected 3.3 V and
CLDO5 programmed to 1.8 V, and reports whether the PERI0_300M parent is
stable. With the switch available, every ACMD41 carries S18R. A ready
response with S18A leads to CMD11 before CMD2:

1. CMD11 with the controller's voltage-switch flag; its R1 must be clean.
2. At least 1 ms later, DAT3 must read low.
3. The card clock stops through a voltage-switch update with DAT0 masked.
4. The platform writes the selector once, then polls until PF reports 1.8 V,
   within 10 ms.
5. After 10 ms the clock restarts the same way. Within a further 1–11 ms,
   DAT3 must read high and the switch-done bit, `RINTSTS` bit 10, must be set.

After ACMD6, CMD6 checks and selects SDR104. The module clock becomes
PERI0_300M / 1 (150 MHz card clock), or PERI0_400M / 2 (100 MHz) without that
parent. The controller moves to the old sample mode with 180-degree drive,
and each SDR104 read sets the read threshold to its block size. It then sweeps the 64 sample delays with CMD19: a
delay passes after 20 matching tuning blocks, a failed block resets the FIFO
and state machines, and the core selects the midpoint of the first longest
passing run of at least 12 delays.

A failure before CMD11 leaves high speed at 3.3 V. Without SDR104 or a
usable window, the card runs SDR25 at 50 MHz and 1.8 V. Failures between
CMD11 and the restarted clock are terminal and retained, because the
specification then requires a card power cycle; whole-board power removal is
the only card power cycle here. The card keeps 1.8 V signalling until it loses
power, so the boot ROM and every later SD lifetime need that power removal
first. The card record reports the outcome, the capability bits, the passing
window and the chosen delay.

`checks/sunxi-mmc-uhs.py` pins the references, asserts the cited facts and
runs the core and platform models under AddressSanitizer and
UndefinedBehaviorSanitizer.

## Remaining storage coverage

The native platform binding implements clock, rail, pin, reset and presence
ownership for SDC0, SDC2 and SDC3. Its physical ownership, signaling, supply
and reviewed recovery prerequisites remain required before a bounded
identification/read attempt. Only the read-only SD service permits its explicit
first CMD0 policy in place of cold card history. SDC0 card voltage and PF6 configuration must be observed. SDC2 population and the existing
CMD1 timeout require new evidence before another physical attempt.

SD v1 identification, SDUC, eight-bit buses, SDR50 and DDR50, DMA,
interrupts, multiple-block write/erase modes, eMMC boot/alternate partitions,
4 KiB sectors, command queuing, hotplug/recovery and power management remain
outside the implemented profile. Four-bit default-speed, high-speed and UHS-I
SDR104 SD reads are implemented but physically unqualified, as is the
update-bound writer at every profile. UFS remains tracked separately in
[storage.md](storage.md). Completing this component does not complete board
storage support or the full OS goal. Performance qualification follows
functional hardware coverage under the [validation policy](validation.md).
