# Native Bluetooth identification and headset capture

The supervised Bluetooth image connects the onboard radio firmware bootstrap,
native UART1 and existing H4/HCI worker. It identifies the controller through
six actual command completions and returns copied results through USB0.
Physical operation remains unqualified. This document assumes familiarity with
FreeRTOS ownership and the existing radio and Bluetooth workers.

The separate `sunstone-a733-bluetooth` target uses the USB0-only board startup
and its existing shared PMIC, clock registry, pins, rails and register mutex.
One additional privileged task owns radio startup and UART/HCI polling.
The board's 592-byte workload ABI is unchanged. This image uses workload 2;
the session's SID-evidence field authenticates the complete Bluetooth profile.
That profile contains the actual secure SID evidence as a separate digest.

The immutable 624-byte profile resides at `0x43000000`. Its four pinned firmware
spans lie within `0x43010000..0x43100000`, separately admitted and retained DDR.
The native firmware engine validates the existing artifact identities and
uploads Bluetooth ADID, patch code and patch table before starting the radio
application. The Bluetooth-only session then services the actual runtime
binding and dispatcher. It initializes no Wi-Fi management, WPA2, CE entropy
or IP stack. Existing Wi-Fi session initialization retains its default behavior.

The EL3 publication hook authenticates the profile before reading exactly four
SID chip-ID words at `0x03006200..0x0300620c`. It requires the explicit secure-read
authority in the admitted session and profile. It publishes the copied identity,
profile and firmware reservation before EL1 startup. No new secure-monitor ABI
or EL1 SID access is supplied.

UART1 uses PG6–PG9 mux 2, its actual 24 MHz source and exactly 1,500,000 baud,
eight data bits, no parity, one stop bit and automatic RTS/CTS. Its native
platform owns the controller and masked INTID 35. UART setup precedes radio
enable and firmware startup. External fresh-transport evidence must establish
the quiescent peer, absent stale bytes and exclusion of earlier HCI owners;
empty FIFO observations alone cannot prove those conditions.

The existing radio platform owns PM3 host wake and PM4 host-wake observation
alongside its Wi-Fi enable/wake pins. PM3 remains high during this bounded
workload; PM4 is input and its sampled level grants no readiness. The pinned
board device tree declares both signals active high. Its AIC low-power source
selects those polarities, requests host wake as input and drives external wake
to its asserted level. Schematic V1.2 sheet 14 connects the corresponding
`AP_WAKE_BT` and `BT_WAKE_AP` nets to the module wake signals.

PM2 remains unchanged. Its reset/LPO wiring ambiguity requires installed-board
evidence before admission. The complete PM bank latch record must preserve that
pin and exclude other DATA writers. This implementation supplies no second
Bluetooth module reset or power writer and creates no suspend/resume protocol.

The H4 binding uses the actual radio borrow generation and UART clock-token
generation. An owner query authenticates firmware start confirmation, completed
Bluetooth patching, the exact runtime/dispatcher graph and any required binding
response. The owner preflights both actual live guards before each bounded HCI
step. Resource contention pauses the iteration. The short binding callback
checks the same immutable generations, owner and irreversible shared faults.
Neither a generation number nor configured baud fabricates H4 readiness.

Only successful Reset, Version, Supported Commands, Features, Buffer Size and
BD_ADDR completions publish IDENTIFIED. Unsolicited records are consumed within
the existing bounded packet storage. Identification establishes no pairing,
authenticated peer, connection, audio path or controller security readiness.
The optional headset capture path below adds security, hands-free control and
synchronous PCM capture. Physical headset operation and broader Bluetooth modes
remain unqualified.

UART startup has a two-second software deadline. Radio bootstrap is bounded by
40 seconds, HCI identification by 55 seconds and the complete supervised session
by 60 seconds. Command and partial-frame deadlines remain independently fixed.
Failure first closes the shared revocation latch, then retains H4, UART and
radio metadata. Later owner iterations perform no healthy device work.
USB terminal errors also revoke the application before parking.

These software deadlines cannot recover a stalled bus or CPU. The supervised
launcher writes no watchdog or reset redirect. Every hardware resource remains
reserved until confirmed complete-board power removal. A later launch requires
a new cold lifetime; USB reconnection alone cannot authorize reuse.

USB0 uses the existing permitted product ID `0xefe8` with distinct Bluetooth
product and serial strings. A request containing one zero-valued byte returns an 80-byte header with
session/profile identity and pending or retained-failure status. IDENTIFIED adds
97 bytes containing the actual generations, validity mask and copied controller
identity fields. The offline decoder rejects mismatched identities and partial
results. It performs no device access.

The CMake target emits the linked ELF manifest required by offline preparation.
The preparer reuses existing supervised SRAM entry and board admission checks,
then validates the exact four firmware spans and frozen Bluetooth wiring,
secure-SID and fresh-transport evidence. Preparation grants no physical
authority. Retained DDR, mapping/exclusion, topology, pin and manual-recovery
records must describe the actual coordinator-approved board lifetime.

