# Native SDIO controller and card identification

SunstoneOS implements a bounded controller core for the A733 SDC1 register
interface, SDIO card enumeration, and CMD52/CMD53 request construction.
A host register model verifies the implementation. Physical SDC1 access,
radio firmware startup, Wi-Fi traffic, and Bluetooth remain unqualified.
The [radio report](radio.md) lists the firmware and board dependencies.

The existing [storage probe](storage.md) only performs diagnostic eMMC
identification through SDC2. It does not supply a reusable transfer engine.
The native SDIO implementation uses separate files and leaves that probe intact.

## SDC1 uses the pinned controller variant with explicit initial timing

The [SoC device tree][soc] identifies SDC1 at `0x04021000` with compatible
string `allwinner,sunxi-mmc-v5p3x` and GIC SPI 162. The [controller match
code][controller] selects the corresponding implementation with physical index
1. The attached board's SDC1 version register has not been read by SunstoneOS.
SDC2's earlier diagnostic version does not establish SDC1's revision.

[drivers/storage/sunxi_sdio.c](../drivers/storage/sunxi_sdio.c) accepts a
reported controller version in the 5.x family starting at 5.3.0. An earlier
or later family produces a terminal variant error before reset. This range
selects the implemented register behavior; it is not physical qualification
of every revision in that range.

The platform must admit and retain a stable module clock greater than zero
and no faster than 800 kHz. Preparation clears the newer timing-selection bit,
selects the SDR divide-by-two path and divider zero, and sets one-bit width.
The output clock is therefore at most 400 kHz. The [pinned bootloader timing
implementation][timing] establishes that divider relationship and the version
condition for clearing the selection bit.

After enumeration, an owner may request four-bit default speed with
`sunxi_sdio_widen`. The owner first sets the card's bus-interface register
(CCCR address 7) to four bits. The controller then waits for an idle command
path and card, stops the card clock and calls the platform's module callback.
That callback selects the 400 MHz PERI0 parent with a linear divisor of 8 to 32.
The controller then sets four-bit width and restarts the clock. With divisor 8
the module runs at 50 MHz and the card clock at 25 MHz, the default-speed
maximum for a full-speed SDIO card.

Widening keeps the controller in the old timing mode (NTSR bit 31 clear) and
leaves the delay settings from preparation unchanged. For default speed at
both 400 kHz and 25 MHz, the [pinned bootloader timing implementation][timing]
uses command and data output at 180 degrees (DRV_DL bits 16 and 17 set) and a
sample delay of zero with the delay-enable bit set. The phase fields in NTSR
apply only in the newer 2x timing mode, so widening does not write them. Before
READY, the controller verifies clock control, width, both delay settings and
that NTSR bit 31 is still clear. A failed
step is terminal; the card keeps its four-bit setting and there is no one-bit
fallback. High-speed (50 MHz) timing is not implemented.

Preparation resets the local controller, selects CPU FIFO access, disables DMA
and controller interrupt delivery, applies initial delay settings, and performs
the clock-update commands. It waits across calls for reset and update completion.
It verifies output clock control, width, and timing selection before accepting
requests. The controller's eMMC reset register is not part of this radio path.

Shared resources remain explicit platform prerequisites:

- SDC1 module/bus clocks, reset release, shared storage gates, and the accessible
  register mapping must remain owned throughout controller use.
- PG0–PG5 must have the SDIO pin functions and correct I/O voltage. PG/PM rail
  ownership must remain coordinated with UART, wake signals, and Bluetooth.
- Radio power, enable/reset ordering, and settling time must already be established.
  The supported OCR mask must describe the admitted fixed supply range.
- Independent watchdog recovery must be qualified before a physical peripheral
  sequence. A software callback timeout cannot recover a stalled MMIO read.

The core does not modify CCU, pin, or PMIC registers. These prerequisites need
concrete board bindings before its register operations can run on hardware.

## One owner advances one retained request

