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

Steady HID, serial and UVC completion uses IRQ notifications and the shared
architectural timer. Raw owner and application notification waits avoid tick
rounding; application retirement still has a legacy tick loop. Ready
work has a 32-pass bound. Noninterruptible startup and delayed GIC status use
bounded 50 µs retries without extending device deadlines. Lost or late timer
delivery expires the supervisor's observation window. Storage and keyboard
mailboxes notify both their owner and consumer at publication boundaries.
Physical timing and idle current remain unmeasured. The
[real-time audit](driver-realtime-audit.md) records the remaining service,
batching, copy and power gaps.

The JPEG variant retains its existing owner cadence: setup, decoder task-end
and chunked readout have no connected IRQ81/readiness wake path. Serial
submission supports an admitted EL1 caller directly, but applications run at
EL0 and still use the owner mailbox. A bounded privileged submission interface
and more than one outstanding TX per endpoint remain open. Controller/PHY
idle gating needs a proven halt and completed DMA writes; it is not enabled.

The corrected class package and composed sanitizer/traffic checks are under
`~/.cache/sunstoneos/build/integrate-streams-1031/usb/`, including `images/`.
[The image guide](../usb-host-images.md) lists exact hashes and the superseded
queue entries. Reused addresses, short HID descriptors and legacy keyboard
state publication have focused corrections. Public C270 traffic contains
negotiation without completed image payloads; FT2232H FPGA/FIFO captures do not
establish serial baud timing.

The linker and existing uncached allocator own `0x42200000..0x42a00000`
for USB classes. The first 5 MiB lease holds the controller; retained JPEG
allocations fit in the remaining 3 MiB. This replaces the conflicting
`0x47000000..0x48000000` range and leaves the NPU pool at
`0x47400000..0x47c00000` disjoint. Physical RAM ownership and remote-writer
exclusion still belong to runner admission. An optional session extent checks
geometry without treating a digest as authority.

The protected runner accepts finite `usb-class` probes through the existing
watchdog marker, real quiescence hook and normal-return receipt. It requires
coordinator approval for USB source/cutoff authority, including alternate
probe launch kinds. The four OS images retain resources and still lack real
host drain and a returning lifetime; the probe route does not admit them.
The coordinator approves the separate empty-port diagnostics on port `3-1`.
Normal class attachment retains its admitted source session.
Gen1 needs at least 900 mA plus image-bound COMB0/topology authority; no physical
bundle exists. Earlier queue entries need corrected hashes or revalidation.

Remaining work includes physical devices, DMA visibility, interrupt delivery,
source current/cutoff, sustained traffic, capture contents/continuity, serial
flow control and overruns, repeated attachment and recovery. Wider classes,
isochronous OUT, higher SuperSpeed modes, PD/VCONN, role swaps and applicable
alternate modes remain open. See [host USB](../usb2-host.md),
[Type-C](../typec.md) and [host image evidence](../usb-host-images.md).

## Shared camera contract and software evidence

UVC binds from class/subclass and descriptor data; serial vendor bridges use
the existing VID/PID tables. No camera product ID selects the driver. UVC
shares modes and retained frame spans with CSI through
[`sunstone_camera.h`](../../include/sunstone_camera.h). The native USB consumer
uses its mode/view conversion helpers without copying frame payloads. The
generic adapter checks deadlines and retains leases on expired operations.
Native exact-mode selection, shared application-kit lifecycle binding and
verified stream stop remain incomplete; the compatibility USB camera API
continues to serve current applications.

Evidence is under `~/.cache/sunstoneos/build/usb-host-board-1101/`:
`memory/check-final/` covers actual allocator/linker/table/session behavior;
`memory/usb-npu-result.json` checks the selected combined CMake/ELF layout.
`final-runner/` and `final-runner-arena/` cover admission, typed extents and
the exact uncached-arena exception. `final-generic/` and
`final-shared-camera/` cover retained class traffic with read-only aliases.
`final-deadline-health/`, `final-precise-production/` and
`final-mailbox-publication/` cover deadline loss, IRQ wake and mailbox
publication. Independent reviews cover memory, runner, camera and completion
changes. Registers, timer delivery and cache effects are modeled; these
records establish no physical host qualification.

The raw and JPEG OS baseline links pass with their respective precise and
legacy owner selections. The supervised, resident and resident-audio baselines,
selected integration gate and check index pass under
`integration/gate-usb-board-final-runtime/`. The largest recorded build/check
child uses about 143 MiB; the shared host retains over 113 GiB available.
No containers or board operations run. These baseline builds are software
checks; no new qualification image package is frozen.

