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

For WPA2 the API takes a prederived master key. Passphrase derivation and credential
storage are separate requirements. WPA3 association, Enterprise EAP, protected
management frames, fast transition, multi-link operation and offloaded PMKSA
cache selection are not enabled. PMKSA is cached authentication state; the adapter keeps one network
context and disables fast reauthentication.

## WPA3 personal authenticates but cannot yet associate

WPA3 personal authenticates with Simultaneous Authentication of Equals (SAE)
and requires protected management frames (PMF). SAE is a password exchange of
two Authentication frames in each direction: a commit carrying an
elliptic-curve scalar and element, then a confirm carrying a hash over both
commits. Both sides derive the same pairwise master key (PMK) and PMK
identifier (PMKID) only when they used the same password.

The SAE exchange is implemented. Association with the SAE key-management
suite is not, so the supplicant refuses to claim an SAE configuration.

### The station exchange

A supplicant initialized with `key_mgmt` set to `AIC8800_SUPPLICANT_SAE`, a
password of 1 to 64 bytes and a zero `pmk` runs the station side through
`aic8800_supplicant_sae_start` and `aic8800_supplicant_sae_receive`
([aic8800_sae.c](../drivers/net/aic8800_sae.c)). Start writes the first commit
as an Authentication frame body. Receive takes each frame body that the
access point sends and returns one of four results: a frame to send, a
discarded frame, acceptance, or failure. The caller passes only frames whose
source is the BSSID given to start. The observed access-point RSN element must
offer the SAE suite (00-0F-AC:8) and CCMP, and must not require PMF.

The exchange follows the station logic of the pinned wpa_supplicant release
and runs its unmodified `sae.c` and `dragonfly.c`:

- **Groups.** The station tries the NIST P-256, P-384 and P-521 curves (groups
  19, 20 and 21) in that order. A refusal with status 77 moves to the next
  group. A commit that uses hash-to-element lists the refused groups, and an
  access point that reports refusing a group this station enables is rejected
  as a downgrade.
- **Password element.** When the first capability octet of the access point's
  RSN extension element (`ap_rsnx`) has its hash-to-element bit set, the
  station derives the password element by hash-to-element (H2E). Otherwise it
  uses hunting-and-pecking. SAE-PK and password identifiers are not
  supported.
- **Anti-clogging.** A token request with status 76 resends the same commit
  with the token, inside a token container element when H2E is used.
- **Bounds.** Token requests and group changes together allow at most five
  commits. A frame received at or after the start deadline fails the exchange.
- **Refusal.** A commit that reflects the station's own values is discarded
  and leaves the station waiting. A wrong password, an altered confirm, an
  unexpected status, sequence or state, or an entropy refusal faults the
  supplicant like a failed handshake and retains its state.

Acceptance stores the 32-byte PMK in the configuration and the PMKID beside
it. `aic8800_supplicant_sae_clear` frees the exchange state and wipes the
password. A cleared supplicant cannot start again, because start refuses a
configuration without a password. A retry therefore initializes a new
supplicant with the password.

The exchange does not retransmit lost frames. Receive checks the deadline only
when a frame arrives, so a lost exchange holds its state and allocations until
the caller acts. When the deadline passes without a frame, the caller must call
`aic8800_supplicant_sae_clear`.

### Missing parts before association

- **External authentication.** The pinned full-MAC driver refuses SAE unless
  the host performs authentication. The firmware reports
  `SM_EXTERNAL_AUTH_REQUIRED_IND` with the BSSID, SSID and AKM suite. The host
  then exchanges the commit and confirm frames through management-frame
  transmission and reception, and answers with `SM_EXTERNAL_AUTH_REQUIRED_RSP`
  and a status. The native data worker sends only Ethernet data descriptors and
  leaves received management frames opaque, so neither direction exists.
- **Protected management frames.** The association must set the source's
  `MFP_IN_USE` connection flag and advertise PMF capability in its RSN
  element, and the firmware must report `MM_FEAT_MFP_BIT` (bit 13). The
  supplicant must also install the integrity group key for BIP-CMAC-128. The
  security worker installs only CCMP pairwise and group keys. WPA3-only access
  points require PMF, and certified transition-mode access points require it
  for SAE associations.
