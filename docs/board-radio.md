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
closes admission before another radio iteration, retains the actual session and
continues pending CE retention polling. Existing replies remain receivable.

The four pinned firmware blobs contain 391,000 bytes. At the initial one-bit
400 kHz SDIO rate, the payload alone requires at least 7.82 seconds. Protocol
traffic, hashing, delays, association and security add time. A four-second
watchdog window cannot cover cold startup.

The transport counts only consumed successful CMD53 transactions that moved
nonzero bytes. The counter saturates and never wraps. The radio owner publishes
that count and the current bounded operation/session deadline under one short
atomic lock. The watchdog supervisor receives no feed permission on contention,
missing progress or application fault. It must require a strictly increasing
count and enforce the immutable board session cap before each reload.

The configured client task executes one TCP exchange through copied requests:
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