[include/sunxi_sdio.h](../include/sunxi_sdio.h) defines offset-based register
callbacks, a barrier, and a monotonic clock. Callbacks must return promptly,
report failure, and avoid reentry. One serialized worker owns all API calls;
the core is not a concurrent task/ISR interface.
Register offsets, command fields and raw-interrupt bits come from
[include/sunxi_smhc.h](../include/sunxi_smhc.h), which the SD/eMMC core also
uses. SDIO stops on the pinned BSP's error mask `0xbbc2`.

Both host and card objects require zero-initialized storage before first use.
Successful initialization sets a permanent guard; later initialization attempts
are refused without mutation or I/O, including during active work or a fault.
There is no disposal API. Storage cannot be cleared or reused while requests,
callback contexts, faults, or hardware access remain owned.

Initialization copies the configuration without I/O. Preparation and request
submission admit work without touching registers. Each poll performs a bounded
step. A condition that has not completed returns control to the worker instead
of spinning. Each poll that changes the controller state or moves FIFO words
increments `progress`, which lets an owner repeat polls while work advances.

At one bit, a poll reads the status register before each FIFO word and moves at
most 16 words. At four bits, one status sample sizes each burst, as in the
SD/eMMC core: a read takes the FIFO fill level, or one word when the level reads
zero but FIFO-empty is clear, and a write takes the free space. A poll moves at
most 128 words, one 512-byte block, at four bits. The level interpretation is
taken from the manual and has not been confirmed on the board. An overstated
level would raise the raw FIFO underrun/overflow error, which is terminal.

A request remains owned until its completion result is taken. CMD53 buffers
borrow caller storage and may be unaligned, but their length must be a multiple
of four bytes. Input/configuration/output alias checks precede state writes.
Callback context storage must remain independent and alive under the platform
contract; its extent is not known to the core.

Clock observations occur before and after a poll's register work. A regressed
clock, expired absolute deadline, controller error, card-response error, or
callback failure enters terminal `FAULT`. Even a final callback that completes
late produces a timeout. The first error, response, interrupt status, progress,
request, and buffer reference remain available.

Taking a fault result does not release the request or return the controller to
service. Subsequent polls and interrupt acknowledgements perform no callbacks.
There is no automatic reset, abort, resubmission, or power cycle. External
recovery must establish controller/card quiescence before disposing of retained
state or reinitializing it. A write that reports failure may already have taken
effect. The `transferred` count records confirmed software progress; it does not
prove that an unsuccessful operation had no device effects.

## Counted SDIO transfers do not send automatic CMD12

The core accepts CMD3, CMD5, CMD7, CMD52, and CMD53. CMD5 expects an R4 response
without response-CRC checking. CMD3, CMD7, CMD52, and CMD53 enable response-CRC
checking. It validates source-defined R6, R1 error, and R5 error fields without
requiring informational status bits to be zero.

| Operation | Implemented behavior |
| --- | --- |
| CMD52 | One function register byte; optional read-after-write for writes |
| CMD53 byte mode | 4–512 bytes, in whole words; count zero encodes 512 bytes |
| CMD53 block mode | 1–511 counted blocks, each 4–512 bytes in whole words; total at most 4,096 bytes |
| Fixed address | Repeated FIFO access to the same card address |
| Incrementing address | Reject transfers crossing the 17-bit card address space |

The [SDIO request builder][sdio-ops] supplies a counted data request without a
stop command. The [Linux controller parser][controller] enables automatic stop
only when a request requires it. The bootloader's generic storage path enables
auto-stop for multiple blocks; that behavior is not applied to SDIO CMD53.
The native core emits neither automatic nor manual CMD12.

The [bootloader PIO path][pio] establishes whole-word CPU FIFO transfers.
Arbitrary byte tails are unsupported in this implementation. CMD52 handles byte
registers and card information. A later DMA or validated partial-word path must
cover other CMD53 lengths before claiming unrestricted SDIO support.

A data request completes only after command completion, a valid response,
data-over status, and the requested FIFO byte count. CMD7 and data writes also
wait for the controller's busy indication to clear. FIFO progress alone does
not establish completion. Late raw controller errors take precedence over a
success result.

