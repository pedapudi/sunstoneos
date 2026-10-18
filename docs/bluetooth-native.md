# Native Bluetooth controller identification

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
HFP, security/pairing, SCO/headset audio and broader Bluetooth modes remain
required work.

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
Only a valid pending reply permits another query. A terminal identity or fault
ends capture; malformed, late or failed transfers stop immediately. The copied
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