Selected validation covers the new application with the actual UART/H4/HCI
workers over modeled native providers, explicit existing firmware checkpoints,
bootstrap-only session readiness, actual shared PM3/PM4 pin acquisition and
the actual copied result serializer. Existing UART, firmware and HCI protocol
suites are reused. The full native image link covers real providers separately;
the models establish no physical controller operation.

The reviewed native image links with the actual shared providers. The focused
application, bootstrap session, wake-pin and UART deadline cases pass with
AddressSanitizer and UndefinedBehaviorSanitizer. The offline admission/result
mutations and the ARM SRAM launcher compile/link also pass. The final ELF SHA256
is `93a51a0e8b2d5003b1e1709a23706bb90fda6e0fb58a73a797859bc662cf21bc`.
Exact source, dependency, tool and generated-artifact inputs are retained under
`~/.cache/sunstoneos/evidence/bluetooth-native-20261017`.

The first wake fixture omitted the required clock lease before entering its
isolated pin-acquisition checkpoint. Its failure and prior artifact are
preserved. The corrected baseline acquires that lease through the actual
platform ACQUIRE step and checks owner, token health, deadline and pin-provider
initialization before entering PINS. An audit of the remaining fixtures found
no other omitted entry prerequisites: the UART case uses actual initialization
through READY, the session case uses actual bootstrap initialization with
explicit modeled lower-worker completion, and the application case starts at
its actual public initializer. Completed cases were reused; only the affected
wake fixture was rebuilt and resumed. These host results establish no physical
firmware upload or controller identification.

The bounded host adapter uses the shared supervised session runner and the pure
Bluetooth bundle validator and result decoder. It sends one zero-valued byte
on USB OUT and requires one transferred byte before reading at most 177 bytes.
For the identification profile, only a valid pending reply permits another
query. A terminal identity or fault ends capture; malformed, late or failed transfers stop immediately. The copied
frame becomes durable before its terminal result is published. Queue review
binds the initially frozen bundle, executable image, launcher and host source
identities. The board lock and queue claim remain retained for coordinator
review and complete-board power removal.

Native input preservation follows the actual linked-object list, compiler
records and assembly dependency records. Recovery from the retained Bluetooth
build records 165 linked objects and 420 input dependencies, including five
assembly sources. Missing or empty coverage is refused. The original successful
checks are unchanged; the recovered bytes and records are stored in the
`dependency-recovery` directory alongside their original evidence.

The focused host batch passes 17 cases using the retained C-produced identity,
profile, native image and ARM launcher. It covers bounded pending capture,
terminal faults, malformed or stale identities, expired transfers, durable-save
failure, queue/source identity changes, frozen input copies and shared completion
routing. The new Bluetooth launcher wrapper also passes its session, SID,
workload and absent-watchdog checks with the unchanged supervisor explicitly
modeled. No physical transport or earlier protocol suite is replayed. Inputs
are copied and hashed before compilation and execution; raw results reside in
`bluetooth-native-20261017/host-capture` under the evidence cache.


## Bounded native headset capture

Building with `SUNSTONE_BT_SECURITY_DIR` enables the 752-byte headset profile.
The default 624-byte identification profile remains supported. Both profiles
retain the same board workload ABI, four firmware spans and full-power-removal
recovery requirement. The headset profile adds one permitted peer, explicit
pairing consent or PIN, minimum security, RFCOMM channel, capture sample count,
receive-service authority and actual peer discovery metadata. Outgoing page-scan
mode, role-switch policy and clock offset come from that immutable input.

The security provider compiles the pinned Zephyr 4.2.1 SSP and RAM link-key
sources against the existing serialized HCI owner. The pin and per-file hashes
are in `apps/bluetooth/security-source.json`. The adapter preserves original
source license notices and the Apache license. It removes platform includes and
logging registration, supplies fixed copied command buffers and connection
lookups, and reuses the original security-level decision body. No Zephyr runtime
or second Bluetooth transport runs on the board. Keys remain in RAM until full
power removal; replacement pairing and authenticated-key replacement retain a
failure before changing existing proof.

A command queue copies parameters from upstream callbacks. Only matching tagged
controller completions retire entries. Pending security acknowledgements block
replacement commands from ACL and synchronous-audio owners. SSP consent alone
never opens a service. Actual authentication, encryption mode and a successful
16-byte encryption-key-size response must all refer to the same connected peer,
ACL generation and controller lifetime. This port advertises no input or display;
its Just Works policy also governs upstream initiator auto-confirmation.

One task drives the existing local SDP server, RFCOMM registry, hands-free audio
gateway and synchronous-audio provider. Incoming mode listens for the admitted
peer. Outgoing mode discovers the peer's hands-free service and closes the SDP
client while continuing its close work beside RFCOMM. The retained bootstrap
initializer claims L2CAP once without discarding partial H4 framing or staged
bytes. Its ordinary strict initializer retains its existing empty-input rule.
Every service admission checks actual security again.

