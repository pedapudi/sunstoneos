# USB host qualification images and attachment

## Attach checklist after the empty-port runs

- Keep TYPEC16 connected for board power and USB0/FEL recovery.
- Use TYPEC24, the USB-C/DisplayPort connector, for a separately powered
  high-speed USB2 hub with verified upstream backfeed protection. Its supply
  must cover the hub, webcam and other peripherals together.
- Attach a boot keyboard and mouse or combined receiver, a UVC MJPEG/YUYV
  webcam, and a supported CDC-ACM, CP210x, CH340/341, FTDI or PL2303 adapter.
- For serial, connect a voltage-compatible 1 Mbaud, 8N1 test source and
  receiver with common ground. Do not connect an RS-232 voltage interface
  directly to TTL pins. Press a key or move the mouse during the input run.
- Keep TYPEC24 empty for the source-disabled control, one-second pulse and
  ten-second hold on board port `3-1`. The coordinator approves these runs
  using the vendor PL8/SY6280 protection. Each needs PL8 post-return readback
  from the updated runner. Attach the hub only after all three return normally.

The [USB status](status/usb.md) records the approval, reset-state limits and
software cleanup requirements.

## Empty-port images are ready for the updated runner

The package is under
`~/.cache/sunstoneos/build/usb-host-board-1101/qualification/vbus-images/`.
Each directory contains `image.elf`, `decoder.py` and a short README with its
submission command and expected result. All three images pass host admission
and the native cutoff, fatal-cleanup and sequential-retirement models.
These checks establish no physical VBUS result.

| Directory | Test with TYPEC24 empty | ELF SHA-256 |
| --- | --- | --- |
| `control` | Source disabled, open CC, normal return | `f4f58d544a58b21c39298d2fb184b17536c1087e321f4790b7e336904d100ec7` |
| `pulse-1s` | VBUSOK rise, source on for 0.980–1.000 s, VBUSOK fall, normal return | `390c84eed35f4fbc5027278a9d42f5fba37edff92305da6a36c8f6ebabfff3f0` |
| `hold-10s` | VBUSOK rise, source on for 9.980–10.000 s, VBUSOK fall, normal return | `d067b62a130c75cca01d354c11da10b310bbbd823400033212244b5fb230575b` |

The coordinator must integrate the runner PL8 checks before submission and use
that runner for the queue daemon. The observed daemon uses `bringup` without
those checks; no diagnostic entry is submitted to it. Run the control first,
then submit each powered image only after the prior result passes. The
submission commands use the full payload decoder and its failure predicates.
They do not compare dynamic timing fields with a fixed success string.

The four class packages below are archived host artifacts. Their returning
controller and decoder lifetimes remain open; they are not admitted board
entries. Rebuild them after the empty-port results pass and finite class
cleanup is integrated. No physical host result is claimed.

The native images implement OS-owned HID input, USB serial and UVC camera
services. Application manifests grant each class separately. Host checks and
native links pass; physical device operation and sustained traffic remain
unqualified. The accepted objective remains incomplete.

## Archived images identify their devices and expected results

The package is under
`~/.cache/sunstoneos/build/integrate-streams-1031/usb/images/`. Each directory contains
`image.elf`, its native link check and a README with the full hash, source
revision, accessory, expected result and offline preparation command.
The source revision is `9800a681`, including the reviewed USB host changes
and three corrective commits. These images supersede the earlier package under
`~/.cache/sunstoneos/evidence/usb-host-1031/images/`. The earlier images lack
those fixes.

| Directory | Accessory on TYPEC24 | Expected application result |
| --- | --- | --- |
| `input` | Boot keyboard or mouse through a USB-C adapter; a combined receiver or hub can supply both | One 48-byte input event after a key press or pointer action |
| `serial` | CDC-ACM, CP210x, CH340/341, FT232/FT2232 or PL2303 adapter, with a compatible TTL pattern source and receiver | One zero-copy receive/echo at 1 Mbaud, 8N1; equal nonzero receive and transmit byte counts |
| `camera` | C270-class UVC camera | One frame checksum and dimensions; VGA YUYV has 614,400 bytes |
| `camera-jpeg` | MJPEG camera and an admitted JPEG decoder session | One hardware decode and planar YU12 checksum; VGA output has 460,800 active bytes |

| Directory | ELF SHA-256 |
| --- | --- |
| `input` | `e5b9198d85cfdd3d914c79002d034385d12e7a6de2d5ede3955a35c327263400` |
| `serial` | `82b81bcaccea040c8258643c180fa6c600d5a467a11dba5f3fc73ee91b99d244` |
| `camera` | `f64f3aa85e8eeef74f0994a396dacb5528d86ec8ab8add8a231bc81bc6a4e15a` |
| `camera-jpeg` | `0bd0f9389cad310d68be76930cd93f79f4e2f2a5d2339b0220af8268f38b0d56` |

