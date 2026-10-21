# Native WPA2 station security

SunstoneOS implements the WPA2-Personal four-way handshake and group-key renewal
through the BSD RSN implementation from wpa_supplicant 2.12. A bounded native
adapter connects its cryptography and state machine to the AIC8800D80 key,
controlled-port and EAPOL transport commands. RSN means Robust Security Network;
EAPOL carries authentication and key-management messages over the local link.

The host composition verifies this path with synthetic access-point messages.
The component also compiles for freestanding AArch64. Physical firmware key
installation, encryption, receive integrity and entropy quality remain
unqualified. An authorized software state does not establish a working IP network.

## Admission requires observed network configuration

The initial profile accepts WPA2-Personal with CCMP-128 encryption, a caller-supplied
32-byte pairwise master key, and the access point's actual RSN information element.
The caller supplies the same beacon evidence used to admit association. The adapter
checks that the observed element permits PSK authentication and CCMP, and rejects
mandatory protected management frames. It supplies the station's matching RSN
element to the real state machine; it does not construct a fictitious access-point
security state.

Association and permission for radio transmission remain prerequisites described
in [station association](radio-association.md). Security admission copies its
configuration and binds it to the current BSSID, station identity and association
generation. The caller must keep its entropy callback and context alive under the
same serialized privileged owner. The context must not alias owned driver objects.
The callback must fill every requested byte with cryptographically suitable entropy
and return exactly one. A timestamp, device identifier or deterministic fallback
cannot satisfy production admission.

The current API takes a prederived master key. Passphrase derivation and credential
storage are separate requirements. WPA3, Enterprise EAP, protected management
frames, fast transition, multi-link operation and offloaded PMKSA cache selection
are not enabled. PMKSA is cached authentication state; the adapter keeps one network
context and disables fast reauthentication.

## Firmware actions complete in order

The upstream state machine computes and checks message authentication codes,
decrypts wrapped group keys, validates replay counters and derives the pairwise
transient key. Its callbacks copy outgoing EAPOL messages and key installations
into an eight-entry owned queue. Only one firmware operation runs at a time. The
supported handshake produces at most three queued actions before the adapter
returns to its worker. Queue exhaustion causes a retained terminal fault.

For the initial handshake, the adapter waits for the fourth EAPOL message's actual
transmit confirmation, then the pairwise-key and group-key confirmations. It opens
the controlled port only after the state machine has completed and all queued
actions have completed. A group renewal installs its group key before sending its
response and reopening data access. Application EAPOL transmission is excluded
while the supplicant owns authentication.

Ordinary packet transfer completion and EAPOL acknowledgement remain distinct.
The adapter requires the firmware's transmit-done and acknowledgement bits for its
EAPOL actions, and rejects retry indications. A key-add confirmation has a defined
status and hardware index; nonzero status causes failure. Key-delete and
controlled-port senders in the pinned driver request confirmations without a
response structure. Those confirmations retain up to 64 opaque bytes. Their
matching message IDs establish trusted firmware completion, without an invented
success field or empty-payload requirement. Hardware qualification must verify
these semantics and ordered, duplicate-free confirmation delivery. Responses have
no operation sequence field beyond this exclusive serialized protocol.

The key-add layout follows the published 44-byte parameter structure. Replacement
records follow the source driver's returned hardware-index behavior. A replacement
confirmation replaces the corresponding retained key record; the firmware's
replacement and old-index disposal semantics require physical qualification.

## Pending input cannot block confirmation processing

The adapter retains one EAPOL frame while an action is pending. If further frames
from the same admitted access point arrive, it preserves the oldest frame and
drops excess retransmissions. A saturating counter records these drops without
recording packet contents. Firmware confirmations can therefore continue through
the same receive stream. The access point may retransmit dropped messages.

Unknown commands and unsupported receive formats retain the existing raw-record
ownership path. A generic dispatcher must consume or explicitly dispose those
records; retaining one pauses further bus work. This includes rejected CCMP replay
records and unsupported aggregation or reordering formats. They are not counted
as delivered Ethernet frames.

## Data access depends on installed keys and association generation

The data worker admits the published firmware-decrypted CCMP receive format. It
checks the source descriptor, protected-frame bit, CCMP header, key identifier,
packet number, address direction and LLC framing before exposing Ethernet bytes.
The packet-number baseline comes from the authenticated key handshake. Separate
counters cover each traffic identifier and non-QoS traffic. A counter advances
only after the complete Ethernet frame is retained. Equal or lower numbers remain
raw records.

The pinned receive source removes the eight-byte CCMP header before LLC conversion
and does not subtract a separate integrity trailer on this path. SunstoneOS follows
that layout. Firmware integrity checking and decrypted-length semantics must be
confirmed on hardware; this adapter does not perform an independent CCMP decrypt
or message-integrity check. Plaintext ordinary traffic remains refused on a WPA2
association, while EAPOL can pass before authorization.

The upstream implementation prevents reinstalling the same pairwise or group key
when authenticated messages are retransmitted. The native key worker also avoids
reinstalling identical material into its existing slot. These paths preserve
receive packet numbers. A new association generation revokes all previous data
authority and faults an attached security owner. Successful software association
alone never grants ordinary data access.

