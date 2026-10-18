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
audio output. Mimus currently accepts finite contiguous 16 kHz input; live input,
resampling and headset-to-recognition integration remain outstanding.

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