The gateway advertises no telephony service or active call. After hands-free
setup, it opens CVSD synchronous audio and collects signed 16-bit little-endian
PCM at 8 kHz. Packet boundaries may split a sample. The application copies bytes
across boundaries and preserves each packet's acquisition serial, source length,
copied length, offset and loss status. It sends explicit zero-valued playback
samples to service the synchronous link. Capture is bounded by the immutable
sample count, buffer capacity, packet-record capacity and absolute deadlines.
Any loss prevents successful publication. A complete capture becomes readable
only after the controller confirms synchronous disconnect.

Headset requests contain eight bytes: kind 1 for PCM or 2 for packet records,
a zero reserved byte, a little-endian 16-bit requested count and a little-endian
32-bit byte offset. Each request copies at most 1024 bytes. Replies contain a
160-byte header followed by the copied bytes. Pending and failed replies carry
no producer metadata. Successful headers identify the session, profile,
firmware, UART, ACL and audio generations, complete extents and PCM SHA256.
Packet records use the 24-byte little-endian layout declared in `headset.h`.
The one-byte identification request retains its original reply format.

The host saves each raw frame before decoding. It checks immutable identity
across chunks, packet extents and loss, the complete PCM digest and the capture
deadline before publishing success. The saved `bluetooth-8k-s16le.pcm` is copied
audio output. The finite recognition extension below converts this capture to the existing
Mimus input format. Continuous input and physical headset-to-recognition
qualification remain outstanding.

The affected native ELF links with the pinned security sources. Eight focused
composition cases pass with AddressSanitizer and UndefinedBehaviorSanitizer:
complete incoming authenticated control/audio capture, initiator consent refusal,
authenticated key replacement refusal, operation expiry, PCM loss refusal,
retained incomplete H4 handoff, pre-authentication service refusal and
pending copied results before the owner starts. The model
starts through actual application initialization and uses production protocol
workers. Radio startup and UART register behavior are modeled. The host cases
cover profile admission, copied frame identity, pending privacy, bounded capture,
full digest refusal, identity changes and late finalization. Their frames are
constructed in Python; native USB serialization has source review and native
link coverage. Host source and result preservation completed after execution
following failed pre-execution snapshot setup. That snapshot cannot prove
pre-execution input identity. Outgoing connection/discovery composition remains untested. No physical headset
or Bluetooth operation is established.

The retained headset ELF SHA256 is
`d082652408a0463ed32385cf0560e5ea3f02d5babbaf9bfae088aad89ad4e4db`.
Its dependency record covers 177 linked objects and 446 inputs. Inputs, executable
snapshots, compiler dependencies and raw results are under
`~/.cache/sunstoneos/evidence/bluetooth-headset-native-20261017`. The native image
and host adapter require a coordinator-reviewed board lifetime, actual wiring,
secure SID and transport evidence, scheduling/buffer/RTS-CTS evidence, and one
identified headset before a physical capture request can run. No headset is
currently confirmed. Physical pairing, control, audio capture and recognition
qualification remain required.


## Finite headset transcription with unchanged Mimus

`SUNSTONE_BLUETOOTH_MIMUS=ON` adds one finite transcription to the headset image.
The 832-byte profile includes the existing headset policy and the exact base
and punctuation model lengths and SHA256 hashes. The model pin is
`apps/bluetooth/recognition-models.json`; tensor bodies and recognition arithmetic
remain unchanged. The 624-byte identification and 752-byte capture profiles
retain their original behavior.

The fixed memory layout separates models from radio firmware and profiles:

| Range | Use |
| --- | --- |
| `0x43000000..0x43100000` | Existing Bluetooth profile and four firmware assets |
| `0x44000000..0x44800000` | Base model, at most 8 MiB |
| `0x44800000..0x45000000` | Punctuation overlay, at most 8 MiB |
| `0x46000000..0x47000000` | Existing 16 MiB recognition arena |

Preparation requires exclusive DDR evidence for both model slots and the arena,
with cache publication and remote-writer exclusions. It freezes exact admitted
model bytes into the upload bundle and revalidates their hashes, sizes and
addresses. Native startup checks the mapped ranges; the recognition task hashes
both models in bounded chunks before loading them. Native USB exposes copied
results and cannot replace model assets during this lifetime.

The Bluetooth task retains priority 4. A priority-3 recognition task waits for
the completed copied capture. It rejects failure, packet loss, changing capture
identity and a mismatching complete PCM digest. Only a complete capture whose
synchronous link has actually disconnected can enter conversion. The task owns
static 48,000-byte PCM, 96,044-byte WAV, 97,280-byte internal scratch and
65,536-byte text buffers through the entire attempt.

