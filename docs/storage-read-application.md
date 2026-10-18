# Native read-only microSD application

`sunstone-a733-storage-read.elf` identifies an inserted SD card on SD0 and reads
one 512-byte logical sector at address zero. It uses the native MMC controller
through the existing common clock, pin, rail, PMIC, register-lock and memory
services. It runs as a supervised RAM-only application with USB0 results and
manual whole-board power-removal recovery. It does not arm a watchdog.

This application removes a discovery dependency in the maintenance application:
identification requires neither a known card identity nor an owned media range.
The maintenance update grant and its watchdog qualification remain unchanged.
The application never creates or binds an update service. It has no sector-write,
erase, trim, raw-command, media-range or storage-update input.

The image selects SD0, its PF0–6 reservation and the existing one-bit legacy
profile. PF1–3 carry the one-bit interface; PF6 supplies active-low detection.
The controller receives an 800 kHz module clock and generates a 400 kHz card
clock. Existing cold controller setup includes its local reset. The application
adds no board reset, post-fault controller reset, recovery pulses or retry.

## Admission and ownership

The full 864-byte session is hashed and privately sealed in EL3 before provider
construction. Its 592-byte board prefix selects workload 2: the shared PMIC and
USB0 services, without radio or USB host activity. The suffix binds a distinct
MMC owner, authority, five evidence hashes, exact inherited and desired pin
fields, voltage mask, and bounded identification/read deadlines. It contains no
card identity or media allocation. The task derives absolute deadlines from the
running session rather than accepting uploaded absolute times.

Future physical use requires current-instance evidence for all of these facts:

- Exclusive cold SD0/card protocol state, disabled and drained IRQ193, no DMA,
  queue, encryption, remote client or clock/reset writer.
- A private, permanently reserved PIO buffer with the actual identity-mapped,
  cacheable, inner-shareable RAM translation and no aliases.
- Source-qualified SD slot wiring, PF routing, pulls and drives, active-low card
  detect, PF 3.3 V domain, and no alternate-pad writers.
- Actual DCDC1/DC1SW2 3.3 V and CLDO5 1.8 V supply topology and settling.
- One attempt, full power-removal recovery, permanent retention after uncertainty,
  no reuse, watchdog arm or reset retry.

The actual PMIC service also reads each programmed supply through its retained
rail token. These reads prove programmed values, not wiring, measured voltage or
settling. A card-detect observation does not establish the remaining authority.
The offline fixture uses explicitly modeled records and cannot substitute for
physical evidence. Missing board facts block physical execution, not compilation.

The designated MMC task owns every provider call. The native adapter checks
actual EL1/task/interrupt context and the common register mutex before controller
access. The default binding rejects FIFO writes and media-write commands without
an actual update authority. Shared code retains conditional update references;
this application never supplies that authority. Its fixed command path is SD
identification, CMD17 for sector zero and final CMD13 status validation.

## Results and failure handling

A successful `take` requires the controller's completion, integrity, idle/FIFO and
final card-state checks. A later task step copies the sector into separate USB
storage, hashes it and rechecks session eligibility before publication. The PIO
buffer is never exposed to USB and remains reserved after cancellation or fault.

Request byte zero asks for a copied record. The 256-byte little-endian header
contains status, epoch, generation, sector digest, complete CID/CSD/OCR/capacity
and RCA, addressing mode, raw controller/result facts, protocol progress, reason
and the immutable build ID. Only success appends the 512-byte sector. Partial
card facts on failure remain diagnostic; `card_valid` distinguishes completed
identification. The observer makes no filesystem-validity claim, including when
sector bytes happen to contain a familiar signature.

Request byte one irrevocably cancels the session. Cancellation or deadline
revokes output before metadata retention. The active read deadline also cuts off
success publication and new USB queueing, even while the board session remains
live. Eligibility is checked again before
copying and each USB queue operation. Bytes already accepted by TinyUSB cannot
be retracted; cancellation preserves their completion accounting and abandons
unsent bytes. A cancelling host must permanently reject that session's output
and discard any incomplete frame.

`a733_mmc_platform_retain()` checks the designated current owner, serializes
metadata, rejects update-bound providers and retains existing reservations. It
makes no time, presence, admission, controller, reset or release call. BUSY
serialization or manager bookkeeping is retried explicitly. The application
does not poll a healthy idle core to manufacture a fault.

An uncertain controller access can retain the common register mutex and stop
USB as well as storage. Diagnostics remain in owned RAM; an error transfer is
not guaranteed. Deadlines cannot recover a stalled MMIO instruction. Full power
removal remains necessary before reusing resources. When USB is the only supply,
unplugging it removes board and card power; the coordinator verifies the new
instance after reconnect. Any additional supplies must also be removed.

## Build and physical request

Enable `SUNSTONE_A733_STORAGE_READ` and build target
`sunstone-a733-storage-read` with the pinned board dependencies. The target
inherits the actual supervised boot and shared providers, adds the existing MMC
platform/native/core sources, and records their build identity. Frozen evidence
must include the final ELF, compiler dependency closure and focused results.

`apps/storage-read/prepare.py` prepares a new epoch, generation, lease and sealed
upload bundle from the tested image, manifest, session template and reviewed
current-instance authority. It reads each input once and validates derived ELF,
contract, session and evidence against those frozen bytes.
`tools/check-a733-storage-read-supervised.py` uses the existing queue, lock,
sealed transport, native-instance selection and retained manual-recovery path.
The coordinator alone submits, reviews, claims and executes a physical request.

The initial profile does not qualify faster SD modes, wider buses, eMMC, UFS,
filesystem operations, storage updates or watchdog recovery. Those remain
separate hardware requirements. Software models validate composition and
failure handling; they do not establish physical SD operation.

The first software batch is preserved under
`~/.cache/sunstoneos/evidence/storage-read-native-20261018/`. Its linked model
executes the actual application, MMC platform, native aperture, controller and
clock metadata. Rail, pin and PMIC boundary results, task scheduling, card/MMIO,
translations and USB queues are modeled. The trace includes SD identification,
CMD17 and final CMD13 without a media-write command or FIFO write.

Focused cases cover post-take cancellation, deadline expiry, CRC failure with a
retained borrowed buffer, partial USB queue cancellation, complete session
sealing, native owner/write guards, metadata BUSY retries, durable fragmented
capture and missing-authority refusal. The deadline correction recompiles only
the application body and build ID. Its changed case expires the read deadline
while the board remains live; the other original case records remain preserved
against their original image. The reconciliation records this reuse explicitly.
No physical card operation or watchdog recovery is established by this batch.