PIO uses no DMA descriptors or cache-maintenance operations. Its eventual worker
must service the FIFO frequently enough to avoid overrun or underrun at the
selected clock. At 25 MHz with four lines, a 1,024-byte FIFO fills or drains in
about 80 microseconds. The register model does not qualify that scheduling
behavior, and the manual does not state whether the controller stops the card
clock when the FIFO is full or empty.

## SDIO interrupt acknowledgement belongs to the function owner

The raw SDIO interrupt bit is separate from command completion. Polls retain
it in `sdio_pending`; command-status acknowledgements exclude bit 16.
A command completion therefore cannot discard a function interrupt.

After the function owner services the interrupt source on the card, it may call
`sunxi_sdio_ack_irq` while the host is ready. The core acknowledges only bit 16
and reads back raw status. Immediate reassertion remains pending. An
acknowledgement failure retains the error and stops further access.

This component polls raw status and keeps controller interrupt delivery disabled.
A GIC binding, ISR notification, and the card's function/master interrupt-enable
registers still need a coordinated lifecycle. The API does not claim to service
Wi-Fi firmware events or clear their device-specific causes.

## Enumeration reads common and function identities with fixed bounds

[drivers/storage/sdio_card.c](../drivers/storage/sdio_card.c) uses the prepared
host exclusively while enumerating an I/O-only card. Its operations are:

1. Query CMD5, intersect the card's operating-condition register (OCR) with the
   admitted supply mask, and poll readiness with an explicit interval and deadline.
   Mixed memory/I/O cards and voltage-switch requests are outside this path.
2. Obtain a nonzero relative card address through CMD3 and select it with CMD7.
3. Read common control register revisions and capabilities through CMD52.
4. Read each function's basic register area and its card-information structure
   (CIS), including manufacturer/device IDs and advertised block-size limits.

The card worker advances at most one host step and admits at most one subsequent
command per call. Each command has a deadline within the overall enumeration
deadline. The shared host clock also catches a timeout after the final response.
Partial identities and the last result remain available after failure.

CIS traversal accepts common function 0 and up to seven I/O functions.
Each structure is bounded to 4,096 examined bytes and 128 tuple headers, within
the 17-bit address range. NULL and end tuples follow the [pinned parser][cis].
Manufacturer tuples need four payload bytes. Function-extension tuples need the
source-defined minimum length for the reported SDIO revision. Unknown tuples
are counted and skipped after bounded reads; their complete contents are not
retained by this identification layer.

A function without its own manufacturer tuple inherits the common identity.
Conflicting duplicate manufacturer tuples, invalid pointers, unsupported
revisions, malformed recognized tuples, or exhausted traversal bounds stop
enumeration. A missing function-extension tuple leaves its capability absent;
block-mode request construction then refuses an unestablished block size.
The source's compatibility workaround for a truncated SDIO 1.1 extension tuple
is not implemented and would currently require a documented follow-up.

[include/sdio_card.h](../include/sdio_card.h) exposes pure CMD52 and CMD53
constructors after enumeration. They validate function numbers, register ranges,
transfer bounds, address increment limits, and advertised block capability.
They do not enable a function or program its current block size. A caller must
complete and verify those operations before submitting the corresponding
CMD53 request. The advertised maximum is not the programmed block size.

## Focused evidence covers the implemented boundaries

The [register-model fixture](../checks/sdio-controller.c) passed with address
and undefined-behavior sanitizers. It covers enumeration with two functions,
manufacturer inheritance, CMD52 read-after-write, a 20-byte radio identity
command, a 512-byte byte-mode read, a 1,024-byte block-mode write, and interrupt
reassertion. Error cases cover version rejection, reset timeout, clock regression,
R5 errors, missing completion events, late completion, and an invalid CIS pointer.

Focused follow-up cases passed for CMD7 informational/error status, partial
FIFO callback failure, malformed function-extension tuple shapes, and refusal
to reinitialize active or faulted objects. The
earlier enumeration/transfer evidence was reused. These corrections change
response-bit and malformed-tuple rejection; the earlier successful responses
and valid tuples remain accepted. The initialization guard preserves the
first initialization path; its refusal check confirms retained state and
callback counts remain unchanged.

