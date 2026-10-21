# Native radio firmware preparation, upload, and start

SunstoneOS implements a bounded firmware engine for the source-selected
AIC8800D80 SDIO bootstrap protocol. It verifies pinned artifacts, uploads
Bluetooth data and patches, applies their configuration, loads the Wi-Fi image,
publishes its patch metadata, and requests application start. The engine uses
the [native SDIO transport](radio-transport.md), controller, and card APIs.
An integrated host register model passes; physical firmware execution remains
unqualified.

`START_CONFIRMED` means that the published start-confirmation message arrived.
It does not establish Wi-Fi traffic, Bluetooth HCI operation, pairing, or audio.
The loader keeps Bluetooth patch application and Wi-Fi startup as separate
progress states. The subsequent [runtime worker](radio-runtime.md) handles the
binding exchange and continuous receive; both remain physically unqualified.

## Selection follows completed identity and pinned artifact availability

The transport admits firmware only after the complete bootstrap identification
transaction succeeds. The same awake processor, power/reset lifetime, and
exclusive owner must continue across the handoff. There must be no stale debug
responses or outstanding operations. The firmware engine recomputes revision
and H status from the raw identity words and compares those words with the
transport's completed identity before accepting ownership.

The [published normal-mode mapping][bootstrap] selects these artifacts:

| Verified identity | Bluetooth ADID, patch, and table | Wi-Fi image | Native admission |
| --- | --- | --- | --- |
| Non-H revision 3 or 7, source U02/U03 | Published U02 artifacts | `fmacfw_8800d80_u02.bin` | Accepted with exact sizes and hashes |
| H flag with accepted revision 1, 3, 7, or 15 | Published U02 artifacts | `fmacfw_8800d80_h_u02.bin` | Accepted with exact sizes and hashes |
| Non-H revision 1, source U01 | U01 artifacts | U01 image | Refused because the inspected overlay lacks the required artifacts |
| Non-H revision 15, source D80 U04 | U04 artifacts | U02 Wi-Fi image | Refused because the required Bluetooth artifacts are absent |
| Any other revision | Unspecified | Unspecified | Refused |

The source validates revision 1, 3, 7, or 15 before testing the H flag. Its H
branch precedes the non-H revision branches and selects the U02 Bluetooth set
for every accepted H revision.

IDs alone never select firmware. A different available image, including an RF-test
or combined image, cannot substitute for a missing selected artifact.

The admitted profile matches the source's normal SDIO configuration: shared
antenna coexistence, Bluetooth UART at 1,500,000 baud, hardware flow control,
low-power signaling, and AMSDU reception (aggregated MAC service data units).
The Bluetooth port field uses `AICBT_BTPORT_UART`, numeric value 2. It uses the source's default hardware
information value of minus one and transmit-power value `0x6f2f`. Out-of-band
interrupt, adaptivity-test, RF-test, USB, and external configuration-file overrides
are outside this profile. UART/HCI and wake-signal bindings are still required
before claiming Bluetooth operation.

## Immutable blobs are verified before the first firmware command

[include/aic8800_firmware.h](../include/aic8800_firmware.h) defines four borrowed
blob roles: Bluetooth ADID data, Bluetooth patch, Bluetooth patch table, and Wi-Fi
image. These are memory descriptors, not runtime filesystem paths. Initialization
checks the selected sizes, disjoint storage, identity mapping, and profile without
performing I/O. Object storage must initially be zero; reinitialization is refused.

The caller must keep all bytes immutable and pinned throughout verification,
upload, completion, fault, and external disposal. The blobs cannot overlap one
another, either worker object, the card, or the controller. Callback storage must
also remain independent under the platform ownership contract.

Before any firmware command, the engine computes SHA-256 over every admitted
blob and compares the digest with its built-in artifact catalogue. Each step
hashes at most 4 KiB, so the four D80 artifacts need 98 steps. The accepted
sizes bound total work; the shared absolute deadline also applies during hashing
and table validation. The engine uses the existing software SHA-256 in
[storage_digest.c](../drivers/storage/storage_digest.c), which follows
[FIPS 180-4][sha256]. The host runner checks it against complete artifacts that
it hashes independently, and against a changed final byte of the Wi-Fi image.
This verifies artifact identity against the pinned catalogue.