Conversion produces 16 kHz mono signed 16-bit little-endian WAV data. Each
8 kHz sample emits itself and the average of itself and the next sample. The
sum uses signed 32-bit arithmetic; division rounds toward zero, including
negative odd sums. The last sample is held for the final interpolated value.
An input of N samples produces 2N samples and retains the same duration.
This linear interpolation has limited image suppression and cannot restore
frequency content absent from the narrowband headset input. Matching converted
input can establish software parity; original 16 kHz recordings are a different
input condition.

The existing recorded engine is shared through `apps/mimus/recorded.h`.
Its model loader, recognizer, WAV parser, metrics and punctuation/formatting
sequence are preserved. The existing recorded caller passes its original
scratch and the default null stop pointer. Capture integrations may pass a stop
flag to the recognizer's existing cancellation checkpoints. The shared function
checks cancellation around finite load, create, parse, recognition and formatting
stages. For a finite capture, it returns after all temporary engine and model
objects are destroyed. In the repeated profile, the recognition task loads the
model and recognizer once and keeps them in static storage. Every later segment
reuses them until the session ends. Both profiles skip the per-tensor model CRC
pass, because the recognition task has already checked the SHA256 of both models.

The image reuses SMP newlib task reentrancy, locks and the existing 4 MiB heap
and initializer-array linker patch. Runtime initialization completes before the
scheduler starts. Only the recognition task appends to its private text buffer;
USB cannot read that buffer until final release publication. The Bluetooth and
USB tasks do not allocate through this C++ heap. Their actual receive-service
bound during recognition still requires physical scheduling evidence.

The unchanged supervised session lasts 60 seconds. The transcription profile
bounds firmware bootstrap at 30 seconds, HCI identification at 35 seconds and
capture plus confirmed synchronous disconnect at 40 seconds. The application
ends at 59.99 seconds. This reserves less than 20 seconds after the application
margin for model loading, recognition and output; host transfer and publication
also consume that interval. These are failure bounds, not measured A55 execution
guarantees. No physical result establishes that the complete attempt fits.

Shared revocation stops publication immediately. The model loader has no
mid-load cancellation hook, so finite load work may continue until its next
stage check. Recognition uses its existing stop checkpoints. Failed or expired
attempts retain all inputs, heap allocations and protocol ownership while work
unwinds. Neither a failed result nor a parked task authorizes resource reuse;
complete-board power removal remains required.

An eight-byte request with kind 3, zero reserved byte, little-endian count and
byte offset copies at most 1024 transcript bytes. A successful response has a
208-byte header identifying the original session and controller/link generations,
input/output sample counts and PCM, converted-WAV and text digests. Pending and
failed replies publish no producer metadata. The host saves raw frames, verifies
capture identity and complete text digest, checks the deadline and then saves
the formatted transcription. No keyboard events are generated.

The native image links the actual Bluetooth stack, converter, shared recorded
engine, model library, C++ runtime and USB adapter. The focused software batch
passes one new modeled security/control/SCO capture containing 24,000 samples,
then executes the actual converter and unchanged portable-host recognition and
formatter on its copied bytes. Features, log probabilities, frames, words,
punctuation, capitals and formatted text match a host reference using the same
converted WAV. Failed capture, loss, model hash, expiry and mid-recognition
revocation prevent successful copied publication. The converter checks signed
rounding, endpoints, mono/rate declarations and its exact 96,044-byte maximum.

The original supervised recorded caller also compiles and links with unchanged
frozen objects and its default null stop pointer. Its recordings were not replayed.
The first host reference used forced final punctuation instead of the recorded
caller's automatic mode; that oracle failure is preserved. Only the corrected
reference/parity step was resumed. Model/range/schema and copied-result host
checks also pass. Those USB frames are constructed from the real recognition
snapshot; native USB serialization has source review and native link coverage.
Scheduler, registers, peer radio behavior and DDR evidence remain modeled.

The final ELF SHA256 is
`3f469086547f200d33a2eba2dd34def1ba3d8975847aa9e0a78874c11abdde53`.
The final dependency snapshot covers 185 executable objects, 33 ASR archive
objects and 756 source/header inputs. Compilation records confirm that C startup
omits the legacy packed-asset Mimus macro while C++ retains native timing.
Exact inputs, executable snapshots, compiler commands, dependencies and raw
results are under `~/.cache/sunstoneos/evidence/bluetooth-mimus-native-20261017`.
The software checks establish the finite integration and matching converted-input
reference behavior. An identified headset, actual pairing/control/audio operation,
concurrent receive-service bounds and physical transcription within the supervised
window remain unqualified. This profile captures one finite segment. The repeated profile below accepts successive segments within the same supervised session.

The recognition-enabled startup image is now preserved under
`board-session-seal-mimus-20261018/`, with ELF SHA256
`7e01cd78e899a5a45fa9074fc8f188bd835c87a53615b444d7f69128b1fe6d0b`.
It retains the authenticated session and profile before provider initialization.
Earlier integration reconciled its 210 compiled repository inputs. The capture,
conversion and recognition evidence remains applicable.