- **SAE association.** The association request carries a fixed PSK RSN
  element, and claim refuses an SAE configuration. The four-way handshake
  with the SAE suite and the SAE PMK and PMKID has no host check yet.
- **Board profile.** The board radio profile has no security-mode field and
  carries a PMK, not a password. Adding one changes the profile layout that
  `apps/board/prepare.py` writes.

Physical qualification also needs a WPA3 access point.

## OpenSSL supplies the elliptic-curve arithmetic

The pinned wpa_supplicant release implements the `crypto_ec_*` and
`crypto_bignum_*` interfaces that SAE uses only in its OpenSSL and wolfSSL back
ends. SAE images therefore build `crypto_openssl.c` against a minimal static
`libcrypto` from OpenSSL 3.5.9, a long-term-support release. In those images
OpenSSL performs every RSN primitive, including the WPA2 hashes, HMAC, AES key
wrapping and CMAC; the internal hash and AES files are removed.

[cmake/openssl.cmake](../cmake/openssl.cmake) records the library
configuration:

- **Excluded features.** The build has no operating-system seed source,
  threads, sockets, files, console, TLS or engines, and it omits the
  algorithms that WPA2 and SAE do not use: DES, MD4, RC4, DSA, DH, the
  Edwards and Montgomery curves, post-quantum signatures and the rest.
- **Fetch cache.** The fetch cache is disabled. Each fetch then rebuilds its
  algorithm methods, which keeps about 65 KB less on the heap.
- **Build.** CMake configures and builds the library once per build directory,
  while configuring, with the image's compiler, `-Os` and the freestanding C
  headers. A target that copies a radio target's sources and link options,
  such as the dispatch model, therefore links the same library. The archive
  hash, the options, the target definition, the compiler and the flags form
  the build identity in `<build>/openssl/build-identity.txt`; a change
  rebuilds the library.

[aic8800_openssl.c](../drivers/net/aic8800_openssl.c) sets the library up on
the first supplicant OS entry. Every OpenSSL allocation comes from the FreeRTOS
heap with a size prefix and is wiped when freed. A built-in provider supplies
the seed and nonce of OpenSSL's random generator (a CTR-DRBG with AES-256), and
each of its requests calls the entered supplicant's entropy callback through
`os_get_random`. Outside a supplicant entry, or when the callback refuses,
every seed request fails and the entered supplicant faults. A refusal does not
latch: a later supplicant can seed again. The generator reseeds after a fixed
number of requests. There is no wall clock, so it never reseeds by age.

The library is built without threads. All calls come from the radio owner task
under the supplicant's single OS entry.

Native images have no C library.
[aic8800_openssl_libc.c](../drivers/net/aic8800_openssl_libc.c) defines the
functions that OpenSSL and `sae.c` reference. It has no file system,
environment, user identity or wall clock, and its `malloc` refuses so that a
library use before setup fails. Its string, conversion and sorting functions
serve OpenSSL code that WPA2 and SAE do not call: encoders, PEM input and
configuration loading.

Two constraints apply to a board image that adds SAE:

- **Floating-point and SIMD registers.** The recipe sets the library's
  code-generation flags itself and does not take them from the image. The
  network model allows floating-point and SIMD code, and the library as built
  uses those registers. Board images compile C with `-mgeneral-regs-only`, so
  the board composition must either allow OpenSSL to use these registers or
  build the library with that flag.
- **C library.** The C library file defines `malloc`, `free`, `abort`,
  `__errno`, `_ctype_`, `strtoul`, `qsort` and other C library functions. In
  an image that links newlib through `runtime/newlib.c`, these definitions
  collide with newlib or silently replace it. The board composition must
  resolve each overlap.

### Footprint

