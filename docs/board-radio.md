# Native board radio application

The board radio application connects the native platform, firmware session,
CE entropy reservoir, WPA2 supplicant and IPv4 socket service. Physical operation
and source entropy quality remain unqualified. This document assumes familiarity
with FreeRTOS task ownership and the existing radio session.

The constructor runs during serialized boot after the actual task handles and
shared providers exist. The application borrows the native USB provider's clock
registry, counter, register mutex, pins, rails and PMIC. The existing PMIC task
continues polling its provider. The radio application creates no second PMIC,
clock registry or module power writer.

The supplied immutable profile retains pinned firmware spans, RF/channel
authority, an observed WPA2 target, its RSN information, the PMK, IPv4 settings
and configured socket client tasks. Beacon and entropy records must match their
consumers. Numeric records identify external authority; the application does
not establish that authority. Missing SID access or entropy qualification
refuses construction.

The board session authenticates the pointer-free startup profile at `0x43000000`
by size and SHA-256 digest. Firmware spans lie within the separately retained
read-only range `0x43010000..0x43100000`. Boot authority must establish those
mappings and exclude uploads, DMA and remote writers throughout the lifetime.
The existing firmware engine verifies the pinned artifact lengths and hashes.

The startup profile occupies 1,872 bytes. Outgoing TCP uses runtime version 3
or supervised upload version 4, which converts to runtime version 3 after
authentication. The bounded listener uses runtime version 5 or supervised upload
version 6, which converts to runtime version 5. Other versions are refused. The appended
network fields preserve every preceding field offset, including the checked
response length at byte 1,336 and response data at byte 1,596.

| Offset | Wire field | Meaning |
| --- | --- | --- |
| 1,856 | `uint8_t network_dhcp` | Zero selects fixed IPv4; one selects DHCP. |
| 1,857 | `uint8_t network_dns[4]` | Optional IPv4 DNS server; all zero leaves it unspecified. |
| 1,861 | `uint8_t network_reserved[3]` | Must be zero. |
| 1,864 | `uint32_t address_timeout_ticks` | DHCP address acquisition timeout in FreeRTOS ticks. |
| 1,868 | `uint32_t lease_cap_ticks` | Maximum DHCP lease lifetime in FreeRTOS ticks. |

Fixed addressing requires both timing fields to be zero. DHCP requires both
to be nonzero and no greater than half the runtime maximum tick value. The
board uses 64-bit ticks, so every nonzero 32-bit wire value meets that upper
bound. Startup widens these values to `TickType_t` and passes the policy,
DNS server and timers to the actual network configuration. The profile digest
covers the appended fields. The supervised preparation tool and result
observer enforce the same wire policy and fixed response-length offset.

Native startup allocates one actual cached-pool CE page, obtains a CE token
from the shared registry and constructs the real entropy client under the
common pool mutex. Other CE clients must use that same session and mutex.
The production profile admits only an already qualified, unchanged health-test
epoch. It supplies external characterization and successful startup-test
records, assessed thresholds, the observed chip ID and their evidence digest.
Health reconfiguration is refused because it would invalidate that epoch.
Current physical CE has no such qualification; nonzero fields alone do not
establish it. The authenticated launch authority must refuse unsupported claims.

The runtime identity callback copies the four chip-ID words obtained by the
board's admitted EL3 boot reader. It performs no EL1 SID access and issues no
new secure monitor call. The retained identity storage remains independent of
the radio session. The pinned Allwinner SID implementation selects secure
access for secure systems; its weak direct-read fallback is insufficient
authority for this application.

One radio task polls the actual entropy adapter before advancing the session.
It waits for a completed entropy refill, submits the observed WPA2 target,
attaches security after the real association indication and activates the actual
IPv4 stack after the supplicant authorizes the controlled port. Startup,
association and security retain fixed deadlines. Transient transport activity
defers attachment before consuming supplicant or socket-service storage.

Before sockets exist, each iteration consumes at most one copied dispatcher
event and one diagnostic record. The application retains the latest copies and
counts; this prevents station-event backpressure from blocking startup. Socket
clients consume later event copies through the existing EVENT request.

Application tasks submit requests and receive results through the existing
bounded copied socket queues. Network publication uses a release/acquire flag
so clients cannot inspect partially initialized socket state. Entropy failure
closes admission before another radio iteration and retains the actual session.
Client, session and external board failures set the shared watchdog revocation
bit before publishing application failure. Failed owner iterations call only
the entropy and platform retention APIs; they cannot submit CE work or return
to healthy hardware polling. Existing replies remain receivable.

The firmware engine sends each of the 2,880 written patch-table pairs as its
own padded 512-byte CMD53 transfer and each 1 KiB image chunk as a padded
1,536-byte transfer. Firmware loading therefore moves about 2.04 MB over the
bus, although the four pinned blobs contain about 391,000 bytes. At the one-bit
400 kHz SDIO rate that the platform admits, this data alone takes 40.8 seconds.
With the smallest command, polling and confirmation framing it takes at least
46.7 seconds. The supervised profile allows 40 seconds from session start to its
bootstrap deadline, so firmware loading cannot finish in time; power-up, card
setup, controller latency and the one-transfer-step-per-tick cadence add more.
A four-second watchdog window cannot cover cold startup either.

