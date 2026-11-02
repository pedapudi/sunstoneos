# USB device, host and Type-C

**Status: implemented.**

Native USB0 device transport and bounded xHCI and TinyUSB host classes have host and instruction-model evidence. Device results do not qualify source power or host peripherals.

**Software evidence:** emulator.

**Board-qualified scope:** High-speed inherited-clock USB0 PIO status, echo,
logging and return pass: native takeover clears SoftConn, completes a 40-byte
exchange, quiesces USB and returns responsively
(`board-session-20261023/port-3-3/r9-v3-usb/` and `r9-v4-usb/`). On port 3-1,
with TYPEC24 physically empty, the source-disabled control, a one-second VBUS
pulse and a ten-second VBUS hold return normally; VBUS rises within 6.8 ms and
falls within 2.0 ms (three records below). Loaded-port operation and attached
peripherals remain unqualified.

## Open work

1. Rebuild the input, serial, camera and camera-JPEG images, attach the powered hub and devices, and run each class image on port 3-1. Gen1 needs at least 900 mA plus image-bound COMB0 and topology authority.
2. Qualify physical devices, DMA visibility, interrupt delivery, source current and cutoff, sustained traffic, capture contents and continuity, serial flow control and overruns, repeated attachment and recovery. An overloaded output may stay above the controller threshold, so VBUS loss does not always reveal an overload.
3. Qualify physical cold start, fabric and security ownership, and wider USB1, USB2 and DWC3 operation.
4. Close the real-time gaps in the [real-time audit](driver-realtime-audit.md): a bounded privileged serial submission interface, more than one outstanding TX per endpoint, a connected IRQ wake path for the JPEG variant, and physical timing and idle current.
5. Complete native exact-mode selection, shared application-kit lifecycle binding and verified stream stop; the compatibility camera API still serves current applications.
6. Add wider classes, isochronous OUT, higher SuperSpeed modes, PD, VCONN, role swaps and applicable alternate modes.
7. Resolve the device-class overlap files: `apps/os/client.c`, `drivers/usb/host/uvc-camera.c`, `include/sunstone_uvc_camera.h`, `checks/device-classes-usb.c` and `checks/device-classes-usb.py`.

## Host and Type-C

**Board-qualified scope:** None.

TYPEC24, the USB-C and DisplayPort connector, requires a USB-C adapter or hub. The native stack supplies Type-C ownership, USB2 and selected Gen1 profiles, bounded hubs, HID, BOT sector reads and recovery, serial bridges, UVC and UAC1 microphone capture. Hot-plug inside an admitted adapter or hub connection is implemented; removing TYPEC24 requires a new source session. The runner accepts finite `usb-class` probes through the watchdog marker, quiescence hook and normal-return receipt; the four OS images still lack real host drain and a returning lifetime. Image hashes and superseded queue entries are in [the image guide](../usb-host-images.md). See [host USB](../usb2-host.md) and [Type-C](../typec.md).

## Evidence

Board results are records in `docs/evidence/usb.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-usb-20261101.md).

<!-- evidence-table:begin -->
4 records in `docs/evidence/usb.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793567391558674480-ec1dbf2c` | 2026-11-01 | 3-1 | `f4f58d544a58` | refused-before-launch | inconclusive | run_total 8.214 s |
| `1793570915607590841-5a219698` | 2026-11-01 | 3-1 | `28ee313dea47` | returned | passed | run_total 10.6 s |
| `1793571535501560347-d0b1985c` | 2026-11-01 | 3-1 | `9a241f3c7a3e` | returned | passed | run_total 11.467 s, vbus_rise_max 6.8 ms, vbus_fall_max 2 ms |
| `1793571557988499335-8426c3ed` | 2026-11-01 | 3-1 | `68a7b3702bfb` | returned | passed | run_total 20.616 s, vbus_rise_max 6.8 ms, vbus_fall_max 2 ms |
<!-- evidence-table:end -->