In the AArch64 network model with the same compiler, linking OpenSSL instead
of the internal crypto files adds 991,775 bytes of code and read-only data,
2,240 bytes of data and 6,736 bytes of zeroed data. Almost all of the code is
the OpenSSL default provider. The SAE exchange, together with the model's own
test exchange, adds 37,553 bytes of code more.

In the AArch64 network model under QEMU, OpenSSL keeps 80,491 bytes on the
FreeRTOS heap after one hunting-and-pecking and one H2E exchange. It peaks at
130,422 bytes while setting up its random generator and first algorithms. The
host check runs other sequences of exchanges, so its heap figures differ. The
network configuration's heap holds 256 KB and is shared with FreeRTOS+TCP, so
an image that adds SAE must account for this demand.

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

For SAE, fetch the pinned OpenSSL archive with `tools/fetch-sources.py openssl`,
set `SUNSTONE_OPENSSL_DIR` to the extracted tree, include
`cmake/openssl.cmake` and call
`sunstone_enable_radio_sae(target /absolute/path/to/wpa_supplicant-2.12)` after
`sunstone_enable_radio_security`. The helper verifies every file of the OpenSSL
tree against its archive, builds the library, replaces the internal crypto
files with `crypto_openssl.c` and the SAE sources, adds the SAE, OpenSSL and C
library adapters, and defines `SUNSTONE_AIC8800_SAE`. The OpenSSL archive is
[openssl-3.5.9.tar.gz](https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz),
SHA-256 `603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a`,
under the Apache License 2.0; distribution must preserve its `LICENSE.txt`.
The configuration option `SUNSTONE_RADIO_SAE` applies the helper to the
network model. No board image calls it yet.

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
executable or proof of radio operation. With `--openssl`, the host recipe runs
the same WPA2 cases with `crypto_openssl.c` and the minimal OpenSSL library.
Before each case it also checks that claim refuses an SAE configuration with
the owner, time and beacon evidence that admit the PSK supplicant.

`checks/sae-openssl.py` builds the minimal OpenSSL library for the host from the
recipe's options and runs the station exchange under the same sanitizers
against a second `sae.c` instance acting as the access point, in the order
hostapd uses. Passing cases cover:

- the commit, key confirmation key, PMK, PMKID and group-19 H2E password
  element of IEEE Std 802.11-2020 Annex J.10;
- accepted hunting-and-pecking and H2E exchanges with equal PMK and PMKID;
- anti-clogging tokens with and without the token container;
- acceptance in group 20 after the access point refuses group 19, and in
  group 21 after it refuses groups 19 and 20;
- refusal after every group is refused or after five token requests;
- refusal of an altered confirm, a wrong password, a confirm before a commit,
  a rejection status, a zero status on an H2E commit and a late frame;
- discard of a reflected commit;
- refusal of a second start after `aic8800_supplicant_sae_clear` ends an
  unfinished exchange;
- refusal of an SAE start for a PSK configuration and of a supplied PMK;
- an entropy refusal before the generator is seeded, followed by a successful
  exchange with a new supplicant.

With address randomization disabled, two runs with the same entropy stream
produce the same commit and a different stream produces a different one. No
time value therefore reaches the random values; OpenSSL mixes only object
addresses into its generator as additional input.

`checks/radio-network.py --openssl <tree> --case sae` builds the AArch64
network model with SAE and runs one hunting-and-pecking and one H2E exchange
in a FreeRTOS task under QEMU, with OpenSSL on the FreeRTOS heap. The entropy
of these checks is a synthetic model input.

Physical work still requires qualified board recovery, clock/pin/power ownership,
a real entropy provider, and admitted radio transmission. SAE additionally needs
the external-authentication frame path, PMF and an SAE association before a WPA3
access point can test it. A board image that adds SAE must also decide how
OpenSSL is compiled with respect to floating-point and SIMD registers and must
resolve the C library overlap with newlib. Confirm firmware key
replacement/deletion, controlled-port behavior, EAPOL status and CCMP integrity
before qualifying secure data. IP networking, credential lifecycle, the excluded
security modes, broader receive profiles and Bluetooth remain separate required
parts of full board support.