## Failure retains ownership until confirmed disposal

Each command has a deadline within the transport's lifetime. The shared monotonic
clock is checked around transport operations; a timeout, uncertain bus write,
rejected key, entropy failure or generation mismatch closes host data access and
retains the master key, key records, pending actions and backend ownership.
Automatic reinitialization and retries cannot discard that state.

Normal retirement requires an idle action queue. It closes the firmware port,
deletes every known installed key with confirmations, destroys the RSN state,
and wipes the master key, allocation arena and timers before detachment. A failed
retirement keeps the terminal owner for external recovery. Retirement after an
unsolicited disconnection, with the link down, skips the port closure and the
pairwise-key delete because the firmware station no longer exists; it wipes the
pairwise record locally and deletes only the group keys. Retirement is refused
while roaming is unresolved. A library invariant
failure marks the owner failed and traps; the native exception/recovery path must
retain that owner. Secret-bearing diagnostics are disabled.

The OS port supplies bounded allocation and timers without a hosted process or
network API. Its arena contains 32 blocks of 2,048 bytes and four blocks of 16,384
bytes. Freed blocks are wiped. One privileged serialized invocation binds the
context-free upstream OS callbacks to their current owner. Interrupts and other
workers must not enter this port concurrently. There is no global entropy source.

## Sources, licensing and build integration

The external archive is [wpa_supplicant 2.12](https://w1.fi/releases/wpa_supplicant-2.12.tar.gz),
SHA-256 `08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6`.
Source files remain outside Git and are compared with the archive before use.
The archive's `COPYING` selects BSD terms and points to
`wpa_supplicant/README` for the license. Distribution must preserve those notices
and each compiled source file's copyright and license terms. No upstream source
is rewritten by the port.

The [official security advisory index](https://w1.fi/security/) was checked on
October 16, 2026. The
[PMKSA context advisory](https://w1.fi/security/2026-2/missing-network-context-validation-for-pmksa-caching.txt)
requires network-context and authentication-method matching. The authenticated
archive contains both the caller's context/method arguments and the cache's
matching checks. The
[multi-link parsing advisory](https://w1.fi/security/2026-1/missing-ml-parsing-validation.txt)
identifies additional bounds requirements. The source inspection checks declared
common-info limits and link-ID bounds in the shared helper; this profile excludes
multi-link operation. These checks establish specific mitigation inputs. Continuing security review
remains required.

The firmware ABI reference is the board-driver revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, specifically `lmac_msg.h`, `lmac_mac.h`,
`rwnx_msg_tx.c`, `rwnx_main.c`, `rwnx_rx.c` and `rwnx_rx.h`. The source-layout
inspection records message IDs 36/37 for key addition, 38/39 for deletion and
5124/5125 for controlled-port state, with parameter sizes 44, one and two bytes.

Include `cmake/radio-security.cmake` and call
`sunstone_enable_radio_security(target /absolute/path/to/wpa_supplicant-2.12)`
for the existing native radio target. The helper verifies the external tree, adds
the selected RSN/crypto files and OS adapter, and defines `SUNSTONE_AIC8800_RSN`.
The target must already link the radio transport, runtime, Wi-Fi, data, controller
and native memory primitives. Targets using the data/transport worker also need
`drivers/net/aic8800_security.c` when RSN attachment is disabled. That small worker
has no external-library dependency. No callable security-success stub replaces
the enabled adapter.

## Focused validation and remaining qualification

The source-layout check uses the pinned declarations. The composed host fixture
starts at an explicit associated-firmware checkpoint and runs the real RSN,
crypto, key, data, SDIO transport, card and controller code under address and
undefined-behavior sanitizers. Python HMAC-SHA1 and AES key wrapping independently
construct synthetic access-point messages and expected station responses.

Passing cases cover the four-way handshake, group renewal, equal-replay
refusal, authenticated higher-replay retransmissions without key or packet-number
reset, decrypted receive ownership, raw replay retention and confirmed key
retirement. Focused failure cases cover invalid message authentication, oversized
EAPOL length, entropy refusal, rejected keys, uncertain transmission, deadline
expiry and generation revocation. A retransmission burst verifies that the oldest
pending frame survives while confirmations continue. Earlier initialization,
firmware-upload, association and controller evidence is reused.

Commands and input identities are recorded under
`~/.cache/sunstoneos/evidence/radio-security-20261016/`. The host recipe is
`checks/aic8800-supplicant.py`; select a case rather than rerunning every case for
each change. `checks/aic8800-security.py --native` compiles and links the actual
AArch64 dependency closure without libc. Its link-only entry is not a board
executable or proof of radio operation.

Physical work still requires qualified board recovery, clock/pin/power ownership,
a real entropy provider, and admitted radio transmission. Confirm firmware key
replacement/deletion, controlled-port behavior, EAPOL status and CCMP integrity
before qualifying secure data. IP networking, credential lifecycle, the excluded
security modes, broader receive profiles and Bluetooth remain separate required
parts of full board support.
