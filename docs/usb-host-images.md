# USB class images await supervised board admission

The native images implement OS-owned HID input, USB serial and UVC camera
services. Application manifests grant each class separately. Host checks and
native links pass; physical device operation and sustained traffic remain
unqualified. The accepted objective remains incomplete.

## Frozen images identify their devices and expected results

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

Run from the SunstoneOS checkout. Each configuration uses a separate cache
directory. The empty external application path keeps the OS independent of
mimus.

```sh
py="$HOME/.cache/sunstoneos/venv/bin/python3"
cfg="$HOME/.cache/sunstoneos/bin/cfg.sh"
build="$HOME/.cache/sunstoneos/build/integrate-streams-1031/usb/reproduction/native"

for class in input serial camera; do
  "$cfg" "$PWD" "$build/$class" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON \
    -DSUNSTONE_APPLICATION="usb-$class" -DSUNSTONE_A733_OS_USB_CLASSES=ON \
    -DSUNSTONE_BOARD_USB2_IRQ=ON -DCMAKE_C_COMPILER=/usr/bin/clang-21 \
    -DCMAKE_ASM_COMPILER=/usr/bin/clang-21 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 \
    -DPython3_EXECUTABLE="$py" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build "$build/$class" --target "sunstone-a733-os-usb-$class" -j8
done

"$cfg" "$PWD" "$build/camera-jpeg" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON \
  -DSUNSTONE_APPLICATION=usb-camera -DSUNSTONE_A733_OS_USB_CLASSES=ON \
  -DSUNSTONE_BOARD_USB2_IRQ=ON -DSUNSTONE_A733_OS_USB_JPEG=ON \
  -DCMAKE_C_COMPILER=/usr/bin/clang-21 -DCMAKE_ASM_COMPILER=/usr/bin/clang-21 \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 -DPython3_EXECUTABLE="$py" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build/camera-jpeg" --target sunstone-a733-os-usb-camera -j8

"$py" -B apps/usb-host/package.py --build-root "$build" \
  --output "$HOME/.cache/sunstoneos/build/integrate-streams-1031/usb/reproduction/images"
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

The package READMEs give exact offline `apps/os/prepare.py` commands for each
desired run. Preparation requires a sealed workload-zero board prefix, thermal
extent type 1, and USB memory extent type 6. Input grants are `0x2` for keyboard,
`0x20` for mouse or `0x22` for both; serial uses `0x40`, camera `0x80`.
The JPEG image also requires its version-4 extent type 5. These records must
identify actual board authority; the package supplies none.

The new exclusive RAM range is `0x47000000..0x48000000`. The earlier image-RAM
DDR record does not cover it. The coordinator must establish its ownership,
remote-writer exclusion and use before launching these images.
TYPEC24 also needs its existing qualified source, PHY, clock and rail records.
VBUS enable requires coordinator approval and an explicit admitted source
session. These host checks grant neither.
No Type-C source admission record is established by these host checks.

These supervised images retain resources. The protected development
runner currently accepts CPU-return images; it must refuse these images because
their host drain and normal return are unqualified. A supervised USB-class
launch/capture route in that shared runner remains an integration blocker.
An executable board-launch command is therefore not supplied. Direct FEL
commands would bypass that required ownership path.

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
