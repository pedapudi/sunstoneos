# Native USB diagnostics on A733

The FreeRTOS runtime provides a tested USB 2 high-speed device channel on the
attached board's recovery port. It accepts status, echo, log, and recovery
commands. The driver transfers data through programmed I/O and runs in one
FreeRTOS task. The [mimus runtime](mimus-usb.md) extends this channel with asset
uploads and repeated transcription. USB host operation, hardware interrupt delivery, DMA, USB 3,
Type-C role changes, and complete USB qualification remain outstanding.

## Controller and runtime

USB0 uses the sunxi MUSB controller at `0x04100000`. The driver adapts TinyUSB's
sunxi device-controller implementation through the patches in `patches/`.
[Source identities](../sources.lock.json) pin the downloaded implementation.
The AArch64 adaptation uses 32-bit register accesses, A733 clock/reset controls,
and polling from the USB task. Local corrections handle control-transfer
completion coalesced with a subsequent setup packet and arm zero-length IN
packets after responses ending on a packet boundary.

Initialization removes the soft connection, resets the device controller,
enables its gate, and configures the PHY. It inherits parent reference and
interconnect clocks from ROM. Cold initialization and PHY calibration remain
unqualified. This channel uses USB0; the separate DWC3 controller at
`0x06a00000` requires another driver.

The runtime enters nonsecure EL1 through the [EL3 monitor](a733-kernel.md).
The USB task renews the two-second secure recovery timer through SMC
operation `0x734` once per 1 ms tick, before each service burst. A stalled
task stops renewing that deadline.