## TYPEC24 power wiring and fault limits

The V1.2 schematic, sheets 9 and 11, connects PL8 through TYPEC_EN only to
the enable input of U3, the SY6280 switch. R45 pulls that input down through
100 kΩ. U3 switches VCC-5V to VBUS_TYPEC, which supplies TYPEC24 and the
FUSB302's VBUS sense input. Its input supply is shared: VCC-5V also connects
directly to TYPEC16 input power, the main PMIC input, FUSB302 VCONN, HDMI
through D1 and expansion-header pins 2 and 4. A dedicated PL8 enable does not
make the TYPEC24 load independent of board input power.

R46, 4.7 kΩ, connects U3 pin 3 to ground. The schematic labels that pin OCB;
the SY6280 datasheet identifies it as the current-setting input ISET. The
formula `ILIM(A) = 6800 / RSET(Ω)` gives 1.447 A nominal. The datasheet's
0.75–1.25 A limits apply to 6.8 kΩ at 5 V and 25 °C. They do not establish
the board's exact current limit or available input headroom.

U3 regulates excess current and shuts down if sustained overload raises its
junction above a typical 130 °C. It restarts at a typical 110 °C. This thermal
restart does not provide a latched fault cutoff. U3 has no external fault output.
The FUSB302's OCP bit reports its internal VCONN switch; OVRTEMP reports the
FUSB302's temperature. Neither detects U3 current limiting or temperature.
VBUS loss can reveal some overloads, but an overloaded output may remain
above the controller's voltage threshold.

The existing provider revokes host tokens on CC change, VBUS loss, FUSB302
fault, expired observations or lost authority. Its separate GPIO task lowers
PL8 even when the I2C task retains its bus mutex. That task requires a running
CPU, scheduler, clock, GPIO and interconnect. CPU/interconnect failure or reset
therefore has no established independent guarantee of PL8 disable. The
host-armed system watchdog and the switch's enable pulldown do not establish
that guarantee without a verified reset-to-output sequence.

The [source pins](../../sources.lock.json) identify these cached references
under `~/.cache/sunstoneos/sources/`:

| Reference and location | SHA-256 |
| --- | --- |
| Schematic V1.2, `board-documents/schematic-v1.2.pdf`, sheets 6, 9 and 11; render `board-documents/sheet-11.png` | `4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0` |
| SY6280 rev1.0E, `usb2-electrical/sy6280-rev1.0e.pdf`, pages 2, 4 and 7 | `589001cd56c64cc860556e9ecc4e17d14bfbac22ab04df06d8573e8c37be33b1` |
| Vendor DTS, `board-kernel/sun60i-a733-orangepi-zero3w.dts`, lines 16–24 and 2578–2587 | `31145367f604fd756f2c6a67447e5134e4be0ad5fae87672cfaa9b7c6d055a39` |
| FUSB302 rev2 primary-source extract, `typec-host/fusb302-primary-web-extract.json`, pages 14 and 28, embedded lines L702–704, L753–754 and L1430–1433 | `72ebcbba6a1df5257cce5ee1aa7107d8360c52cc0e7882e4ca742259c3e5cce5` |

The FUSB302 extract is a JSON-encoded string from an earlier primary-PDF
extraction; its hash is not a PDF hash. The cached full-PDF fetch returned
HTTP 403. The vendor DTS revision is
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Its active-high PL8 regulator and
1,000 µs ramp delay corroborate the wiring. Its name `usb0-vbus` does not
change the physical connector route. [Type-C sources](../typec-pd-sources.json)
pin the corresponding vendor driver.

## Coordinator approval and empty-port diagnostics

The coordinator approves TYPEC24 VBUS enable on board port `3-1` using the
vendor's active-high PL8 regulator practice. U3's nominal 1.447 A current limit
and thermal shutdown provide the accepted protection for this step. Independent
cutoff equipment and bench measurement of shared-input headroom are not
prerequisites. TYPEC16 supplies board power, and TYPEC24 stays physically empty
for the first diagnostics. This decision replaces the earlier design refusal.
It does not establish loaded-port performance or a latched overload cutoff.

