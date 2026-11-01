# USB device, host and Type-C

**Status: implemented.**

Native USB0 device transport and bounded xHCI/TinyUSB
host classes have host and instruction-model evidence. Device results do not
qualify source power or host peripherals.

**Board-qualified scope:** High-speed inherited-clock USB0 PIO status, echo,
logging and return pass. Native takeover clears SoftConn, completes a 40-byte
exchange, quiesces USB and returns responsively in the frozen records
`board-session-20261023/port-3-3/r9-v3-usb/` and
`board-session-20261023/port-3-3/r9-v4-usb/` under
`~/.cache/sunstoneos/evidence/`.
The [device](../usb.md) and [platform](../usb-platform.md) records bind inputs.

Cold USB0 clock/reset/PHY preparation is host-checked but keeps the device
disconnected. Physical cold start, fabric/security ownership and wider
USB1/USB2/DWC3 operation remain unqualified. The inherited SESSION refusal and
32-bit PCTL SoftConn failure are resolved in the byte-store takeover path;
[the worklog](../hardware-worklog-20261031.md) retains their exact input scope.

## Host and Type-C

**Board-qualified scope:** None.

TYPEC24, the USB-C/DisplayPort connector, requires a USB-C adapter or hub.
The native stack supplies Type-C ownership, USB2 and selected Gen1 profiles,
bounded hubs, HID, BOT sector reads/recovery, serial bridges, UVC and UAC1
microphone capture. Serial/camera services expose retained application views;
optional MJPEG consumption uses the hardware JPEG decoder. Hot-plug inside an
admitted adapter/hub connection is implemented; removing TYPEC24 requires a
new source session.

The corrected class package and composed sanitizer/traffic checks are under
`~/.cache/sunstoneos/build/integrate-streams-1031/usb/`, including `images/`.
[The image guide](../usb-host-images.md) lists exact hashes and the superseded
queue entries. Reused addresses, short HID descriptors and legacy keyboard
state publication have focused corrections. Public C270 traffic contains
negotiation without completed image payloads; FT2232H FPGA/FIFO captures do not
establish serial baud timing.

Launch is blocked on reviewed VBUS/source/PHY/rail authority, exclusive
`0x47000000..0x48000000` RAM ownership and a supervised class route in the
protected runner. Images retain resources and refuse CPU-return lifetime.
VBUS enable requires coordinator approval and an admitted source session.
Gen1 needs at least 900 mA plus image-bound COMB0/topology authority; no physical
bundle exists. Earlier queue entries need corrected hashes or revalidation.

Remaining work includes physical devices, DMA visibility, interrupt delivery,
source current/cutoff, sustained traffic, capture contents/continuity, serial
flow control and overruns, repeated attachment and recovery. Wider classes,
isochronous OUT, higher SuperSpeed modes, PD/VCONN, role swaps and applicable
alternate modes remain open. See [host USB](../usb2-host.md),
[Type-C](../typec.md) and [host image evidence](../usb-host-images.md).