The [native USB check](../checks/bluetooth-usb-native.py) executes that frozen
image's recognition copy, serializer, USB task and TinyUSB device stack without
rebuilding it. Five cases pass: fragmented output with a queued repeated request,
malformed-request refusal, provider failure, deadline expiry and recognition stop
during copying. Two identical 1,232-byte responses survive withheld endpoint
completion without changing their backing bytes. The remaining transcript chunk
also transfers, reconstructing all 1,212 bytes of the earlier recognition log.
The production decoder accepts the copied identities, sample counts and digest.
Pending and stopped replies expose no producer metadata.

Inputs and results reside under `bluetooth-usb-native-20261018/` in the evidence
cache. The check adopts the earlier real transcript and snapshot into their final
native fields. It models ready provider/application state, clock/rail/PMIC status,
controller transfers/events, scheduler yields and monotonic time. It executes no
radio initialization, recognition or physical USB transaction. The first attempt
omitted the recognizer's separate monotonic-time source; its failure is preserved.
The repaired fixture uses one time value for both application paths. These results
close the recognition serializer/TinyUSB software gap. Physical headset operation,
receive-service bounds and complete transcription within the supervised window
remain unqualified.


## Repeated headset capture and recognition

The recognition profile also accepts successive 8 kHz, signed little-endian
16-bit mono PCM segments from one authenticated CVSD synchronous link. Set the
headset mode at byte 624 of the existing 832-byte profile to 2. Mode 1 retains
its finite capture protocol. The controller performs CVSD coding; this profile
adds no host codec and does not negotiate wideband mSBC.

The Bluetooth task owns transport service and two copied PCM slots. Each slot
holds at most 24,000 samples and 1,024 packet records. A packet that crosses a
segment boundary retains its acquired-record identity and byte offset until its
remaining bytes enter the next slot. The task releases the HCI record only after
its final byte is copied, or explicitly abandons the remainder while closing.
Complete, loss-free segments carry an increasing ticket, absolute sample offset,
transport identities and SHA256 digest. Release/acquire publication prevents a
consumer from reading a mutable slot.

The recognizer copies one complete segment into its private PCM buffer, checks
its identity and digest, then acknowledges that exact input ticket. Only that
acknowledgment permits the Bluetooth task to reuse the slot. The existing
8-to-16 kHz converter and recorded recognition engine process the private copy.
One transcript log remains immutable until its entire byte range completes USB
transfer and the host acknowledges the current ticket. Queueing USB bytes or
reading a partial log provides no acknowledgment credit. Recognition waits for
that acknowledgment before processing the next segment.

The profile retains the session's overall deadline. New capture admission ends
38 seconds after application start, and the matching synchronous disconnect must
arrive by 40 seconds. These are application budgets within the existing roughly
60-second session; they are not evidence of a transport drain. Recognition and
copied output may continue after capture stops, subject to the session deadline.

A full copied-input queue, packet loss or a capture deadline stops admission and
starts the existing bounded close sequence. Accepted complete segments and their
transcripts remain available after an orderly close. The application waits for
accepted H4 output to complete locally before requesting synchronous disconnect,
then requires its matching disconnect event. An unknown or late result retains
failure. Security revocation denies further input and transcript publication.
The Bluetooth task continues security/control service after audio disconnect.
No condition silently overwrites accepted input or reports a guessed disconnect.

### Repeated USB requests and results

Requests are 16 little-endian bytes. Byte 0 is `0x80`; byte 1 selects status (0),
read (1), acknowledge (2), or capture cancellation (3). Bytes 2–3 hold a read
length of at most 1,024 bytes, bytes 4–7 the byte offset, and bytes 8–15 the
64-bit transcript ticket. Read and acknowledge require a nonzero ticket.
Acknowledgment requires zero offset and length. Status and cancellation require
all remaining fields to be zero. The existing one-byte identity query remains
available. Cancellation may truncate audio and is not a graceful wire drain.

Responses contain a 272-byte header and up to 1,024 transcript bytes. All fields
are little-endian. Reserved and unpublished metadata bytes are zero.

| Offset | Field |
| --- | --- |
| 0, 4 | 32-bit magic `0x53525442`, version 1 |
| 8, 12 | 32-bit state (pending 0, result 1, finished 2, failure 3), payload bytes |
| 16, 24, 32 | 64-bit session epoch and generation, 32-byte profile SHA256 |
| 64–95 | Four 64-bit firmware, UART, ACL and audio identities |
| 96, 104 | 64-bit transcript ticket and first input sample |
| 112, 116, 120, 124 | 32-bit text bytes, input samples, output samples, reserved zero |
| 128, 160, 192 | 32-byte PCM, converted WAV and transcript SHA256 digests |
| 224, 232 | 64-bit published and consumed input counts |
| 240, 244 | 32-bit stop flags and confirmed-disconnect flag |
| 248, 256 | 64-bit first and last acquired HCI record serials |
| 264, 268 | 32-bit recognition-finished flag and returned byte offset |