A later case, selected with `--case widen`, covers four-bit widening: refusal of
out-of-range divisors and repeated requests, the module callback with the card
clock stopped, readback of clock, width, delays and timing mode, a level-sized
128-word read burst from one status sample, and terminal faults when the
module change fails or the sample delay no longer matches preparation.

Raw evidence is under `~/.cache/sunstoneos/evidence/sdio-controller-20261016/`.
`results.json` identifies the initial successful direct build/run;
`select-io/results.json`, `cis-shape/results.json`, and `reinit/results.json`
identify the focused follow-ups. The runner was added
after the initial session and did not trigger an unchanged rerun.
The evidence includes compiler, command, source/header hashes, and executable
identity. Freestanding AArch64 object evidence is under `aarch64-lifecycle/`.
The card layer uses the runtime's `memset` plus the controller API.

```sh
python3 checks/sdio-controller.py \
  --reference-dir /path/to/pinned/kernel \
  --evidence /path/outside/repository/sdio-evidence
```

No physical board operations or performance measurements were performed.
No board executable exists for the test queue. Physical qualification requires
shared-resource admission, the board binding, watchdog protection, and a worker
that can sustain the FIFO's service requirements. Physical four-bit operation,
high-speed timing, DMA, arbitrary CMD53 tails, function setup, radio firmware
upload, and native network/HCI integration remain outstanding.

## Reference identities

Kernel references use revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
Bootloader references use revision `b791be842935b27268ae3d00e943a9075495f30a`.
The native state machines use the register and protocol facts from these files;
Linux and U-Boot execution or allocation code is not linked into SunstoneOS.

| Source | SHA-256 |
| --- | --- |
| Kernel `bsp/drivers/mmc/sunxi-mmc.c` | `52c7f92db77290437b7818f3c3a92a2956c1ab637214794e56220d0dde99ce16` |
| Kernel `bsp/drivers/mmc/sunxi-mmc.h` | `0945ee0ea5e47082c2ff159e578380671f86f4cd797f4fb3a853bc6f8cde0da3` |
| Kernel `bsp/drivers/mmc/sunxi-mmc-v5p3x.c` | `348a6893ae256b2df712813e998ab2b6a13341dd8c57f6fcfb31656fe735d94c` |
| Kernel `drivers/mmc/core/sdio_ops.c` | `23e6cd52c802263e69e95eacc4054a9afcd751da746ef6fa53f8312f93a7aa29` |
| Kernel `drivers/mmc/core/sdio_cis.c` | `5ff82f36a6c15fb9bb8836ec54bb53c44a0be9210e898a0db647bc702716d118` |
| Kernel `include/linux/mmc/sdio.h` | `c2657f975c3f38c5c1071c63c7638913cc227296794010d3745d15e1fe414cbe` |
| Kernel `include/linux/mmc/mmc.h` | `9762e69cbbadb447abe1ba191ad2fc883d645c808024dfd810c12436ad4a695b` |
| Bootloader `drivers/mmc/sunxi_mmc.c` | `890c3c48de3592cb214d4f32195c5e55fe2a03cfcd0d16d13cb2e36f4ca354c5` |
| Bootloader `drivers/mmc/host/sunxi_mmc_host_tm4.c` | `b3d849ca1cb049753d9bcb0d38d90f86c8e9179dd5916a91d91955f8104bc082` |

[soc]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi
[controller]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/mmc/sunxi-mmc.c
[sdio-ops]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/sdio_ops.c
[cis]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/sdio_cis.c
[timing]: https://github.com/orangepi-xunlong/u-boot-orangepi/blob/b791be842935b27268ae3d00e943a9075495f30a/drivers/mmc/host/sunxi_mmc_host_tm4.c
[pio]: https://github.com/orangepi-xunlong/u-boot-orangepi/blob/b791be842935b27268ae3d00e943a9075495f30a/drivers/mmc/sunxi_mmc.c