TYPEC24 is the Type-C/DisplayPort connector. Its FUSB302 controls attachment,
and PL8 enables the SY6280 switch from board 5 V to connector VBUS.
TYPEC16 remains board power and USB0/FEL. The pinned vendor device tree and
schematic routing are described in [USB host support](usb2-host.md) and
[Type-C power](typec.md). No USB0 role or power route changes are part of these
class images.

## Builds select the actual native class paths

Run this recipe after the approved empty-port runs pass and finite class
return is implemented. Each configuration uses a separate cache directory. The empty
external application path keeps the OS independent of mimus.

```sh
py="$HOME/.cache/sunstoneos/venv/bin/python3"
cfg="$HOME/.cache/sunstoneos/bin/cfg.sh"
scratch="$HOME/.cache/sunstoneos/build/usb-host-board-1101/qualification"
build="$scratch/native"
mkdir -p "$scratch/tmp"
export TMPDIR="$scratch/tmp"

for class in input serial camera; do
  "$cfg" "$PWD" "$build/$class" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON \
    -DSUNSTONE_APPLICATION="usb-$class" -DSUNSTONE_A733_OS_USB_CLASSES=ON \
    -DSUNSTONE_BOARD_USB2_IRQ=ON -DCMAKE_C_COMPILER=/usr/bin/clang-21 \
    -DCMAKE_ASM_COMPILER=/usr/bin/clang-21 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 \
    -DPython3_EXECUTABLE="$py" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build "$build/$class" --target "sunstone-a733-os-usb-$class" -j2
done

"$cfg" "$PWD" "$build/camera-jpeg" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON \
  -DSUNSTONE_APPLICATION=usb-camera -DSUNSTONE_A733_OS_USB_CLASSES=ON \
  -DSUNSTONE_BOARD_USB2_IRQ=ON -DSUNSTONE_A733_OS_USB_JPEG=ON \
  -DCMAKE_C_COMPILER=/usr/bin/clang-21 -DCMAKE_ASM_COMPILER=/usr/bin/clang-21 \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 -DPython3_EXECUTABLE="$py" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build/camera-jpeg" --target sunstone-a733-os-usb-camera -j2

"$py" -B apps/usb-host/package.py --build-root "$build" \
  --output "$scratch/images"
```

The package tool refuses an existing destination and verifies the native link
record against each ELF hash. It also checks that the linked symbols contain
the class services, DMA leases and memory admission. The JPEG image must contain
the existing hardware decoder and native provider. The JPEG variant explicitly
selects MJPEG; the default camera image prefers VGA YUYV when available.

## Host evidence covers the transport and application boundary

Evidence directories below are relative to
`~/.cache/sunstoneos/build/integrate-streams-1031/usb/`. Raw downloads remain in the source
cache named by the component source records.

| Evidence | Result and boundary |
| --- | --- |
| `checks/generic/result.json` | Six sanitizer cases: actual TinyUSB HID, CP2102 receive/echo/detach/reattach, complete VGA YUYV over ISO and bulk, MJPEG selection and unsupported-format refusal. Controller, peripheral and interrupt delivery are modeled. |
| `checks/generic-alias/result.json` | The actual application TX adapter reverses the read-only payload alias; the echoed TRB points to the original RX DMA bytes. A held buffer remains readable through detach and is released before reattachment. |
| `broker.log` | Clang 21 sanitizer checks for grants, pending results, ownership, disconnect, frame pinning, retirement and explicit release. |
| `checks/serial/result.json` | Actual upstream bridge request encoding, public descriptor layouts, FTDI packet views, backpressure, echo and quiescence. UART timing remains untested. |
| `checks/serial-capture/result.json` | 1,401 public FT2232H IN completions replay to 472,568 payload bytes matching an independent packet decoder. The recorded device uses FPGA/FIFO mode; this does not measure UART speed. |
| `checks/memory/result.json` | Actual AArch64 table builder and eight modeled cold-entry contexts cover NC mappings, read-only aliases and L3 checksum refusal. Cache effects are modeled. |
| `checks/legacy-memory/result.json` | The feature-disabled memory code is byte-identical to the preserved legacy image. |
| `native/` | Input, serial, camera and hardware JPEG camera images pass their selected native link checks. |

Independent review covered descriptor/report bounds, retained DMA ownership,
detach, application grants and the VBUS admission path. Separate fixes exclude
detaching records from reused-address lookup, reject truncated HID report
descriptors and synchronize legacy keyboard publication. The merged-tree host
checks cover those regressions, captured traffic and generic/alias composition.
The keyboard fixture also passes ThreadSanitizer while the application reads
on another thread.
Public HID recordings supply 445 reports. The camera fixture preserves all
38 reported C270 video modes and 11 alternate settings. Nine captured Logitech
probe responses exercise negotiation; their capture contains no completed image
payloads. Frame assembly checks therefore use constructed image traffic.
The [HID](usb-hid-sources.json), [serial](usb-serial-sources.json) and
[camera](usb-uvc-sources.json) records identify provenance and license boundaries.
The separate [serial traffic record](usb-serial-traffic-sources.json) pins the
FT2232H packet capture and its FIFO-mode boundary.