Stop flags are cancellation (1), admission window (2), backlog (4), loss (8),
and deadline (16). A failure response may still carry an accepted transcript
when a capture stop flag is set. Authorization failure suppresses its metadata
and body. Finished means that disconnect is confirmed and all accepted inputs
have passed through recognition and their transcript acknowledgments.
The host stores each complete transcript and its digest before acknowledging it.
It requires matching session/profile identities, consecutive tickets and sample
offsets, confirmed disconnect, and equal accepted/consumed/acknowledged counts.

### Software evidence and remaining qualification

Evidence is preserved under
`~/.cache/sunstoneos/evidence/bluetooth-repeated-recognition-20261019/`.
Six actual-stack ownership/error cases pass. A separate stack composition copies
two distinct speech segments across HCI record boundaries. The unchanged
recognizer processes those copied segments, and both results match the portable
host reference for features, log probabilities, frames and formatted text.

The actual native USB task, serializer, result APIs and TinyUSB pass delivery,
early/stale acknowledgment, revocation and publication-order checks. The host
uses their produced frames to check two-result collection, backlog, durable-store
failure, wrong identity, sample gaps, missing disconnect and a 512-byte first
USB packet. Scheduler progress, device registers, radio/controller events and
USB device effects are modeled. No hardware transaction or performance
measurement was performed. Physical headset operation, receive-service bounds,
recognition within the session deadline and wideband mSBC remain unqualified.


## Wideband capture through the existing recognizer

An optional image profile negotiates mSBC and delivers decoded 16 kHz PCM to the
same repeated recognition path. Enable `SUNSTONE_BLUETOOTH_MSBC=ON` and set
`SUNSTONE_BT_SBC_DIR` to the pinned BlueZ SBC source directory. The default
CVSD build omits this dependency. Set headset mode 3 in the existing 832-byte
profile; its segment length must be a multiple of 120 and at most 24,000 samples.
The existing two 48,000-byte copied slots therefore hold at most 1.5 seconds of
wideband PCM each. The session and transcript acknowledgment budgets remain the
same as those for repeated CVSD capture.