The complete Bluetooth table is authenticated by its pinned digest before its
records are interpreted. Bounded record validation checks lengths, types,
mode-field count, and the first record's destinations. The engine never mutates
the artifact: configuration substitutions are applied to individual outgoing
requests. The pinned table contains seven records, no extension images, and
2,880 executable address/value pairs. Its version record is informational.

## Bluetooth patching precedes Wi-Fi startup

The [published loader][sender] transfers image data in chunks of at most 1,024
bytes. Its block-write request always contains a 1,032-byte parameter structure,
including the full data array, even for the final short chunk. The native encoder
zeros unused bytes and the transport pads each request to 1,536 bytes.

| Stage | Implemented operation |
| --- | --- |
| Bluetooth ADID | Upload 1,708 bytes to radio address `0x00201940` |
| Bluetooth patch | Upload 24,744 bytes to `0x001e0000`, the destination selected by the table |
| Bluetooth table | Apply all executable pairs in source order; replace the nine mode values with the admitted defaults |
| Bluetooth power-on delay | Wait at least 500 microseconds after the power-on record |
| Wi-Fi image | Upload the selected normal image to `0x00120000` |
| Wi-Fi metadata reads | Read configuration base, patch-structure base, version, and patch-buffer base from the uploaded image's source-defined locations |
| Wi-Fi patch publication | Publish both magic words, one patch pair, and zero block sizes for the four optional patch blocks |
| Wi-Fi start | Send `HOST_START_APP_AUTO`, value 1, at `0x00120000` and retain its confirmation |

These addresses belong to the radio processor's memory space. They are never
used as A733 CPU or MMIO pointers.

The accepted images contain version `0x06090101`. Both name patch structure
`0x00174000` and pair buffer `0x0017cd7c`; their configuration bases are
`0x001730dc` for non-H and `0x0017310c` for H. The engine checks each device-read
value against the corresponding bytes in the verified image before publishing
patch pointers. This checks the required metadata; it does not read back and
compare the entire uploaded image.

The admitted AMSDU patch writes the source's configuration offset `0x0170`
with value `0x0100000a`. System and masked RF configuration tables are empty
for the selected SDIO profile. The engine does not copy USB-only operations.

## Confirmations, backpressure, and uncertain effects retain ownership

The transport accepts firmware requests only from its claimed engine after
completed identity. It exposes no raw application command interface. Each poll
performs bounded firmware preparation, controller progress, or record parsing.
One outstanding message remains owned until its matching confirmation arrives.

| Request and confirmation IDs | Required confirmation payload | Interpretation |
| --- | --- | --- |
| Memory write `0x0402` / `0x0403` | Eight bytes | Preserve address/data fields without an invented echo comparison |
| Block write `0x040b` / `0x040c` | Four bytes | Preserve `wstatus`; the source sender ignores it and specifies no success-value rule |
| Memory read `0x0400` / `0x0401` | Eight bytes | Correlate the requested address and verify the returned metadata value |
| Start application `0x040d` / `0x040e` | Four bytes | Preserve `bootstatus`; the source uses its low byte as hardware information |

The engine keeps the latest confirmation fields and the first nonzero opaque
block status for diagnosis. It does not reinterpret the start status as a
zero-means-success error code. Unexpected message IDs, wrong read addresses,
and other valid control records remain unrelated messages in the transport's
owned receive aggregate.

The consumer receives a borrowed view and must copy any asynchronous work before
consuming it. While a record is held, the aggregate remains unchanged and no new
command is submitted. The deadline still advances. Binding indication `0x0417`
requires its declared 16-byte challenge, remains subject to the same backpressure,
and sets a retained `binding_required` flag. The loader preserves the first
challenge for the runtime worker, which computes the reply and checks confirmation.

After the start confirmation, the transport drains the current aggregate before
reporting `START_CONFIRMED`. This endpoint does not provide continuous runtime
receive service. The [runtime worker](radio-runtime.md) handles later aggregates
and binding indications. Normal network operation remains separate work.

A failed callback may follow a partial transfer or a write that took effect.
Faults retain both workers, immutable blobs, outgoing bytes, the controller
request, received bytes, and diagnostic state. The first error remains terminal;
there is no reset, retry, image substitution, rollback, or disposal shortcut.
External recovery must quiesce the device before releasing this lifetime.

## Artifact provenance and redistribution remain separate