The four earlier proposals below remain in the 3-3 board queue. They identify
pre-review images and do not cover the corrected package. The coordinator must
replace or revalidate them against the new hashes before any launch. Their
archived source and artifact records are under
`~/.cache/sunstoneos/board-test-proposals/usb-host-1031/`; no queue entry was
changed during this integration.

| Image | Queue proposal ID |
| --- | --- |
| `input` | `ac508f359fc4032c1e0e95a15f1d81329c0fb16ddd45d643bc36aa1ce416dcc9` |
| `serial` | `a5a8a61711c4695dbd2898b8f84e24aa1b1dcc6f40018d3d89247ecf5e3eaed4` |
| `camera` | `3a4427abffbc02651cdb20e1ea0bc6c53cd09b836d46633dabbf897bdb559c9b` |
| `camera-jpeg` | `f86259938e0237b586d998f0447c8ed7c65412297039f16c1ffee2ce594b38dc` |

## The coordinator must establish launch and physical authority

The archived package READMEs give offline `apps/os/prepare.py` commands for the
older images. Their mandatory USB memory extent describes the old reservation.
Current source uses the existing memory owner and accepts an optional extent
type 6 only when its geometry matches the linked reservation. Preparation
still uses a sealed board prefix and thermal extent type 1.
Input grants are `0x2` for keyboard,
`0x20` for mouse or `0x22` for both; serial uses `0x40`, camera `0x80`.
The JPEG image also requires its version-4 extent type 5. These records must
identify actual board authority; the package supplies none.

The current linker reserves `0x42200000..0x42a00000` as an 8 MiB uncached arena.
The existing allocator owns it; USB takes the first 5 MiB lease and retained
JPEG allocations fit in the remainder. The old `0x47000000..0x48000000`
reservation overlaps the private NPU pool at `0x47400000..0x47c00000`.
The archived images must be rebuilt before current qualification. The runner
must establish physical ownership, remote-writer exclusion and restoration
for the new arena; linker and host checks establish no physical authority.
TYPEC24 also needs its existing qualified source, PHY, clock and rail records.
VBUS enable requires coordinator approval and an explicit admitted source
session. These host checks grant neither.
No Type-C source admission record is established by these host checks.

The protected runner supports `--kind usb-class` for finite native probes,
using the shared watchdog contract and verified quiescence/normal-return
receipt. Coordinator approval is mandatory. The four OS images retain
resources and lack that returning lifetime, so the new route refuses them.
It supplies no substitute for host halt, completed DMA writes or decoder
retirement. An executable class launch command is therefore not supplied.

No new physical queue entries or class image packages are submitted while
these prerequisites are unresolved. The empty-port control must report no
device and keep PL8 low. The separate empty-port pulse requires independent
cutoff and coordinator approval described in
[USB status](status/usb.md). A completely empty port does not satisfy normal
Type-C attachment. A Rd/load fixture is needed for loaded qualification.
After power approval and real finite cleanup, run the source-only pulse
before the input, serial, raw camera and JPEG camera images.

Raw class owners and application notification waits use the architectural
timer and IRQ wakeups. The JPEG image preserves its existing waiter until
decoder setup, IRQ81 and chunked readout have real wake/readiness hooks.
UVC mode/frame spans share the CSI camera contract; complete native lifecycle
and application-kit binding remain open. See the
[real-time audit](status/driver-realtime-audit.md).

For the requested physical runs, attach the named accessory to TYPEC24 while
keeping TYPEC16 on the development host. Establish a 5 V source capable of the
admitted 500 mA load and the reviewed RAM reservation. For serial, provide a
known 1 Mbaud pattern source and receiver; later qualify continuous traffic,
bridge overrun reporting and RTS/CTS with its pins connected.
For cameras, qualify real MJPEG/YUYV contents, payload continuity and delivery
latency. The available public camera traffic does not supply this evidence.

Device hot-plug is implemented while the Type-C adapter or hub remains connected.
Removing TYPEC24 revokes the admitted source session; a fresh Type-C attachment
needs a new session. After a retained run, supervised recovery requires removing
all board and externally supplied hub/accessory power, then restoring the same
board's ROM access. Preserve both microSD cards. No board command was run for
this change, and no reconnect is requested from a cached board-state conclusion.

Requested breadcrumb names for coordinator integration are `USB_HOST_ENUM`,
`USB_HID_REPORT`, `USB_SERIAL_CONFIG`, `USB_SERIAL_RX`, `USB_UVC_CONTROL`,
`USB_UVC_FRAME` and `USB_JPEG_DECODE`. The shared breadcrumb header is unchanged.
