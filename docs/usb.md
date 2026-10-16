# Native USB diagnostics on A733

The FreeRTOS runtime provides a tested USB 2 high-speed device channel on the
attached board's recovery port. It accepts status, echo, log, and recovery
commands. The driver transfers data through programmed I/O and runs in one
FreeRTOS task. USB host operation, hardware interrupt delivery, DMA, USB 3,
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
Each USB task iteration renews the two-second secure recovery timer through
SMC operation `0x734`. A stalled task stops renewing that deadline. Boot mode 2
also imposes an eight-second application deadline, so a failed host check
returns independently to ROM. Boot mode 3 implements a resident loop without
that application deadline; it has no physical qualification yet.

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
A response preserves the sequence and sets bit 31 of the command.

| Command | Value | Response payload |
| --- | --- | --- |
| Status | 1 | Five little-endian 64-bit values: tick count, CurrentEL, command count, TinyUSB speed, hardware counter |
| Echo | 2 | The request payload |
| Log | 3 | Up to 2,048 bytes of the runtime log |
| Recover | 4 | Empty acknowledgment, followed by disconnect and ROM return |

This is a development protocol. Malformed-message handling, reset during a
transfer, cancellation, and concurrent clients need further implementation and
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