Each tick runs one service burst and then yields for one tick. A pass of the
burst polls the controller, runs TinyUSB and services the protocol. The burst
repeats passes while a pass handles controller events or moves bytes through
either vendor FIFO. After the last such progress it continues for 125 µs, one
high-speed microframe, so the host can collect a written packet or send the
next one. A burst ends after 200 µs of counter time or 64 passes. An idle
host, or a host that abandons a response, therefore costs one pass per tick,
while an active transfer moves several 512-byte packets per tick. The
sustained transfer rate has not been measured on the board. Boot mode 2
also imposes an eight-second application deadline, so a failed host check
returns independently to ROM. Boot mode 3 implements a resident loop without
that application deadline; it has no physical qualification yet. The mimus
application selects mode 4, whose [host lease and task deadlines](mimus-usb.md#task-ownership-and-recovery)
bound its active session.

The ROM and RAM runtime share USB ID `1f3a:efe8` during development. The native
service has product string `sunstoneos RAM diagnostics` and device revision
`0100`; the verified ROM has revision `02b3`. FEL loaders check the revision
before sending recovery requests. A release USB identity remains unresolved.
The existing session access rule covers both services on the verified port.

## Application protocol

The host sends requests to bulk OUT endpoint `0x02` and reads responses from
bulk IN endpoint `0x81`. High-speed packets carry up to 512 bytes. Full-speed
64-byte descriptors are implemented but have not been physically tested.
Only one request may be outstanding.

Each message starts with four little-endian 32-bit words: magic `0x53534f53`,
sequence, command, and payload length. Payloads contain at most 2,048 bytes.
A response preserves the sequence and sets bit 31 of the command. An unknown
command receives command `0xffffffff` with an empty payload.

| Command | Value | Response payload |
| --- | --- | --- |
| Status | 1 | Five little-endian 64-bit values: tick count, CurrentEL, command count, TinyUSB speed, hardware counter |
| Echo | 2 | The request payload |
| Log | 3 | Up to 2,048 bytes of the runtime log, starting at the byte offset given by an optional 4-byte little-endian payload; an offset at or beyond the log end returns no bytes |
| Recover | 4 | Empty acknowledgment, followed by disconnect and ROM return |

Application commands `0x10` through `0x15` add [asset loading and transcription](mimus-usb.md#application-commands).

A header with the wrong magic or a payload length above 2,048 bytes cannot
delimit its body. The device discards every byte queued in the receive FIFO
and replies with the received sequence, command `0xfffffffe` and an empty
payload. It does not return to ROM. Bytes of the same request that arrive
later are rejected in the same way.

The host resynchronizes with a vendor control request: request type `0x40`
(host to device, vendor), request `0x01`, value 0, index 0 and no data stage.
The device discards a partial request and any unsent response, clears both
vendor FIFOs and cancels a pending recovery command. A packet the controller
has already loaded can still reach the host, so the host reads the IN
endpoint until a short timeout before it sends the next request. A bus reset
clears the same state. The device stalls other vendor control requests.

This is a development protocol. Rejection and the reset request are checked
only in a host model. Physical resynchronization and concurrent clients need
qualification before use as a general application transport.

## Build and check

Configure the compiler and FreeRTOS dependency as described in [build.md](build.md).
Then add the pinned USB source and build only the affected target:

```sh
set -e
cache="$HOME/.cache/sunstoneos"
tinyusb=$(python3 tools/fetch-sources.py tinyusb)
cmake -S . -B "$cache/build/qemu-virt" -DTINYUSB_DIR="$tinyusb"
cmake --build "$cache/build/qemu-virt" --target a733-usb-runtime -j 8
llvm-objcopy -O binary "$cache/build/qemu-virt/a733-usb-runtime.elf" \
  "$cache/build/qemu-virt/a733-usb-runtime.bin"
python3 -m venv "$cache/venv"
"$cache/venv/bin/pip" install -r checks/host-requirements.txt
```

The board must already have initialized DDR and a successful physical kernel
result. The check verifies ROM identity and upload contents, holds the shared
board lock, and requires [automatic reconnect access](../tools/README.md).
Run it when changed inputs require physical validation:

```sh
"$cache/venv/bin/python" tools/usb-diagnostic.py \
  --fel "$cache/build/sunxi-a733/sunxi-fel"
```

The check stops at the first failed request. It does not retry connections or
reset the board after a failure. Inspect host USB metadata to establish that
ROM returned independently before using `tools/read-returned-result.py` to
retrieve retained diagnostics. Pass the failed session's evidence directory
and the same `--fel` executable. That reader refuses an active native service.

The host model in `checks/usb-diagnostic-protocol/check.py` compiles the
actual `drivers/usb/diagnostic.c` for AArch64 and runs it under Unicorn with
a modeled TinyUSB vendor stream, controller and host. It covers status, a
2,048-byte echo serviced within one tick, log reads at several offsets, both
rejected header forms, recovery from a truncated request and from an
abandoned response through the reset request, and one recovery-timer renewal
per tick. It models no controller registers.

The compiled-driver check in `checks/usb-zlp.py` uses Unicorn to verify that
zero-length IN submission arms the transmit register. It resolves that specific
regression; it is not a USB controller model. The separate
`a733-monitor-model` target checks recovery-timer renewal and subsequent expiry
in QEMU. Reuse their evidence while the relevant inputs remain unchanged.

## Physical evidence and limits

On 2026-10-15, the board enumerated at high speed and passed status, log,
and echo requests with payload lengths 0, 496, 497, and 2,048 bytes.
The 16-byte header makes the middle cases end at and cross a 512-byte packet
boundary. Status remained available after a 2.1-second host wait, confirming
that the task renewed its initial recovery deadline. The recovery command
returned successfully to FEL, with automatic access to the recreated USB node.

The successful raw image contains 40,124 bytes. Its SHA-256 is
`545a8ce03bc908689c2488eddf34a048d187897eeaa4bd4b2dfb5464eaa14816`.
ELF sections contain 39,912 bytes of text/read-only data, 201 bytes of initialized
data, and 85,184 bytes of reserved RAM. Diagnostics reserve another 20 KiB at
`0x42000000`, including a bounded USB event trace.

Raw evidence is in `~/.cache/sunstoneos/evidence/usb-20261015-234013`.
It includes transactions, input hashes, upload readback, and the returned result.
The result reports stage 5 and status 0. The compiled-driver result is recorded
in `usb-zlp.json`; monitor renewal evidence is in `monitor-renewal.json` under
the same evidence root. Earlier failed sessions returned automatically and
retain traces that distinguish control-transfer and zero-length-packet defects.

The host script now preserves the previous diagnostic header while waiting for
the USB cooldown and checks the returned ROM revision explicitly. These host
sequencing changes followed the successful session and received source and
syntax checks; they did not change the firmware image. No repeat of the physical
session was needed for those changes.

This result establishes one bounded diagnostic session from a ROM-initialized
board. It does not establish sustained throughput, long-duration stability,
full-speed operation, suspend/resume, other USB classes, or arbitrary fault
recovery. Those checks remain part of the USB qualification issue.

## Native application USB0 service

Supervised native applications own USB0 through the board providers and
expose a vendor result channel on the same endpoints. The shared header
`apps/board/usb0_service.h` supplies their device, configuration, qualifier
and string descriptors from a per-image identity: product ID, device revision
and product, serial and interface strings. It also supplies the TinyUSB
platform hooks, the provider startup and the service loop, and each
application keeps its own failure policy, exchange and transmit accounting.

The loop uses the same burst rules as the diagnostics runtime and rechecks
application liveness and provider health on every pass. Applications with
their own provider checks, such as the GPU display and HDMI audio captures and
the NPU and accelerator owners, use the burst alone. The board image's failure
path parks only USB0. An image that reuses the board's USB0 source can add its
own revocation by defining `BOARD_USB0_STOP()`; the mimus board image uses it
to fail the board and revoke Type-C.

`checks/usb0-service-burst.py` runs the actual service loop against a modeled
vendor stream, controller and host. With a 20 µs host turnaround it moves a
64 KiB response in 13 ticks, a 1,160-byte board frame within one tick and a
2,064-byte request within two ticks. A one-pass loop needs one tick per
packet: 128 ticks for the 64 KiB response. The model also checks that idle
and abandoned hosts cost one pass per tick and that no burst exceeds its
budget. `checks/usb0-service-descriptors.py` runs the descriptor callbacks of
two linked images under Unicorn and compares their bytes; it confirms that
the shared descriptors match the earlier per-application copies.