The image compiles the generic C encoder and decoder from
[BlueZ libsbc 2.0](https://kernel.googlesource.com/pub/scm/bluetooth/sbc/+/8dc5d5ba381512ad5b1afa45c63ec6b0a3833244/),
using its `SBC_HIGH_PRECISION` configuration. The CPU executes this code;
controller firmware does not perform the host decode. The source manifest
[msbc-source.json](../apps/bluetooth/msbc-source.json) pins each file and its
LGPL-2.1-or-later license. The evidence freeze retains the original notices,
license, source, object files and link inputs needed to relink the application.
No codec source is copied into the repository.

Codec initialization follows `runtime_smp_init()` and `runtime_init()` before
the scheduler starts. The existing runtime returns its global reentrancy state
and bypasses mutex acquisition during this phase. The codec allocates one encoder
context and one decoder context, then retains both for the session. No frame
processing allocates memory. If the second allocation fails, initialization
releases the first and denies startup. The Bluetooth owner alone subsequently
uses both contexts and the bounded framing buffers.

### Negotiation and controller admission

Wideband advertisement requires the reported local eSCO and transparent-data
features. The AG advertises codec negotiation in BRSF bit 9 and wideband in SDP
bit 5. After the HF advertises negotiation support and supplies BAC, the AG
selects only a codec in that list. A matching BCS command and complete local
acceptance of its OK response precede synchronous setup. HFP lets the HF send
BAC again at any time. A new list cancels an outstanding offer. It keeps the
selected codec when the list still contains that codec and clears the selection
otherwise, so the AG must select again before the next audio setup. An open audio
link keeps the codec bound when it opened; only a different nonzero selection
faults it. Invalid or missing BCS cannot establish audio.

The existing HCI owner queries remote features before setup. Transparent mSBC
requires matching local and remote eSCO/transparent support. Enhanced setup and
accept use transparent input/output and air formats, 8,000 octets/s in both
directions, HCI data paths zero and transport-unit sizes zero. These parameters
follow Core 5.4 §7.1.45–46. The cached Linux comparison uses transport-unit size
one; the native path follows the Core requirement for HCI.

T2 uses 2-EV3, 13 ms maximum latency and retransmission effort 2. T1 uses EV3,
8 ms and effort 2. T2 is preferred when supported; T1 is the safe non-Secure
Connections setting. Secure Connections requires T2. A matching terminal setup
failure may lead to fresh CVSD negotiation and a new audio generation. A known
remote-feature refusal before setup can also permit that fallback. Command
refusal, uncertain timeout or an established handle cannot use this retry path.
The host records fallback and cannot report it as successful wideband operation.

The pinned AIC8800D80 bootstrap configures coant mode 5, UART port 2, 1.5 Mbaud,
flow control and low-power support. Those published settings establish the chosen
HCI mode. Neither those sources nor the opaque pinned firmware establish
transparent eSCO support. Actual reported capabilities, matched command/event
results and physical qualification remain necessary; no vendor routing command
is guessed.

### Framing, copied ownership and results

The Bluetooth owner carries transparent bytes across HCI packet boundaries.
A 60-byte transport frame contains the H2 header, a 57-byte mSBC frame and its
padding byte. The decoder requires the next modulo-four H2 sequence and emits
120 signed little-endian samples per frame. Initial alignment search is bounded
to 120 discarded bytes before the first candidate. A rejected codec candidate,
CRC error, unexpected sequence, reported HCI loss or later framing error stops
capture without publishing its partial PCM segment. The implementation does not
reuse decoder state after a failed candidate or conceal missing samples.

H2's two-bit sequence alone cannot rule out every multiple-of-four frame loss.
Controller packet-status reporting and the admitted receive-service bounds
remain part of physical qualification. Acquired record serials identify host
transport records; they are not radio timestamps.

A complete decoded segment carries the negotiated codec in its copied capture
identity. The recognizer verifies that identity on every chunk before input ACK.
For mSBC, the WAV wrapper copies the 16 kHz samples directly; for explicit CVSD
fallback, the existing 8-to-16 kHz converter applies. The model and recorded
recognition engine remain unchanged. Full-ring backpressure stops before a raw
byte can overwrite accepted PCM. Accepted slots and delivery-owned transcripts
survive an orderly capture stop. Security revocation denies further publication.

In capture-only mode 3, the reverse direction sends valid encoded silence with its own H2
sequence. Its 60-byte backing remains stable until all fragments have entered
SCO-owned copied packets. Controller credits and actual local H4 completion
remain independent of this application buffer. Cancellation may truncate audio;
no serial-wire drain or peer rendering is inferred.

Every synchronous transmit stream keeps a fixed period: 7.5 ms per 60-byte mSBC
frame, for silence and caller playback, and 125 µs per CVSD sample for silence
packets. Each send time is the previous scheduled send time plus one period. The
owner task polls on the 1 ms FreeRTOS tick, so computing the next time from the
serving poll would round 7.5 ms up to 8 ms and send 6% fewer frames than the eSCO
link consumes. A poll that falls up to two periods behind sends the missed
frames back to back, within the eight-packet SCO pool. A longer stall restarts
the schedule at the current time once and increments a private restart counter.

Mode 3 retains the 16-byte requests and 272-byte response header. Responses use
version 2, and offset 124 holds the actual codec ID: 1 for CVSD, 2 for mSBC.
For codec 2, input and output sample counts are equal; for codec 1, output is
twice input. The profile, codec, rates, audio identity and sample counts remain
bound through all reads and acknowledgments. The host rejects inconsistent
counts or codec changes. An image manifest must identify the pinned native
wideband provider before mode 3 can be prepared. Modes 1 and 2 retain their
existing wire formats.

### Focused software evidence

Evidence resides under
`~/.cache/sunstoneos/evidence/bluetooth-msbc-20261019/`. Actual-stack checks cover
fragmented frames, CRC/sequence/HCI loss, full copied-ring backpressure,
mismatched BCS, missing capability, confirmed CVSD fallback and command refusal.
A separate composition copies two distinct decoded speech segments through the
same stack. Both pass through the unchanged recognizer and match its portable
reference for features, log probabilities, frames and formatted text.

The linked AArch64 codec reproduces all 96,000 copied PCM bytes from 400
producer-derived frames. It also checks retained contexts, initialization
allocation failure and sequence loss. The native USB producer and host checks
cover codec-bound results, exact delivery/ACK, revocation, explicit fallback and
inconsistent codec metadata. A final authorization check after source-status
collection closes the observed revocation window during transcript copying.

Controller/radio events, allocator address effects, scheduling and USB device
effects are modeled. Physical headset operation, transparent eSCO support,
receive-service bounds and recognition within the session deadline remain
unqualified. The speech stimuli originate from existing converted recordings;
these checks establish codec integration, not acoustic wideband quality.

### Caller PCM playback and duplex

The optional mSBC image also accepts runtime caller PCM. Headset mode 4 uses the
752-byte version-2 profile for playback without a recognizer task or model
upload. The selected image still links the existing Mimus/newlib dependencies;
mode 4 initializes runtime allocation and the codec without starting recognition.
Mode 5 uses the 832-byte version-3 profile for playback alongside repeated
capture and recognition. Both require actual mSBC negotiation and the pinned
wideband provider. They reject a CVSD selection before consuming caller PCM;
the capture-only mode 3 retains its explicit CVSD fallback policy.

Caller audio is mono signed 16-bit little-endian PCM at 16 kHz. Each period has
2400 bytes, or 1200 samples. The USB task copies contiguous fragments into one
of two private slots. A complete period and matching SHA-256 commit transfer
ownership to the Bluetooth task. A partial upload cannot reach the encoder.
Commit publication and owner closure use the existing kernel critical section.
The existing privileged USB and Bluetooth tasks perform these operations; the
recognizer retains its existing privilege and copied-input boundary.

The Bluetooth task feeds each 120-sample frame through the pinned libsbc encoder
and prepends the existing H2 sequence. It retains each encoded frame and fragment
offset while the SCO pool is busy. Eight private associations bind SCO packet
serials to caller periods. Period results distinguish encoded samples, submitted
encoded bytes, fully submitted H4/UART packets, controller-buffer releases, flushed
packets and packets never sent. The task records a terminal packet disposition
before releasing that exact SCO record. A skipped observation of the intermediate
SENT state cannot omit UART accounting: RETIRED and FLUSHED also imply complete
local UART submission. Here submission means every H4 byte was accepted by the
UART service; it does not mean an empty FIFO or UART-wire completion.

A period stays immutable until all its associated packets are terminal. Its
result remains owned until the USB task has delivered the whole copied response
and receives the exact ticket acknowledgment. Early, stale and duplicate
acknowledgments cannot free an input slot. Full slots return backpressure without
overwriting accepted data. Initial buffering requires one committed period;
until it arrives, and during subsequent input gaps, the owner encodes zero PCM
through the same encoder and H2 sequence. These silence frames and their packet
dispositions have separate counters and never count as caller samples.

Playback-only continues servicing and releasing incoming synchronous packets,
counting discarded packets, bytes and status loss. Duplex uses the existing two
copied capture slots and delivery-owned transcript log. Capture backlog or loss
seals playback admission and stops both directions; accepted playback results
and authenticated copied transcripts remain available after orderly closure.
Security revocation denies their publication and retains the session. The host
stops uploading after sealing, drains the accepted caller prefix and reports
unaccepted input or loss as failure.

The existing 38-second admission cutoff and 40-second synchronous-close budget
bound both new modes within the overall session deadline. END seals input and
waits for accepted period accounting before requesting disconnect; it does not
wait for USB result ACKs. CANCEL abandons unencoded samples and unsent encoded
frame tails, then uses the existing disconnect protocol after any partial H4
frame finishes. Cancellation before audio setup retains pending setup resources
without claiming a disconnect. A deadline or stalled transport cannot synthesize
controller completion or release uncertain ownership.

Core 5.4 Vol 4 Part E sections 4.1 and 7.7.19 define packet completion as controller
buffer reuse. It may follow transmission or flushing, or precede transmission if
the controller moves data to other storage. Section 4.3 requires outstanding
packets to be treated as flushed after the matched successful disconnect event.
Neither this event nor a completed-packet credit establishes headset rendering
or final serial-wire drain. Physical AIC8800 transparent eSCO operation and audio
rendering remain unqualified. Caller playback at other rates/codecs, broader
Bluetooth audio modes and graceful wire-drained completion remain outstanding.

The private runtime exchange adds discriminator `0x81`; the existing `0x80`
transcript exchange is unchanged. A 16-byte little-endian request contains
`u8 discriminator`, `u8 operation`, `u16 payload_bytes`, `u32 offset` and
`u64 ticket`. Operations are STATUS 0, WRITE 1, COMMIT 2, ACK 3, END 4 and CANCEL 5.
WRITE carries at most 1024 bytes; COMMIT carries the 32-byte PCM digest. Requests
are serialized with bounded partial-header and payload deadlines. Responses have
332 bytes and bind the build, original session, link generations, actual codec,
period ticket and digest. Result ACK is permitted only after actual USB transfer
completion. The host option `--pcm` supplies one to 256 complete periods and
preserves the original bytes with its result evidence.

| Response byte offset | Fields, in order |
| --- | --- |
| 0 | `u32` magic `0x50505442`, version 1, period state, operation outcome |
| 16 | `u64` session epoch, session generation; 32-byte profile digest |
| 64 | `u64` firmware, UART, ACL, audio generations and period ticket |
| 104 | `u64` encoded samples, submitted bytes, UART packets, released packets, flushed packets, not-sent packets |
| 152 | 32-byte period PCM digest |
| 184 | `u32` period flags, codec, rate, period bytes |
| 200 | `u64` accepted periods, acknowledged periods, silence frames, discarded RX packets, discarded RX bytes, RX loss packets |
| 248 | `u32` sealed, canceled, disconnected, free slots |
| 264 | `u64` silence UART packets, released packets, flushed packets, not-sent packets |
| 296 | 32-byte build identity; `u32` echoed operation at offset 328 |

Period state 0 is pending, 1 has an immutable result, and 2 denies publication.
Operation outcomes are 1 accepted, 2 busy and 3 rejected. Period flags distinguish
cancellation (bit 0) from flushed/not-sent packets (bit 1). Unencoded samples and
unsubmitted encoded tails remain visible through the encoded/submitted counts.
Authorization failure zeros period and status fields rather than exposing old
results. Empty slots after sealing do not grant new upload permission.
