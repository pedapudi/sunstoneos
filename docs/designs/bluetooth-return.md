# Bluetooth headset return

The OS headset grant registers one return step in the device-owner class.
The supervisor publishes its request and observes completion. The task that
owns Bluetooth performs the protocol and hardware work. When Wi-Fi shares the
module, its task continues polling Bluetooth after the Wi-Fi session stops.
This contract applies to the OS capture profiles, modes 2 and 3.

## Retirement and ownership

`bluetooth_application_stop` keeps one absolute deadline and advances the
existing workers. It closes capture and connection admission, disables scan,
stops voice recognition, closes synchronous audio, and closes the audio
gateway, RFCOMM and L2CAP channels. Accepted input and transmit records remain
owned until their existing completion and release rules permit retirement.
The security worker finishes accepted commands and replies without beginning
another setup operation.

Each pass runs the existing SDP client, SDP server, control and synchronous-audio
consumers in their normal order, with their existing byte budgets. It services
synchronous-audio metadata and terminal copied records between consumers.
Every accepted channel keeps its consumer throughout closure, so buffered input
can retire while other channels await their close responses.

An accepted outbound ACL connection requires Create Connection Cancel and
its correlated Connection Complete event. A connection that succeeds while
cancellation is pending requires Disconnect and Disconnection Complete.
Command Status alone cannot establish disconnection. The synchronous-audio
worker similarly finishes an accepted setup before disconnecting its handle.
An unsent setup can close without sending it.

After every lower owner proves retirement, HCI sends one tagged Reset and
requires its matching successful Command Complete. All H4 receive records,
staging and transmit ownership must be empty. Bluetooth Core, volume 4, part
E, section 7.3.2 leaves the HCI transport active after Reset. Therefore the
owner publishes protocol release and waits for a separate Bluetooth peer-stop
proof before closing UART.

The module session closes SDIO admission immediately and drains its accepted
transfer while Bluetooth closes. Its `stop_held` query validates the existing
firmware generation and retained clock, rail and pin ownership. The one-way
`stop_release` request ends this permission and permits physical module stop.
The shared and standalone paths use the same session operations.

The module publishes Bluetooth peer stop only after an explicitly authorized
PM2 reset assertion and readback. WLAN DOWN alone supplies no such proof.
After peer stop, Bluetooth consumes at most 64 residual UART bytes per pass.
The existing UART close then masks and synchronizes its interrupt, drains TX
and RX, asserts reset, waits its 10 microsecond hold, removes the gate,
restores pins and releases its clock token.

During partial startup, no additional firmware or protocol setup begins.
An accepted UART configuration finishes within its original bound. A module
that has started must still produce its positive peer-stop proof. A malformed
frame, lost generation, uncertain write, expired deadline or missing reset
authority produces FAULT. The owner retains uncertain resources and cannot
later report DONE. Neither the supervisor callback nor a terminal owner pass
performs device I/O.

## Immutable reset authority

Profile version 4 appends a 56-byte `a733_radio_bluetooth_stop` contract to
the 832-byte version 3 prefix. Versions 1, 2 and 3 keep their 624, 752 and
832-byte layouts. The version 4 profile is 888 bytes.

The extension contains authority and topology identifiers, a 32-byte wiring
digest, the expected three-byte GPIO configuration, the inherited level and
four reserved zero bytes. Admission requires GPIO output, valid pull and
drive settings, a known PM2 latch value that matches the inherited level, and
the session topology. The complete profile digest authenticates the extension;
the frozen `bluetooth_stop` record must also match its wiring digest.

Only version 4 adds PM2 to the pin manager's authorized scope. The module's
native admission compares the installed extension with the authenticated
profile. Earlier profiles can start under their existing permissions but
require an absent stop extension at native admission. They cannot infer
Bluetooth peer stop from a Wi-Fi shutdown.

## Time bounds and validation

Every phase uses the supervisor's absolute return deadline, including time
spent on earlier owner classes. The total failure-return window remains
3,000,000 microseconds. Existing command and frame deadlines never extend.
The combined upper-protocol close uses the existing two-second interval,
clipped to the common deadline and any earlier active operation deadline.

Scan-disable, pending command completion, cancellation, disconnection, Reset,
module stop and UART drain all consume that same remaining time. With a
500,000 microsecond command declaration, a maximum scan-disable wait, a full
two-second upper close and a maximum Reset wait exhaust the window. That
combination produces FAULT; it does not permit extra module or UART time.

The UART native adapter makes one register-lock attempt per call, waiting at
most one scheduler tick. Contention remains PENDING. Invalid context, retained
ownership, clock failure and deadline expiry remain errors. The shared return
deadline also guards native UART time and MMIO access, including a call that
begins with an earlier session deadline.

Focused checks execute the HCI, ACL, security and synchronous-audio workers
against UART wire fixtures, and exercise the application and OS-owner
boundaries. Profile checks cover the original layouts and each reset-authority
field. Mutation cases reject omitted registration, a stop in the supervisor
callback, premature buffer release and skipped drain proofs. The native
headset fixture image links these services. These results establish software
behavior only; controller response timing and physical retirement remain
unqualified.

The schedule fixture checks a declared two-millisecond synchronous-audio
response bound with a one-millisecond owner interval. It supplies identical
requests and replies to the normal and return consumer batches, including a
modeled UART serialization delay. This comparison checks service opportunities
and retained deadlines; it does not measure controller or task execution time.

The command layouts and terminal events use the pinned Linux HCI definitions
and Bluetooth Core references recorded by `checks/bt-protocol-return.py`.
`checks/bluetooth-headset-return.py` records the Core and HFP references for
synchronous audio. The module reset and power sequence is described in
[the radio power design](radio-vendor-power-sequence.md).

No physical version 4 authority is supplied by these checks. The fitted PM2
reset route and its relationship to the board's low-power clock route require
evidence before deployment. A Bluetooth headset is also required to qualify
graceful peer disconnection and the complete return on hardware.