Run a source-disabled control, then a pulse lasting at most one second from
the first PL8-high effect, then a ten-second hold. Each run uses the shared
native probe owner and returns normally. The enabled diagnostics observe VBUS
rise within 275 ms through the FUSB302 VBUS-OK comparator. They monitor CC for
unexpected attachment, lower PL8 and observe comparator deassertion within
650 ms. Comparator deassertion is a voltage-threshold observation; it does
not establish the USB 800 mV safe-zero threshold. The diagnostic publishes no
attachment token and does not start xHCI, PHY or DMA. No connected data device
is possible under the required physically empty connector and open-CC checks.

The separate GPIO guard operates independently of a pending I2C transaction.
A service gap above 2 ms revokes the source. Normal close, fault revocation,
operation timeout and the session deadline request PL8 low before return.
The work deadline reserves the full 650 ms shutdown interval and cleanup time
before the absolute session deadline. Delayed or uncertain I2C completion
cannot postpone the GPIO stop. A failed PL8-low acknowledgment prohibits a
normal return. A failed comparator observation records a failed diagnostic;
it cannot become a successful source-off measurement.

A weak shared probe abort hook lets this diagnostic lower its owned PL8 from
EL3 before the existing fatal park, including session expiry. The hook performs
no allocation, RTOS call, I2C transaction or wait. It clears only the dedicated PL8 data bit and preserves neighboring GPIO
bits. The published first-fault marker suppresses recursive attempts. The
override requires no EL1 ownership flag, so Secure EL3 never depends on dirty
Nonsecure EL1 data. A failed MMIO access cannot erase the recorded origin.
This is bounded software cleanup while the CPU and MMIO path work. It cannot
promise cleanup after an interconnect stall; WDT recovery and runner readback
handle the observed return state.

The runner captures PL8 configuration, data and pull before admission and after
every accessible return, including watchdog returns and setup helpers. It checks
these registers before DDR result reads or another native launch. An output-high
observation fails the run even if recovery succeeds. Recovery clears only PL8's
data bit, preserves neighboring pins, verifies the low state, and records both
snapshots before another admission. Failed transport stops commands to that
instance. Unconfirmed recovery retains quarantine and requires coordinator
physical recovery. R45 cannot overcome an actively driven high output.

The native diagnostic reuses the TWI engine and vendor FUSB302 initialization,
keeps PD and VCONN disabled, and owns only the TYPEC24 board pins and local
S_TWI1 controls. It preserves shared APBS parents. Use the vendor's 400 kHz
S_TWI1 operating rate with its documented divider and completion ordering.
The existing provider's 100 kHz cap remains a separate open implementation gap.

Three reviewers approve the implementation design for simplicity, hardware
performance and correctness. The same reviewers assess the resulting code. Host checks must exercise
the real diagnostic state machine with delayed comparator and I2C completion,
service-gap loss, all shutdown reasons and GPIO write failures. Runner checks
cover pre-admission recovery, normal and watchdog returns, neighboring bits,
transport failure and preservation of the first failure. Physical runs establish
rise/fall and actual reset behavior; host models do not qualify these effects.

## PL8 reset state and return admission

The A733 V1.00 manual, sections 18.14.5.2 and 18.14.7.2–.3/.6, documents
S_GPIO at `0x07025000`. PL8 uses configuration word `+0x04[3:0]`, data word
`+0x10[8]` and pull word `+0x24[17:16]`. Documented defaults are mux 15
(IO disabled), data zero and pull zero (disabled). Output mode is mux 1;
input mode is mux 0. The manual hash is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.

Manual section 8.3.6.5 calls WDT configuration 01 a whole-system reset without
specifying S_GPIO reset-domain coverage. The vendor R_PIO node uses APBS0 and
has no reset binding. The captured ROM's direct FEL entry at `0x20..0x40`
reaches dispatcher `0x2514` and bypasses reset prefix `0x44..0x178`. Thus neither
a documented register default nor normal CPU RMR return proves PL8 becomes low.
The runner uses live readback for both normal and watchdog returns.

The vendor DTS and R_PIO driver are pinned at
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`; their SHA-256 values are
`1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa`
and `431fac7b767f5be39015cdf67fdd3dec230cbc1ebeb2fa0c0de08ca0f6e223a1`.
The captured FIXS ROM hash is
`a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa`.
No complete verified ROM/boot0 PL8 write trace establishes a stronger reset
contract. Observations must retain the raw configuration, data and pull words.

The class attach phase uses a separately powered high-speed USB2 hub with
upstream backfeed protection. Keep TYPEC16 connected for power and recovery.
The hub supply covers the webcam, keyboard, mouse and serial adapter. Gen1,
PD, VCONN and increased source-current advertisements remain outside this step.
Controller halt and completed DMA writes still precede any clock or PHY gating.