The artifacts come from the [published firmware overlay][artifacts] at revision
`dc163b0ed2e0272d9ee31977a0e0a04884f0ae61`. The inspected tree has no firmware
license file or radio-specific redistribution grant. Its README only identifies
it as Orange Pi firmware; Linux driver licensing does not establish terms for
these separate binaries. The implementation records their identity and loads
caller-supplied bytes. It adds no firmware blob to Git or an OS distribution.
Applicable redistribution terms remain a packaging dependency.

| Artifact | Bytes | SHA-256 |
| --- | --- | --- |
| `fw_adid_8800d80_u02.bin` | 1,708 | `a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56` |
| `fw_patch_8800d80_u02.bin` | 24,744 | `4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0` |
| `fw_patch_table_8800d80_u02.bin` | 23,288 | `5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60` |
| `fmacfw_8800d80_u02.bin` | 341,212 | `55b5a20865aca7cd2edaae1733a36da45d2b0b29060bc243d8e9ec212e139f6c` |
| `fmacfw_8800d80_h_u02.bin` | 341,260 | `ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7` |

## Integrated evidence and focused follow-up checks

[checks/aic8800-firmware.py](../checks/aic8800-firmware.py) verifies source and
artifact hashes, constructs an independent expected command transcript from
those artifacts and the pinned message enum, then builds the real firmware,
transport, codec, card, and controller implementations with sanitizers.

The complete H-image model traversed 3,256 commands: 361 image blocks, 2,880
Bluetooth pairs, four metadata reads, ten Wi-Fi patch writes, and start. The model
checks 1,536-byte packet padding, FIFO credits, stage order, the power-on delay,
non-echo write fields, nonzero opaque block status, and unrelated/binding record
backpressure. Fault cases cover missing confirmation, partial transmission, and
corrupted artifact rejection before the first firmware command. Selection checks
refuse unavailable revisions and an H image used for non-H identity.

The model forks a verified simulated checkpoint for transport fault cases,
avoiding repeated hashing and bootstrap execution. Existing SDIO and bootstrap
suites were not rerun. Raw commands, source/header identities, transcript,
executable, and result are under
`~/.cache/sunstoneos/evidence/radio-firmware-20261016/model/`.

Evidence reuse excludes the Bluetooth UART selection word from that complete
transcript: its model and implementation both used value 1 before source enum
review established value 2. The focused `--case mode-alias` check derives the UART
value from the pinned enum and passes the corrected command encoding. It also
passes refusal of message-view output pointers that overlap the firmware object
or immutable artifact bytes, with retained-message deadline coverage.
Results are under the adjacent `mode-alias/` directory. Evidence for the remaining
3,255 commands and unaffected state transitions is reused without another upload
simulation.

Both changed runtime files compile once for freestanding AArch64 with Clang
21.1.8 and warnings as errors. Commands, source/object hashes, and symbols are
in the adjacent `aarch64/results.json`. Their runtime dependencies beyond the
native radio/card/controller APIs are `memcpy` and `memset`. Final input hashes
and the evidence-reuse rationale are in `final-inputs.json` at the evidence root.

```sh
python3 checks/aic8800-firmware.py \
  --artifacts /path/to/pinned/aic8800d80 \
  --reference-dir /path/to/pinned/aic8800_bsp \
  --evidence /path/outside/repository/radio-firmware-evidence
```

Physical execution still requires qualified recovery, shared power/pin/clock
ownership, controller bindings, and the selected radio identity. Physical binding
verification, normal network traffic, Bluetooth HCI/profiles, runtime interrupts,
and sleep/wake operation remain outstanding. No board operation, test-queue
submission, or performance measurement was performed.

Source references use kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The focused runner pins
`aic_bsp_driver.c` (`43439d69cd1c3f537ee90f50d4bc52d2a54379f41092f625ded080daacf7de6e`),
`aic_bsp_driver.h` (`9f72ebdd743ac0af134df50ebc36ea25876b9a361a89b82e17a4a8c2e4d918b3`),
`aic_bsp_8800d80.c` (`fcf647395a7f007f22cd19f4e15dcc821e924da55b72528395a9972e5c6290e8`),
and `aicsdio.c` (`f08bcfa63f9b7226f4b26832ee9b6088c057fd5ee06389f3ad872f13f9246f0c`).

[bootstrap]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_8800d80.c
[sender]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.c
[artifacts]: https://github.com/orangepi-xunlong/firmware/tree/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80
[sha256]: https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