[`checks/radio-bootstrap-budget.py`](../checks/radio-bootstrap-budget.py)
computes this lower bound from the pinned artifacts, the engine framing, the
admitted module clock and the profile's bootstrap offset. It fails for the
current profile. Do not queue the radio board test with this profile until a
faster or wider SDIO mode is qualified or the engine sends fewer bytes. A later
deadline cannot help, because the supervised session itself ends at 60 seconds.
With `--four-bit`, the check evaluates the radio platform's optional four-bit
25 MHz profile instead; its bus lower bound is about 0.2 seconds. The board does
not request that profile, and it has no physical qualification.

The transport counts only consumed successful CMD53 transactions that moved
nonzero bytes. The counter saturates and never wraps. The radio owner publishes
that count and the current bounded operation/session deadline under one short
atomic lock. The watchdog supervisor receives no feed permission on contention,
missing progress or application fault. It must require a strictly increasing
count and enforce the immutable board session cap before each reload.

The outgoing client task executes one TCP exchange through copied requests:
open, connect, send, receive the supplied expected bytes and close. Partial sends
and receives preserve their offsets. One absolute client deadline bounds the
entire exchange, including queue contention; individual socket requests also
retain their existing fixed tick deadlines. Failure closes application admission
and leaves outstanding sockets and replies retained for owner fault handling.

The focused check executes application coordination and the changed session
busy admission with explicit lower-worker observations. It does not replay
existing firmware, association, security or network suites. Native compilation
and the complete board image link cover the real providers separately. Software
checkpoints do not establish physical radio or entropy qualification.

The selected batch passed both AddressSanitizer/UndefinedBehaviorSanitizer host
executables and compiled the application, session, transport, entropy adapter
and platform for AArch64. The profile cases use actual SHA-256 and refuse a
changed digest, mismatched chip ID, missing qualification evidence, health
reconfiguration and overlapping firmware spans. Coordinator cases exercise
partial TCP sends/receives, current deadline publication, shared recovery-latch
refusal and metadata-only owner polling after fault. The session case proves
that a busy transport leaves supplicant storage fresh before later attachment.

Inputs, artifacts, tool identity and logs are preserved under
`~/.cache/sunstoneos/evidence/board-radio-20261017/resume1/`.
The coordinator executable has SHA-256
`cba7a93af5328268b7227c54ffdd408fb4e432714df4f2126577db783df1e57d`.
The earlier host link failure remains in `initial/`. The continuation reused
its verified successful SHA-256 object and completed only unfinished steps.
Host fixtures model lower protocol observations; unused native callback-table
boundaries abort if invoked. These checks do not execute the native startup
constructor or qualify physical CE, SID, RF, WPA2 or IPv4 operation. The complete
native board image supplies the real constructor and provider linkage.

The focused addressing check is `checks/board-addressing.py`. It compiles its
host fixture before execution and tests authenticated fixed and DHCP profiles,
reserved bytes, missing timers, incompatible sizes and versions, supervised
conversion, DNS digest coverage and copied-result decoding. Lower providers
remain explicit unused fixtures. DHCP acquisition, DNS traffic and physical
radio operation require separate network and board evidence.

The addressing fixture passed with AddressSanitizer and UndefinedBehaviorSanitizer;
the host admission and result-decoding cases also passed. Evidence is under
`~/.cache/sunstoneos/evidence/board-addressing-20261017/profile2/`.
The initial compile exposed an incorrect response-offset assertion and reuse
of the earlier structure's tail padding. The corrected layout preserves the
response at byte 1,596 and aligns the appended policy to byte 1,856.
The failed compilation remains in `profile/`.

The combined supervised board image links the actual DHCP/DNS implementation.
Its linked DNS source uses the native IP-task bind and close helpers. The
board's local string header declares the five runtime string functions required
by DNS. All 157 linked objects and 403 compiler dependencies are preserved under
`~/.cache/sunstoneos/evidence/board-addressing-20261017/central/`.
The image SHA-256 is
`542cf2dca10fb9b14765a195d39174476a230982e2b86c660b3fa1e681c74cde`.
The link establishes native composition. It supplies no physical radio evidence;
bounded DNS timing also remains unqualified as recorded in
[the network evidence](radio-network.md).

## An admitted listener verifies one copied application exchange

The listener profile preserves the 1,872-byte profile and 592-byte board session.
It uses the existing radio workloads and their private boot admission. The
session still authenticates the complete profile size, retained span and SHA-256
digest before the application copies it. Runtime version 5 and uploaded version 6
require `uint16_t tcp_mode` at byte 1,338 to equal one. Older versions require
that field to be zero. The three reserved network bytes remain zero.

For listener mode, `peer_port` is the local IPv4 listening port and must be
nonzero. `peer_address` is the exact admitted remote IPv4 address; zero is refused.
The caller's copied `request` and expected `response` each contain one through
256 bytes. The remote client connects, reads the complete request and sends the
expected response. The board does not accept arbitrary commands or additional
connections.

The existing application task opens the local socket, listens, accepts one peer,
sends the request and verifies the response. Partial send and receive offsets
remain private to that task. It closes the accepted child and then the listener.
Only both actual socket-disposal receipts permit terminal USB success. A wrong
peer, late acceptance or address loss starts bounded cleanup while authority
remains valid. Failure is published after that attempt; loss of overall authority
or deadline retains unresolved resources and uses the existing recovery policy.

The existing launcher and capture protocol keep their wire layouts. Preparation
accepts the authenticated listener profile. Copied USB results retain its digest,
epoch and generation. Host decoding labels the listener mode and requires the
native terminal state and expected verified-byte count. Stack send acceptance,
radio bus completion and socket disposal do not prove TCP serial or RF drain.
No physical station, entropy, power or recovery admission is relaxed.
