# Latent issues

This document lists confirmed defects in SunstoneOS at commit 70a4e63
(branch `audit/integration`), ordered from high to low severity. Each entry
gives the symptom, the root cause with links to the code, a concrete failure
scenario, the fix, and how to verify the fix. Every finding was re-read
against the code by three independent reviewers. Where a reviewer changed the
severity, narrowed the scenario or replaced the recommended fix, the entry
says so under **Review corrections**, and the fix shown is the corrected one.

Almost nothing in the tree has run on the board, so most of these defects are
latent: the code that contains them has passed host checks against modeled
hardware but has not yet executed on the Orange Pi Zero 3W.

## Terms used in this document

- **Retain, retained.** Project policy: when a driver cannot tell whether
  hardware finished an operation, it keeps every clock, rail, pin, memory and
  lock lease it holds until whole-board power removal. The policy itself is
  intended and is not reported as a defect. Several defects below are cases
  where an ordinary, harmless condition is wrongly classified as uncertain and
  so triggers retention.
- **BUSY.** The result a shared registry (clock, rail, PMIC, PCK or Type-C
  publication) returns when another task holds its short compare-and-swap
  (CAS) critical section. BUSY means no effect has happened and the caller may
  retry.
- **Tick.** The FreeRTOS tick is 1 ms. Almost every driver task in the tree
  advances one step per `vTaskDelay(1)`, so a step happens at most once per
  millisecond.
- **FEL.** The Allwinner boot-ROM USB recovery mode the host uses to load and
  start images. The single connected board is in FEL mode and cannot be
  physically reconnected if its USB link is lost.
- **SPI (interrupt).** A GIC shared peripheral interrupt. Where the serial bus
  is meant, the text says "SPI bus".
- **W1C.** A status bit that software clears by writing 1 to it.

## Severity

| Severity | Meaning | Entries |
|---|---|---|
| High | Expected to fail the first physical run of an affected image, or to fail it with high probability under ordinary conditions | 11 |
| Medium | Fails under a plausible but less common condition, or costs scarce board attempts while failing closed | 15 |
| Low | Defensive hardening, a rare race, a hazard reachable only by a future image, or a tooling defect with a workaround | 28 |

Three pairs of findings from different audit areas describe one defect. They
are merged below and cite both identifiers, so the 54 entries cover 57
findings:

- the counter-clock try-lock (clock-power-pins-7 and x-concurrency-3);
- unchecked patch fuzz (x-duplication-2 and build-system-5);
- launcher generation by text substitution (supervisor-apps-7 and
  x-duplication-3).

## Recurring causes

Three causes account for many of the entries. Fixing each one consistently is
cheaper than fixing the entries one at a time.

- **BUSY treated as a terminal fault.** A driver checks registry authority
  before or after every register access and treats any result other than
  "healthy" as a fault. A collision with another task's few-instruction
  critical section then retains the whole session. Affected:
  clock-power-pins-2 (TWI), usb-3 (xHCI and USB2), video-2 (VE codecs). The
  shared fix is a bounded retry of the authority check, up to the operation
  deadline, without re-issuing any register access. The per-access check
  itself stays, because component documents require it.
- **Deadlines shorter than the 1 ms poll grid allows.** Several bounds were
  copied from busy-wait loops in vendor drivers that sample every few
  microseconds. A task that samples once per tick sees only one to three
  samples in such a window. Affected: display-1, display-2, audio-3,
  storage-4 and radio-net-1.
- **Host models that never produce the hazardous condition.** In many
  entries the passing evidence comes from a model that sets a status bit only
  when the driver expects it, never returns BUSY, or returns an idealized
  attribute. Each verification step below therefore adds a model case that
  produces the condition.

## Summary

| ID | Severity | Defect |
|---|---|---|
| runtime-memory-1 | High | Eight-core newlib images use newlib's NEON `memset` at EL3 with the MMU off, which faults on Device memory |
| clock-power-pins-2 | High | TWI native adapter turns registry BUSY into a terminal fault and can retain the global register lock |
| usb-3 | High | USB host turns registry BUSY in the middle of a register sequence into a permanent session fault |
| radio-net-1 | High | Radio firmware bootstrap cannot finish inside its 40 s deadline at the fixed 400 kHz one-bit SDIO clock |
| radio-net-3 | High | Any Wi-Fi receive aggregate larger than 4 KiB is a terminal transport fault |
| bluetooth-1 | High | Unhandled HCI events permanently occupy the four H4 receive slots |
| display-1 | High | Display-engine register-queue replay faults when an ordinary frame-end lands between its clear and its check |
| video-1 | High | VE decoder platform rejects the 26 MHz crystal reference used by one documented board |
| host-tools-1 | High | Fixed 15 s FEL timeout kills the 14.7 MB mimus preload and quarantines the board |
| x-coherence-1 | High | HDMI-audio DMA mapping check requires a shareability value that Normal non-cacheable pages never report |
| x-duplication-1 | High | Returning-image build scripts link old objects against a changed `struct a733_platform_locks` layout |
| smp-1 | Medium | Uncached eight-core diagnostic lacks the Secure PMR and EL0 WFE fixes the cached path needed |
| storage-3 | Medium | Multi-block read ending at the last card sector faults on a permitted OUT_OF_RANGE status |
| storage-4 | Medium | One 1 s deadline covers two SD write-busy periods and the verify read |
| radio-net-4 | Medium | A data frame admitted after port-open and before network start blocks the radio transport |
| radio-net-5 | Medium | A Wi-Fi group rekey cancels pending ACCEPT and DNS requests with ADDRESS_LOST |
| bluetooth-3 | Medium | SCO transmit schedule anchored to "now" runs about 6% slow on the 1 ms grid |
| audio-3 | Medium | End of I2S playback treats the expected FIFO underrun flag as a fault |
| display-2 | Medium | HDMI PHY lock wait of 2.5 ms needs two samples taken a tick apart |
| video-2 | Medium | Registry BUSY during a VE codec register access permanently fails the codec session |
| buses-pcie-1 | Medium | PCIe sends configuration reads without the post-reset readiness wait |
| supervisor-apps-7, x-duplication-3 | Medium | SRAM launchers are generated by unchecked `str.replace` on C source |
| build-system-4 | Medium | Loaded images embed absolute host paths through `__FILE__` |
| host-tools-4 | Medium | Evidence dependency closure drops static libraries, including the mimus ASR library |
| checks-harness-1 | Medium | UBSan builds recover from undefined behavior, so checks pass with UB present |
| x-duplication-2, build-system-5 | Medium | Build-time patches apply with GNU patch's default fuzz |
| cpu-startup-1 | Low | ROM-return recovery reads a loop bound from memory EL1 can overwrite |
| cpu-startup-5 | Low | Monitor cleanup restores SPI enables but never clears SPIs enabled during the run |
| clock-power-pins-1 | Low | TWI11 camera bus shares the PMIC's I2C lock |
| clock-power-pins-7, x-concurrency-3 | Low | Counter clock faults permanently after 256 contended lock attempts |
| bluetooth-8 | Low | An AT+BAC from the headset during an open audio link faults the SCO object |
| audio-4 | Low | Finite TX with three or more periods fails if only two are committed before START |
| audio-12 | Low | Cancelling a never-started I2S stream briefly enables the bit and frame clocks |
| camera-4 | Low | Camera planes carry no fill pattern, so unwritten regions are not detectable |
| camera-6 | Low | Sensor stop errors are not separated from errors on the captured frame |
| gpu-3 | Low | Informational GPU firmware events fail the GPU session |
| security-misc-2 | Low | Thermal, GPADC and LRADC drivers retain on an already-expired caller deadline |
| security-misc-3 | Low | LRADC read faults if the next conversion lands between acknowledge and readback |
| security-misc-4 | Low | Maintenance watchdog compares the RTC control word including its write-busy bits |
| security-misc-5 | Low | RTC alarm arm retains on a benign second rollover |
| buses-pcie-8 | Low | UART interrupts are never re-enabled after a DMA lease is released |
| supervisor-apps-13 | Low | Radio profile hardcodes a 60 s session independently of the lifetime macro |
| build-system-3 | Low | Build identity omits several included headers and the real compile flags |
| build-system-10 | Low | Profile fragments force `SUNSTONE_BOARD_SUPERVISED` into the CMake cache |
| build-system-15 | Low | `fetch-sources.py` trusts partial extractions and shares temporary file names |
| host-tools-3 | Low | mimus-board admits up to 32 recognitions inside a 60 s native session |
| host-tools-8 | Low | Unprotected device-identity read during the expected FEL disconnect |
| host-tools-9 | Low | Dashboard keeps a hand-copied list of physical-result anchors |
| host-tools-10 | Low | Board lock contention reports a raw errno without the port or lock path |
| checks-harness-4 | Low | Negative check cases accept any exception |
| checks-harness-12 | Low | Resumed check drivers overwrite failed-attempt logs and command records |
| checks-harness-14 | Low | Check outcomes rely on Python `assert`, which `PYTHONOPTIMIZE` removes |
| x-concurrency-4 | Low | pcie-test ISR treats a legal level-interrupt re-delivery as a fault |
| x-concurrency-9 | Low | Single-core `platform_puts` appends to the result log without synchronization |

## High severity

### runtime-memory-1: newlib `memset` faults at EL3 in the eight-core newlib images

**Symptom.** The eight-core images that link newlib
(`sunstone-a733-mimus-native`, `sunstone-a733-mimus-board`, and the newlib
variants of the I2S and Bluetooth images) are expected to take an EL3
alignment fault on the first core during cold boot, before any core enables
translation. The image never reaches the scheduler.

**Root cause.** The cached eight-core boot path runs C at EL3 with
`SCTLR_EL3.M=0`, which makes every data access Device-nGnRnE. Device memory
faults on unaligned accesses and on `DC ZVA`. Clang lowers aggregate
zero-initializers in that boot C to `memset` calls; for example `local_boot`
in [continue.c:96](../../platform/a733-smp-cached/continue.c#L96) zeroes a
136-byte struct. The diagnostic, storage and board images link
[runtime/memory.c](../../runtime/memory.c), a byte loop that is safe on
Device memory. The newlib images remove it and `runtime/memmove.c`
([a733-mimus-native.cmake:18](../../cmake/a733-mimus-native.cmake#L18),
[a733-mimus-board.cmake:20](../../cmake/a733-mimus-board.cmake#L20),
[a733-i2s-application.cmake:60](../../cmake/a733-i2s-application.cmake#L60),
[a733-bluetooth.cmake:128](../../cmake/a733-bluetooth.cmake#L128)), so the
calls resolve to newlib's AArch64 assembly `memset`. That routine uses SIMD
registers, overlapping unaligned 16-byte pair stores for the head and tail,
and `DC ZVA` for fills of 160 bytes or more. It also breaks the rule in
[docs/a733-smp-cached.md](../a733-smp-cached.md) that boot C uses only general
registers.

**Failure scenario.** In a disassembly of a cached `mimus-native` build,
`local_boot` calls `memset` with length 136. The tail store `stp q0, q0,
[x4, #-64]` writes 32 bytes at an address that is 8 mod 16, which faults on
Device memory. `a733_cpu_coherence_init`, `a733_cpu_continue_init`,
`a733_cpu_entry_init/poll` and `a733_cpu_start_init` also call `memset` or
`memcpy` in that image. QEMU fixtures may not model the Device alignment
fault, so the host and emulator evidence does not show the problem.

**Fix.** Keep the EL3 boot objects off libc:

- Give the boot translation units (`continue.c`, `cpu_continue.c`,
  `cpu_coherence.c`, `cpu_entry*`, `cpu_start`) a private, general-register,
  aligned-only `memset` and `memcpy`, for example through an objcopy
  `--redefine-sym`, or replace their aggregate initializers with explicit
  field stores. Compile the private routine with `-fno-builtin` so the
  compiler does not turn its loop back into a `memset` call.
- Do not force the byte loop onto the whole image. The mimus engine relies on
  newlib's fast `memcpy` for heavy work on Normal cacheable memory.

An alternative that matches the AGENTS.md preference for a single mechanism
is to link `runtime/memory.c` and `runtime/memmove.c` everywhere, but only if
that implementation is first made fast on aligned Normal memory.

**Verification.** Rebuild the affected images. Add a host check that
disassembles each final ELF and fails if any function reachable from the EL3
boot path contains SIMD or floating-point instructions, `DC ZVA`, or a call to
libc `memset`/`memcpy`. Scanning only the `memset` symbol is not enough. Run
the existing QEMU mimus parity check unchanged.

**Review corrections.** The original rating was critical; reviewers lowered it
to high because the fault is deterministic but has not happened on hardware,
and the cached images are not yet admitted by the host launcher. The fault
comes from the misaligned SIMD tail store; `DC ZVA` is reached only by
larger fills elsewhere. The disassembly was taken from an earlier cached
build; the CMake removal and the `={0}` initializer are unchanged at 70a4e63.
Only the `mimus-native` ELF was disassembled, so each other affected ELF must
be checked. The original recommendation to link `runtime/memory.c` into every
image was replaced with the boot-private routine above because of its
performance cost to mimus.

### clock-power-pins-2: TWI native adapter treats registry BUSY as a terminal fault

**Symptom.** A camera I2C transaction on TWI11 fails and retains its lock
whenever another task is briefly inside the clock registry or rail ledger. If
the collision happens while the adapter is preparing the bus, the adapter
retains `A733_PLATFORM_REGISTER_LOCK`, which every clock and pin writer uses.

**Root cause.** `healthy()` in
[a733_twi_native.c:59](../../drivers/i2c/a733_twi_native.c#L59) returns 0 when
either registry reports BUSY. `read32` and `write32` call it before and after
each access ([line 126](../../drivers/i2c/a733_twi_native.c#L126)) and return
0, and the engine in [a733_twi.c:97](../../drivers/i2c/a733_twi.c#L97) maps
any non-1 result to `goto fault`. In `a733_twi_native_prepare`, the same BUSY
reaches `fault: retain(n)` while only the register lock is held
([line 223](../../drivers/i2c/a733_twi_native.c#L223)). Each MMIO access takes
at least four CAS operations on the two registries. The PMIC provider handles
the same BUSY correctly by returning `A733_PMIC_BUSY`
([a733_pmic.c:319](../../drivers/power/a733_pmic.c#L319)).

**Failure scenario.** The camera task spins `a733_twi_poll` while the PMIC
provider task on another core queries the clock registry every millisecond.
An overlap makes `read32` see BUSY, the transaction faults, and TWI11 and its
I2C lock are retained. Because the USB provider refuses on any retained lock
bit ([native_usb.c:27](../../platform/a733/native_usb.c#L27)), the USB
evidence channel also stops reporting.

**Fix.** Keep the per-access authority check, which
[docs/camera-native.md](../camera-native.md) requires, and make BUSY
non-terminal:

- Retry `healthy()` in place while it returns BUSY, bounded by the
  transaction deadline. Do not return to the engine with a pending result
  after a store has already been issued, because re-reading the data register
  or repeating a control write is not idempotent.
- In prepare, a BUSY seen before the first store should release the register
  lock and return pending. After the first store, use the same bounded retry.
- Apply the same change to `authority()` in
  [a733_camera_native.c](../../drivers/camera/a733_camera_native.c), which
  requires `a733_clock_token_status==OK` and fails the sensor bus on BUSY.
- Keep the fault for a real authentication failure, a failed access or an
  expired deadline.

**Verification.** In the camera native or TWI host model, make
`a733_clock_token_status` return BUSY on the Nth call during SEND, WAIT and
prepare. Assert that the transaction completes after retry and the
platform-locks retained mask stays 0.

**Review corrections.** The original recommendation to drop the per-access
checks contradicts the documented contract and was replaced. In the camera
app a TWI fault already ends the session, so the main effect is a spurious
bring-up failure. The Type-C path uses the same engine through a different
provider and was not checked. Severity stays high because the overlap is
likely on eight cores.

### usb-3: USB host turns registry BUSY mid-sequence into a permanent session fault

**Symptom.** The USB2/USB3 host session in the board and mimus-board images
can fault permanently during normal operation when another task holds a
registry critical section. The controller, device slots and DMA memory are
retained, Type-C is revoked, and there is no reattach path.

**Root cause.** `checked()` in
[a733_xhci.c:57](../../drivers/usb/a733_xhci.c#L57) faults unless `guard()`
returns exactly 1, and `rd`, `wr`, barriers and DMA copies call it before and
after each access. `guard()` calls `a733_usb2_platform_live`, which returns 0
whenever the clock, rail, PCK or PMIC registry is busy or the Type-C
publication CAS is held
([a733_usb2_platform.c:68-78](../../drivers/usb/a733_usb2_platform.c#L68)).
Only the first guard in `a733_xhci_poll`
([line 439](../../drivers/usb/a733_xhci.c#L439)) treats 0 as retryable. The
comment at [a733_xhci.c:44](../../drivers/usb/a733_xhci.c#L44) states this
design: BUSY is retryable only before a bounded operation begins. The design
turns ordinary contention into retention, which exceeds the project policy of
retaining only after uncertain failures. `permitted()` in the USB2 platform
setup path has the same flaw.

**Failure scenario.** On the eight-core platform, the PMIC task's per-tick
poll, the Type-C and radio callers of `a733_pmic_live`, and the USB owner all
enter the same short CAS sections. The USB owner performs two full provider
checks per register access. During `port() → rd(PORTSC) → checked()`, one
collision makes `a733_pmic_live` return BUSY and the session faults. Over a
session lasting minutes with many thousands of guarded accesses per second, a
collision is plausible.

**Fix.** Inside `checked()`, retry the observation while it returns 0, up to
`x->deadline`, without issuing MMIO. Keep the fault for -1, for deadline
expiry and for a failed access. After a write, re-observe before advancing.
This keeps the rule in [docs/usb2-host.md](../usb2-host.md) that every
controller operation needs a fresh provider observation. Apply the same change
to `permitted()`.

**Verification.** In the xHCI and TinyUSB sanitizer checks, make the provider
`live()` callback return 0 once between the PORTSC read and the doorbell
write. Assert that the core retries and reaches READY instead of RETAINED.

**Review corrections.** The original first recommendation, to evaluate
authority once per poll and check only fatal conditions inside a sequence,
weakens the documented contract and was dropped. The scenario named the
single-core mimus image, which uses only the TinyUSB device role; the
affected images are `apps/board` and `apps/mimus-board`. The one-tick sleep
while the PMIC holds its busy flag happens only when a PMIC transfer is
queued; the common trigger is short CAS overlaps. One reviewer rated the
defect medium because the path fails closed and is not physically qualified.

### radio-net-1: radio bootstrap cannot meet its deadline at 400 kHz one-bit SDIO

**Symptom.** On the first physical startup of the supervised board radio
profile, the AIC8800 firmware upload is expected to time out and retain the
whole radio graph. Wi-Fi and Bluetooth bootstrap cannot complete inside a
supervised session.

**Root cause.** The profile sets the bootstrap deadline to session start plus
40 s inside a 60 s session
([radio_profile.c:19](../../apps/board/radio_profile.c#L19)). The platform
caps the SDIO module clock at 800 kHz
([a733_radio_platform.c:256](../../drivers/net/a733_radio_platform.c#L256)),
which gives a 400 kHz clock on one data line ([docs/sdio.md](../sdio.md)).
The firmware engine sends each patch-table word pair as its own 512-byte
padded block and pads every 1 KiB image chunk to 1,536 bytes
([aic8800_firmware.c:149-174](../../drivers/net/aic8800_firmware.c#L149)).
The published table has 2,880 writable pairs and the images need 361 chunks.
That is 2,029,056 bytes of CMD53 data, about 40.6 s at 0.4 Mbit/s, before
command, response, status-poll, power-up and SHA-256 time. The host models
complete bus operations instantly, so the overrun never appeared.

**Failure scenario.** The transport sees `now >= deadline` during the
patch-table or Wi-Fi image upload, calls `fail(TIMEOUT)` and retains the
radio. Raising the deadline cannot help, because the session ends at 60 s.

**Fix.**

- Add a host check that computes bus time from the real padded
  `request_bytes` at the admitted clock and fails when it exceeds the
  bootstrap deadline minus power-up time.
- Reduce bus time. The main fix is a qualified 4-bit or faster SDIO mode.
  Secondary options are sending word writes as short byte-mode CMD53
  transfers (the transport currently requires multiples of 512 bytes) and
  coalescing consecutive table pairs into memory-block writes. The vendor
  protocol writes table words one at a time, so coalescing needs care.
- Correct [docs/board-radio.md](../board-radio.md) and
  [docs/board-application.md](../board-application.md), which give a
  7.82 s minimum based on 391,000 raw bytes and ignore the padding.
- Do not queue the radio board test with this profile until bus time fits.

**Verification.** The new budget check fails on the current tree and passes
once bus time fits inside the deadline.

**Review corrections.** The data volume alone exceeds the deadline; the
estimate that per-step polling pushes the total past 60 s is plausible but was
not verified step by step.

### radio-net-3: Wi-Fi receive aggregates over 4 KiB are a terminal fault

**Symptom.** When the AIC8800 firmware reports more than eight 512-byte
blocks of pending receive data, the transport faults and the radio is
retained until reboot.

**Root cause.** The transport owns a 4,096-byte receive buffer
([aic8800_sdio_transport.h:6](../../include/aic8800_sdio_transport.h#L6)).
`rx_size()` treats any larger report as `AIC8800_TRANSPORT_RX_BOUNDS`
([aic8800_sdio_transport.c:412](../../drivers/net/aic8800_sdio_transport.c#L412)).
The status register encodes function-1 block counts up to 112 blocks
(57,344 bytes); values 113 to 119 and 121 to 127 select function 2, and 120
selects byte mode ([lines 468-472](../../drivers/net/aic8800_sdio_transport.c#L468)).
The upstream Linux driver reads the whole reported length and walks several
records in one buffer. This host polls slowly by design (one step per tick,
about 50 KB/s on the bus), which gives the firmware time to coalesce frames.
[docs/radio-transport.md](../radio-transport.md) and
[docs/radio-runtime.md](../radio-runtime.md) accept the 4 KiB bound as a
current qualification limit; it blocks sustained data traffic.

**Failure scenario.** Three back-to-back 1.5 KB frames from a TCP download,
DHCP with ARP, or a burst of scan results arrive while the host is still
draining. The firmware reports 10 blocks and `rx_size` faults.

**Fix.** Size the receive buffer to the largest function-1 aggregate (112
blocks, or 64 KiB for simplicity). Either raise the SDIO controller's per-CMD53
block limit (4,096 bytes in [docs/sdio.md](../sdio.md)) so one transfer reads
the aggregate, or read it as several fixed-address block-mode CMD53 transfers.
Whether the firmware accepts a split read is unverified and must be checked
on hardware. Keep the dispatcher's diagnostic copy cap as a separate constant.
Keep `RX_BOUNDS` for counts above the new buffer, byte-mode lengths over 512,
and lengths that are not word or block multiples. Update both radio documents.

**Verification.** In the host transport model, report status 0x0a (10 blocks)
containing three data records and one confirmation. Assert that every record
is parsed in order and no fault occurs; keep the malformed-length cases.

**Review corrections.** The original finding gave the maximum as 127 blocks;
the code's encoding limits function 1 to 112. That ordinary bursts will exceed
eight blocks is an inference from the protocol and polling rate, not a
hardware observation. The Linux line cited is `aicwf_sdio.c:1596` in the
cached tree.

### bluetooth-1: unhandled HCI events pin the four H4 receive slots

**Symptom.** After a real headset connects, the Bluetooth stack is expected to
stall and then fault, before or during capture.

**Root cause.** The H4 transport has four receive slots
([bluetooth.h:68](../../apps/bluetooth/bluetooth.h#L68)). `dispatch()` in
[bt_acl.c:309](../../drivers/bluetooth/bt_acl.c#L309) returns 1 for every
event it does not recognize and leaves the record owned, and `bt_acl_step`
then advances past it, so no worker visits it again. L2CAP consumes only ACL
records and SCO only its own. The only drain API, `bt_l2cap_other()` and
`bt_l2cap_release_other()`, has no caller in `apps/`, and the application
releases `bt_hci_next()` records only for the version-1 identity profile
([bluetooth.c:196](../../apps/bluetooth/bluetooth.c#L196)). The event mask in
[bt_hci.c:30](../../drivers/bluetooth/bt_hci.c#L30) enables every classic
event, including Role Change (0x12), Mode Change (0x14), Max Slots Change
(0x1b), Packet Type Changed (0x1d), Page Scan Repetition Mode Change (0x20),
Sniff Subrating (0x2e), Link Supervision Timeout Changed (0x38) and Remote
Host Features (0x3d). [docs/bluetooth-acl.md](../bluetooth-acl.md) requires
the platform to drain unconsumed records; the headset platform does not.
Other leaks: Read Remote Features Complete (0x0b) that does not match SCO's
link, stale command-complete or command-status events, a failed Connection
Complete with no matching link, and an ACL or synchronous Connection Request
that no owner expects.

**Failure scenario.** A headset connection produces Max Slots Change, Page
Scan Repetition Mode Change and Link Supervision Timeout Changed within
seconds. With three slots pinned, ACL, SCO and command traffic share one slot.
With four pinned, `bt_h4_step` reads no more UART bytes and stops
transmitting, because transmit requires a receive pass with free room. The
outstanding command or L2CAP retransmission timer then expires and the
controller lifetime faults. The modeled checks never inject these events.

**Fix.**

- Narrow `config_mask` to events an owner consumes: 0x03, 0x04, 0x05, 0x06,
  0x08, 0x0b, 0x11, 0x16 to 0x18, 0x2c, 0x2d, 0x30, 0x31 to 0x34, 0x36 and
  0x3b. The comment "all defined Classic events" describes the root cause.
- In the ACL dispatch, release the informational events that stay enabled
  after validating their length. Record new state only if a consumer needs it.
- Reject an unexpected synchronous Connection Request with Reject Synchronous
  Connection (0x042a) from a designated owner. ACL cannot issue that command
  today.
- A backstop drain in the headset poll is optional. It must skip SCO records
  held in `held_rx` and any record whose owner may still claim it, and it
  needs its own scan because `bt_l2cap_other` returns the lowest serial.

**Verification.** In
[checks/bluetooth-headset.c](../../checks/bluetooth-headset.c), inject the
informational events listed above plus a vendor 0xff event after Connection
Complete, and an unexpected eSCO Connection Request during audio. Assert that
capture completes and every H4 record returns to free before DONE.

**Review corrections.** The original evidence cited a "length-0 vendor event"
in the headset check; that case is a two-byte partial header used to test a
record boundary, so it shows nothing about leaks. The controller does reject an
unanswered Connection Request itself after its accept timeout; the defect is
the leaked host record. Whether the AIC8800 emits each event has not been
observed physically.

### display-1: register-queue replay faults on an ordinary frame-end

**Symptom.** A display-engine (DE) replay of a register command queue (RCQ)
can fault with `A733_DE_VERIFY` while the display is running normally. The DE
then enters FAULT_RETAINED and the whole output faults.

**Root cause.** `replay_poll()` runs while the DE is scanning out, so
FRAME_END (bit 0 of status register 0x8144) is set again every 16.7 ms at
60 Hz. Step 3 clears bits 0 and 2 and step 4 faults if either is set; step 16
clears bit 0 and step 17 faults if it is set again
([a733_display_engine.c:176-190](../../drivers/display/a733_display_engine.c#L176)).
Each step is a separate poll, and every app polls once per tick
([gpu-display/display.c:501](../../apps/gpu-display/display.c#L501)). A
frame-end in the roughly 1 ms between a clear and its check is therefore a
fault. Both host models set bit 0 only when the step-16 write arms it
([checks/dsi-replay-native.c:33](../../checks/dsi-replay-native.c#L33)).

**Failure scenario.** Each clear-and-check pair has about a 6% chance
(1 ms / 16.7 ms) of catching a frame-end. Two pairs per replay give about 12%,
and the gpu-display sequence of three replays fails roughly 30% of runs.
Contention on the register mutex widens the window.

**Fix.** Keep RCQ_FINISH (bit 2) a hard fault at step 4. When bit 0 is seen
at step 4 or step 17, return to the clear (step 3, or step 15/16) and try
again, bounded by the existing deadline. Accepting bit 0 at step 17 as the
awaited frame-end is unsafe: it would accept a stale bit if the W1C clear
failed. Give both host models a frame-end source that sets bit 0 on a
periodic read count, independent of the driver's step.
[apps/displayport/memory.c](../../apps/displayport/memory.c) (around lines
344-347) has the same race and should get the same retry.

**Verification.** Change
[checks/dsi-replay-native.c](../../checks/dsi-replay-native.c) and
[checks/gpu-display-replay.py](../../checks/gpu-display-replay.py) to set bit
0 on reads at steps 4 and 17. The current code fails with VERIFY; the fixed
code completes with `replay_frame_end >= replay_rcq_done`.

**Review corrections.** The original primary fix (accept bit 0 at step 17)
was replaced with the retry above. One reviewer rated the defect medium
because it fails safe and has not run physically. It assumes frame-end latches
with interrupt enables clear; step 18 already depends on that.

### video-1: VE decoder platform rejects the 26 MHz crystal reference

**Symptom.** On a board with a 26 MHz crystal, every decode path (JPEG, H.264
and the planned HEVC) fails at the first platform step, before any VE access.

**Root cause.** `parents()` in the decoder platform keeps its own copy of an
old reference-clock guard. It requires `XO_CTRL0` bits 15:14 to be 0 or 3
(24 MHz) and the PLL_REF multiplier N to equal the divider M
([a733_video_platform.c:106-109](../../drivers/video/a733_video_platform.c#L106)).
[docs/provider-clock-reference.md](../provider-clock-reference.md) records
that this guard rejects a correctly normalized 26 MHz reference and that other
providers moved to `a733_clock_ref24_valid()`. The encoder platform already
uses the helper
([a733_video_encode_platform.c:88](../../drivers/video/a733_video_encode_platform.c#L88)).
The host model feeds `OSC=0` and `N=M=100`, so it never shows the problem.

**Failure scenario.** With the preserved second-board values
`OSC=0x983fb0f7` (bits 15:14 = 2, 26 MHz) and `REF=0xf8675f00` (N=96,
M=104), both the selector test and `n!=m` reject. `acquire()` fails, the JPEG
and H.264 polls report FAULT, and the session retains its leases.

**Fix.** Replace the inline test with `!a733_clock_ref24_valid(osc,ref)`. The
helper already enforces 80 ≤ N ≤ 116, so the separate N range check is not
needed. The helper also requires the DCXO enable and stable bits, so the host
model must change in the same patch. Set `OSC` to an enabled, stable value
such as 0x10000002. Add the 26 MHz pair as a positive case, and add negative
cases for a disabled DCXO and for mismatched N and M.
[drivers/pci/a733_pcie.c:150-153](../../drivers/pci/a733_pcie.c#L150) has an
identical copy of the old guard and needs the same fix. Add both providers to
the list in the clock-reference document.

**Verification.** Run
[checks/a733-video-platform.c](../../checks/a733-video-platform.c) with the
26 MHz pair and assert that both the inherited baseline and cold entry accept.

**Review corrections.** One reviewer rated the defect medium: it fails closed,
affects only boards with a 26 MHz or 19.2 MHz crystal, and which board is
connected was not established.

### host-tools-1: fixed 15 s FEL timeout kills the mimus preload

**Symptom.** The first physical `check-a733-mimus-supervised` or mimus-board
launch is expected to stop during preload, before the image runs, and
quarantine the board.

**Root cause.** `transaction()` in
[check-a733-smp-supervised.py:333](../../tools/check-a733-smp-supervised.py#L333)
runs every `sunxi-fel` invocation with `timeout=15`. `preload` sends each
segment as one invocation that writes the whole extent and reads it back
([check-a733-smp.py:253](../../tools/check-a733-smp.py#L253)). The mimus
`assets.bin` segment is 14,758,784 bytes; mimus-board adds a 7,501,968-byte
base model and a 6,560,592-byte overlay. Recorded hardware throughput is about
0.57 MB/s per direction, so the mimus write and readback need about 50 s, and
each mimus-board segment needs more than 20 s. `check-a733-kernel.py` and
[a733-returning-host.py:44](../../tools/a733-returning-host.py#L44) were
already raised to 120 s for this reason; `run_session` was not.

**Failure scenario.** `subprocess.run` kills `sunxi-fel` in the middle of the
bulk transfer. `transaction()` marks the run failed and `stop_after_failure`
quarantines the ROM instance by policy. No recognition result is produced.

**Fix.** Prefer splitting uploads into chunks of about 1 to 2 MiB, each with
its own readback, which also limits what is left pending in the ROM if a
timeout occurs. If the timeout is scaled instead, use a floor of about
0.25 MB/s per direction plus fixed overhead. Record monotonic start and end
times for each transaction in the session journal so throughput can be
tuned. Apply the rule only to the other 15 s runners that move large
extents.

**Verification.** Add a host check that loads each `prepare.py` bundle and
asserts that every preload invocation's timeout covers its bytes at the
throughput floor plus margin. Add a fake transport that delays in proportion
to size and confirm a 14.7 MB segment completes.

**Review corrections.** The original 0.5 MB/s floor is close to the measured
rate and is not conservative. The board is quarantined by policy; an earlier
interrupted read was recovered without reset using
`tools/finish-fel-read.py`, so the original claim that a manual reconnect is
required overstates the physical effect.

### x-coherence-1: HDMI-audio DMA admission requires the wrong shareability

**Symptom.** Finite HDMI audio is expected to be refused at its first DMA
submission on the board and under QEMU.

**Root cause.** `nc_mapping()` in
[apps/hdmi-audio/native.c:161](../../apps/hdmi-audio/native.c#L161) admits
the descriptor page and PCM span only if each page translates with attribute
0x44 (Normal non-cacheable) and `PAR_EL1.SH == 3`. The architecture reports
SH as 0b10 for Device memory and for Normal non-cacheable memory, whatever the
descriptor says. Every other non-cacheable validator accepts 2, for example
[memory_uncached.c:27](../../platform/a733/memory_uncached.c#L27) and
[gpu_vm_memory.c:20](../../platform/a733/gpu_vm_memory.c#L20). The passing
evidence comes from the Unicorn stub in
[checks/hdmi-audio-native.py:136](../../checks/hdmi-audio-native.py#L136),
which returns SH=3 for every non-Device address.

**Failure scenario.** The non-cacheable pool initializes with SH=2 and the
audio allocation is created. `a733_dma_submit` then calls `nc_mapping()`,
which rejects the first page, and the submit returns `A733_DMA_INVALID`.
`dma_cache()` calls the same function and would escalate to `dma_fatal` if
reached first.

**Fix.** For attribute 0x44, accept shareability 2 or 3, as
`memory_uncached.c` does; keep SH=3 for write-back pages such as the PCM
source check at [line 202](../../apps/hdmi-audio/native.c#L202). Better, use
one shared non-cacheable mapping predicate across all sites. Change the stub
to return 2 for 0x44 and Device pages, and add a case that returns 3. Change
[docs/hdmi-audio.md](../hdmi-audio.md) to describe the accepted shareability
the same way [docs/usb2-host.md](../usb2-host.md) does.

**Verification.** With the corrected stub,
[checks/hdmi-audio-native.py](../../checks/hdmi-audio-native.py) fails before
the fix and passes after it.

**Review corrections.** Older Arm ARM text says PAR.SH "can be" the resulting
attribute for non-cacheable memory; newer text mandates 0b10. Accepting both
values is correct under either reading.

### x-duplication-1: returning-image builds mix old objects with a changed struct layout

**Symptom.** Rerunning `apps/npu-returning/build.py`,
`apps/thermal-returning/build.py` or `apps/usb-returning/build.py` at HEAD
builds an image whose parts disagree on the layout of
`struct a733_platform_locks`. The build reports success.

**Root cause.** These scripts copy pinned objects from a 2026-10-18 evidence
directory (`pmic-provider-probe-20261018`), including
`01-shared_locks.obj` and `03-a733_clock_leases.obj`, and link them with units
compiled from the current tree
([thermal-returning/build.py:37](../../apps/thermal-returning/build.py#L37)).
The only check is each object's SHA-256. Commit 19582ec on 2026-10-20 grew
the struct from two locks to three
([a733_platform_locks.h](../../include/a733_platform_locks.h)), which moves
`mutex[]`, `retained`, the callbacks and `initialized`. The clock-lease header
also gained new bits and raised `A733_CLOCK_ALL`, which the reused clock-lease
object does not know.

**Failure scenario.** The old `a733_platform_locks_init` writes at the
two-lock offsets. The new
[npu-returning/main.c:157](../../apps/npu-returning/main.c#L157) reads
`locks.mutex[0]` at the three-lock offset, and
[thermal-returning/main.c:55](../../apps/thermal-returning/main.c#L55) passes
a NULL mutex to `xSemaphoreGetMutexHolder`, which stops at a `configASSERT`.
The NPU image most likely fails at a `REQUIRE` during GPU adapter
initialization. Each outcome wastes a board attempt.

**Fix.** Before linking, read each reused object's `.d` dependency file and
compare every listed header, including `FreeRTOSConfig.h`, with the current
file; refuse the build on any difference. Put this in one helper that takes the
baseline path as a parameter. Rebuilding the shared provider objects from
current source also works, but the images would then no longer be built from
accepted objects, which is a policy decision.

**Verification.** A host script that diffs the reused objects' header
closures against HEAD flags `include/a733_platform_locks.h` today; the patched
build script refuses.

**Review corrections.** `apps/rtc-returning/build.py` does not reuse the lock
object and is unaffected. The original claim about `struct a733_adc_native` is
wrong: that driver is compiled fresh. One reviewer rated the defect medium,
because images already built at their original revision are consistent and
the failure is a bounded assert.

## Medium severity

### smp-1: uncached eight-core diagnostic lacks the Secure PMR and WFE fixes

**Symptom.** The standalone `a733-smp-diagnostic` image would hang if launched
as documented.

**Root cause.** Physical multicore work produced two corrections: commit
2633e50 initializes the Secure `ICC_PMR_EL1` at EL3, and commit cddf71b sets
`SCTLR_EL1.nTWE` so EL0 `WFE` does not trap. Both went only into
[platform/a733-smp-cached/continue.S](../../platform/a733-smp-cached/continue.S).
The uncached image links
[platform/a733/cpu_continue.S](../../platform/a733/cpu_continue.S), which
still sets `SCR_EL3=0x535` (FIQ routed to EL3) without the Secure PMR write and
enters EL1 with `SCTLR_EL1=0x30d00800`. With `SCR.FIQ=1`, the Nonsecure PMR
write is ignored while the Secure mask is below 0x80.
[docs/a733-smp.md](../a733-smp.md) still presents the image as waiting only for
watchdog qualification.

**Failure scenario.** If the Secure PMR resets below 0x80, the tick and SGI0
are never delivered. The deadline is checked only inside the IRQ handler
([platform/a733-smp/platform.c:108](../../platform/a733-smp/platform.c#L108)),
so the image hangs until the external watchdog resets the board and records no
result. Separately, a contended EL0 `WFE` may trap with exception class 0x01
into the abort loop.

**Fix.** Retire the uncached image and the `#if defined(__aarch64__)` native
section of [platform/a733/cpu_continue.c](../../platform/a733/cpu_continue.c).
That also makes `platform/a733-smp-cached/continuation-core.patch`
unnecessary; remove it and update the three CMake files that apply it and
their identity input lists together. Update
[docs/a733-smp.md](../a733-smp.md), the link in
[docs/hardware.md](../hardware.md), and the `tools/check-a733-smp.py`
admission path. If the image is kept, port the Secure PMR write with its
readback check before the `SCR_EL3` write, and set `SCTLR_EL1` bit 18; then
regenerate the reviewed image manifest.

**Verification.** Run the EL0-access and Secure-PMR model cases from
[checks/a733-supervised-scheduler.py](../../checks/a733-supervised-scheduler.py)
against the diagnostic ELF, or confirm the target is gone from CMake and the
checks.

**Review corrections.** Lowered from high: launch is blocked by four unmet
prerequisites in `tools/check-a733-smp.py`. The original scenario said the run
would end with status 3; it would hang with no result instead. The Secure PMR
reset value is architecturally unknown, so non-delivery is inferred from the
cached path's correction.

### storage-3: last-sector multi-block read faults on OUT_OF_RANGE

**Symptom.** A read of two or more sectors that ends at the card's last
sector can fault the SD/eMMC core permanently.

**Root cause.** Reads use open-ended CMD18 with automatic CMD12. `CARD_ERRORS`
includes bit 31 (OUT_OF_RANGE) and is applied to the auto-stop response
([sunxi_mmc.c:359](../../drivers/storage/sunxi_mmc.c#L359)). The SD and eMMC
specifications let a card set OUT_OF_RANGE when CMD18 reads up to the last
block and tell the host to ignore it. Linux ignores it for every open-ended
transfer. The model check exercises a read ending at `sectors-1` but never
sets the bit.

**Failure scenario.** A storage-read request for the tail of the backup GPT
entry array, or a storage-update verify readback that ends on the last sector
after a confirmed write, receives OUT_OF_RANGE. The core enters
`SUNXI_MMC_FAULT` and retains its leases and buffer; in the update case the
update is left incomplete.

**Fix.** For an open-ended CMD18 with `first + blocks == card.sectors`, mask
bit 31 in the auto-stop response check and, as a hedge, in the CMD13 that
immediately follows. Keep it fatal in every other case and leave the CMD18
response check unchanged. Record the rule and its source in
[docs/mmc.md](../mmc.md).

**Verification.** In [checks/sunxi-mmc.c](../../checks/sunxi-mmc.c), set bit
31 in the auto-stop response for an 8-block read ending at `sectors-1` and
assert DONE; add a negative case where the same bit faults a read that does
not end at the last sector.

**Review corrections.** A single-block backup GPT header read uses CMD17 and
is unaffected. The storage-update BACKUP phase reads before any write, so a
fault there leaves no uncertain write. A card "may" set the bit; setting it is
optional.

### storage-4: one 1 s deadline covers two SD write-busy periods and the verify

**Symptom.** A storage-update WRITE chunk can fault on a correctly working
SDXC card, leaving a started write, retained resources and a host intent with
no result. Recovery needs a new qualified backup and restore session.

**Root cause.** Each request gets `operation_deadline = now + min(remaining,
1 s)` ([a733_storage_update.c:112](../../drivers/storage/a733_storage_update.c#L112)).
A WRITE chunk carries two sectors, each a separate CMD24 followed by CMD13
polling through programming, and the verify read must finish by the same
deadline. SDXC cards may stay write-busy for 500 ms per block; Linux allows
10 s.

**Failure scenario.** Two consecutive blocks near the SDXC limit use the whole
second before the verify read starts. The core faults with `write_started`
set.

**Fix.** Use one-sector WRITE chunks (keep two-sector reads) and give the
verify read its own deadline that starts after the write completes. That
needs matching changes to the deadline-equality checks in
`a733_mmc_platform_update_io` and to the write chunk size in
[tools/storage-update.py](../../tools/storage-update.py). If the per-request
bound must grow instead, change `A733_MAINTENANCE_OPERATION_US`, the watchdog
reserve and window checks, and the 1,500 ms host USB timeout together. Record
the per-sector busy budget in [docs/storage-update.md](../storage-update.md).

**Verification.** In
[checks/a733-storage-update.py](../../checks/a733-storage-update.py), model a
card that holds the programming state for 500 ms per sector; assert that a
WRITE plus verify completes and that exceeding the documented bound still
faults.

**Review corrections.** SDSC and SDHC cards (250 ms per block) usually fit;
the overrun needs SDXC or eMMC worst-case busy on consecutive sectors, which
is rare. The 400 kHz transfer-overhead estimate was not verified.

### radio-net-4: a data frame between port-open and network start blocks the transport

**Symptom.** WPA2 association can stall after the four-way handshake, and the
board retains the radio when its security timeout fires.

**Root cause.** The port-open confirmation sets `authorized` and
`port_confirmed` together
([aic8800_security.c:99](../../drivers/net/aic8800_security.c#L99)), so the
data worker starts converting and holding ordinary Ethernet frames. In the
MANAGEMENT stage no consumer takes them
([aic8800_dispatch.c:137](../../drivers/net/aic8800_dispatch.c#L137)). While
a record is held, the transport does not drain the aggregate
([aic8800_sdio_transport.c:503](../../drivers/net/aic8800_sdio_transport.c#L503)),
so the runtime step that promotes the supplicant to AUTHORIZED never runs, and
`attachment_ready` (which requires no held record) stays false. The reconnect
path already discards such frames
([a733_radio_session.c:136](../../drivers/net/a733_radio_session.c#L136)).

**Failure scenario.** The access point sends a broadcast ARP or mDNS frame
that the firmware places after the SET_CONTROL_PORT confirmation in the same
aggregate. The frame is held and the supplicant never reaches AUTHORIZED. A
frame that arrives after AUTHORIZED but during a poll in which
`start_network` fails its attachment check also blocks attachment
permanently.

**Fix.** In the MANAGEMENT, BOOTSTRAP, WIFI and DATA session polls, consume
held non-EAPOL data frames as discarded diagnostics, as `reconnect_poll`
already does. Gating data conversion on network attachment would also work
but changes the data worker's contract in [docs/radio-data.md](../radio-data.md).

**Verification.** In
[checks/aic8800-supplicant.c](../../checks/aic8800-supplicant.c) or the board
radio model, deliver the port confirmation and an encrypted broadcast in one
aggregate; assert AUTHORIZED, a successful `start_network` and no fault.

**Review corrections.** The open-network path is called only from check
harnesses, so for open networks this is an API hazard. One reviewer found the
window narrower than another did; all agree on the same-aggregate case.

### radio-net-5: group rekey cancels ACCEPT and DNS with ADDRESS_LOST

**Symptom.** During a periodic Wi-Fi group-key renewal, a pending listener
ACCEPT is cancelled with `SUNSTONE_NET_ADDRESS_LOST`, and a RESOLVE that
starts during the window returns no address.

**Root cause.** wpa_supplicant enters WPA_GROUP_HANDSHAKE for each group rekey
(`wpa.c` around line 3471 for this non-MLO RSN path). The adapter then clears
AUTHORIZED and `security->authorized`
([aic8800_supplicant.c:28](../../drivers/net/aic8800_supplicant.c#L28)), so
`aic8800_network_ready()` is false. `aic8800_network_address()` is defined
through `ready`
([aic8800_network.c:57](../../drivers/net/aic8800_network.c#L57)), so
`accept_stop`, `accept_ip_step` and the DNS START step in
[sunstone_net.c](../../drivers/net/sunstone_net.c#L32) read the address as
lost. [docs/radio-network.md](../radio-network.md) says a rekey only pauses
data admission. `operation()` already waits while not ready.

**Failure scenario.** Rekeys typically happen hourly. A long-waiting ACCEPT is
cancelled by the first worker pass in the window, and the board listener
treats that as a terminal client failure, although the lease and association
are intact.

**Fix.** In `accept_stop`, `accept_ip_step` and the DNS START step, apply the
rule `operation()` uses: report LINK_LOST when `network.failed` is set, report
ADDRESS_LOST only when `network.address_failed` is set or the lease has
expired, and otherwise wait. Generation change and link loss already set
`network.failed`. A new "paused" state is not needed.

**Verification.** In the host network model, start an ACCEPT and a RESOLVE,
inject a group-key message 1 of 2 and complete the rekey; assert both finish
normally or by their own timeout, and that transmit is admitted after the
second port-open confirmation.

**Review corrections.** The original line citations into `wpa.c` pointed at
the pairwise and MLO paths. A query already waiting is not cut off; only a
RESOLVE whose START runs in the window fails, and it returns `SUNSTONE_NET_IO`.

### bluetooth-3: SCO transmit cadence drifts about 6% slow

**Symptom.** Caller playback over mSBC produces 125 frames per second instead
of 133.3, so the eSCO link underruns repeatedly.

**Root cause.** The next transmit time is computed from the poll time at which
the previous frame was accepted
([playback.c:156](../../apps/bluetooth/playback.c#L156),
[headset.c:471](../../apps/bluetooth/headset.c#L471)). The owner task polls
once per tick ([application.c:25](../../apps/bluetooth/application.c#L25)), so
every 7.5 ms period rounds up to 8 ms, and frames split by the SCO MTU add
further polls. CVSD silence has the same error when its period, `(silence_bytes
/ 2) × 125 µs`, is not a whole number of milliseconds (MTU 60 gives 3.75 ms).

**Failure scenario.** The controller repeatedly runs out of transmit data in
playback modes, which may produce gaps or garbage frames at the headset. In
capture-only modes the transmit direction carries silence, so the main effect
is controller underruns.

**Fix.** Advance the schedule by the period (`next += 7500`, or the sample
count × 125 µs), keeping the existing deadline caps. If the lag exceeds a
small bound, resynchronize once and count it. Keep one or two frames queued
ahead within the eight SCO transmit slots.

**Verification.** In
[checks/bluetooth-playback.c](../../checks/bluetooth-playback.c) and
[checks/bluetooth-headset.c](../../checks/bluetooth-headset.c), step the model
clock in 1 ms ticks with jitter; over 10 s, assert the mSBC frame count is
`floor(t / 7.5 ms)` within one and the CVSD byte count is 16,000 × t within
one packet.

**Review corrections.** The delay is a deterministic round-up, not a 0.5 ms
average. Audible effects on the AIC8800 are inferred, not observed.

### audio-3: end of I2S playback treats the expected underrun as a fault

**Symptom.** Runtime file playback is likely to end in RECOVERY instead of
INPUT_ENDED, losing the result although every sample was delivered.

**Root cause.** After the final finite TX transfer, TX stays enabled and the
worker polls the FIFO status until 128 entries are free. Each end-of-input
poll first reads the interrupt status and faults on any TX error bit,
including TX underrun (bit 6)
([a733_i2s_stream.c:358](../../drivers/audio/a733_i2s_stream.c#L358)). With
TX enabled, the serializer underruns one frame (about 62 µs at 16 kHz) after
the FIFO empties, and the worker polls only once per millisecond. The
`publish()` step already checked errors after the last DMA completion, so an
underrun seen here can only mean the FIFO drained. The runtime provider model
([checks/i2s-runtime-provider.c:53](../../checks/i2s-runtime-provider.c#L53))
never sets the underrun bit.

**Failure scenario.** The worker sees FIFO empty and the underrun flag in the
same poll and calls `fault()`. A duplex RX peer, if present, also enters
RECOVERY because the two directions share control registers.

**Fix.** In the end-of-input FIFO stage, keep TX overrun (bit 5) fatal and
treat TX underrun as proof that the FIFO drained, still requiring a reading of
128 free entries before reporting INPUT_ENDED. Optionally derive the deadline
from the drain time (128 / (rate × channels)) plus `operation_us`. Update
[docs/i2s-stream.md](../i2s-stream.md), which says FIFO errors retain.

**Verification.** In `checks/i2s-runtime-provider.c` with `I2S_FILE_CHECK`,
set the underrun bit in the same read that first returns 128 free entries.
The current code returns RECOVERY; the fixed code reaches INPUT_ENDED.

**Review corrections.** The manual does not define when the underrun bit
latches, so the failure is likely rather than certain and must be confirmed on
hardware. Reading FIFO status before interrupt status is not required. The
10 ms deadline against an 8 ms worst-case drain is tight but not a defect on
its own.

### display-2: HDMI PHY lock wait needs two samples a tick apart within 2.5 ms

**Symptom.** HDMI preparation can fail with `HDMI_IO` when the TX PHY locks
within the vendor's 2.5 ms budget but later than about one tick.

**Root cause.** Stage 14 sets a 2,500 µs deadline taken from the vendor
driver's 500 × 5 µs busy loop
([a733_hdmi_phy.c:233](../../drivers/display/a733_hdmi_phy.c#L233)). Stage 15
must see the lock bit on two consecutive polls and faults once the deadline
passes ([lines 236-242](../../drivers/display/a733_hdmi_phy.c#L236)). The apps
poll once per tick ([apps/hdmi/output.c:82](../../apps/hdmi/output.c#L82)), so
only two or three samples fit. The vendor driver accepts one lock reading.

**Failure scenario.** The PHY locks 1.1 ms after stage 14. The first sample
sees it unlocked, the second sees it locked, and the third lands after the
deadline. The PHY, rails and clocks are retained. A delayed tick has the same
effect even with an early lock.

**Fix.** Bound the wait by a minimum number of samples and a tick-aware floor,
for example at least three samples and `max(2500 µs, 3 ticks)`, or take the
confirming second reading within the same poll after a 5 µs settle. Review
the 5 ms PHY-transaction bound for the same problem; the 10 ms DDC bound is
less exposed.

**Verification.** In the composed HDMI host check, model a 1 ms tick and
assert the lock bit from 1.2 ms after stage 14; the current code faults and
the fixed code reaches `A733_HDMI_TV`.

**Review corrections.** The first sample lands 0 to 1 ms after stage 14,
depending on tick phase, so the failure is nondeterministic. The claim that
every read takes the global register mutex was not verified and is not needed.

### video-2: registry BUSY during a codec register access fails the session

**Symptom.** A JPEG or H.264 decode or encode session can fail permanently
during programming because another task was briefly inside the clock or PCK
registry.

**Root cause.** Before each VE register access, `lock()` calls `held()`,
which maps registry BUSY to 0. The codec adapters treat any non-1 result as
`fail(p)` ([a733_video_platform.c:321](../../drivers/video/a733_video_platform.c#L321),
[line 436](../../drivers/video/a733_video_platform.c#L436),
[a733_video_encode_platform.c:322](../../drivers/video/a733_video_encode_platform.c#L322)),
and the state machines turn that into FAULT
([a733_video_h264.c:223](../../drivers/video/a733_video_h264.c#L223)). The USB
task enters the same registry every tick through `a733_usb_prepare`. `lock()`
also returns 0 when the VE mutex is contended, with the same result.
`a733_clock_token_status` holds the busy flag across a scan of up to 128
slots, which widens the window. [docs/video-platform.md](../video-platform.md)
documents conservative faulting; [docs/clock-leases.md](../clock-leases.md)
says the owner must poll against its own deadline.

**Failure scenario.** While H.264 High programs its roughly 435 frame-SRAM
and table words, the USB task on another core is inside the registry. One
`h264_write` fails and every DMA span, lease and the stream are retained until
power removal.

**Fix.** Add a bounded, deadline-checked retry inside `held()`/`lock()` for
both registry BUSY and mutex BUSY, keeping `fail()` for FAULT, INVALID and
timeout. Apply it once in a shared VE platform core. Update
[docs/video-platform.md](../video-platform.md). The same pattern in
`h264_iommu_admit` ([apps/h264/native.c:46](../../apps/h264/native.c#L46))
should change too.

**Verification.** In
[checks/a733-video-platform.c](../../checks/a733-video-platform.c) and
[checks/h264-encode-ownership.c](../../checks/h264-encode-ownership.c), make
the registry report BUSY once during PROGRAM and once during COPY; assert the
run reaches DONE with `failed` clear.

**Review corrections.** Lowered from high: a single access rarely collides,
and exposure builds over long streams. One reviewer kept high because one
collision retains the whole session.

### buses-pcie-1: PCIe configuration reads lack the post-reset readiness wait

**Symptom.** The first vendor-ID read can reach an endpoint that is not yet
ready, which faults and retains the PCIe port.

**Root cause.** The 100 ms wait runs before PERST is released and is measured
from PHY completion ([a733_pcie.c:337](../../drivers/pci/a733_pcie.c#L337)).
After `RESET_RELEASE` drives PD22 high, the state machine goes straight to
TRAIN, LINK and IDENTIFY. PCIe Base 6.6.1 requires 100 ms after reset before
the first configuration request at 5 GT/s or slower and tolerance of
Configuration Request Retry Status (CRS) for up to 1 s. `endpoint()` treats
0xFFFF as failure ([line 237](../../drivers/pci/a733_pcie.c#L237)), and every
failure becomes `fault()`. The cached U-Boot reference also waits 100 ms after
release, so [docs/pcie.md](../pcie.md) misstates the source ordering.

**Failure scenario.** An endpoint whose firmware takes longer than link
training to become configuration-ready returns 0xFFFF on the first read. The
port is retained until power removal.

**Fix.**

- Record a timestamp in `RESET_RELEASE` and hold off the first vendor-ID read
  until 100 ms have passed.
- Optionally enable CRS Software Visibility (Root Control bit 4) after
  checking that Root Capabilities advertises it. If it is enabled, treat
  0x0001 as not ready until about 1 s after release; `endpoint()` currently
  accepts 0x0001 as a vendor ID and must change first. Do not treat 0xFFFF as
  pending, because it also means no device.
- Make sure the operation deadline covers the readiness window.
- Correct [docs/pcie.md](../pcie.md).

**Verification.** In [checks/a733-pcie.c](../../checks/a733-pcie.c), assert
that no configuration read happens earlier than 100,000 µs of model time after
the PD22 write.

**Review corrections.** Lowered from high: PCIe has never run, the documented
endpoint is a DesignWare `pci-epf-test` device that usually prepares its
header before link training, and the failure is bounded. The AXI-error outcome
in the original scenario is speculation.

### supervisor-apps-7 and x-duplication-3: launchers are generated by unchecked text substitution

**Symptom.** A harmless edit to the shared supervised launcher can make many
applications generate launchers with the wrong deadline, magic or version, and
preparation still succeeds.

**Root cause.** Each board-derived application builds its SRAM launcher by
reading [launch.c](../../platform/a733-smp-supervised/launch.c) and chaining
Python `str.replace` calls for the request magic, request and contract
versions, session-size admission and the 20 s to 60 s deadline
([apps/monitor/prepare.py:55-57](../../apps/monitor/prepare.py#L55),
[apps/board/prepare.py:131-133](../../apps/board/prepare.py#L131)).
`str.replace` does nothing when its anchor is missing. A few scripts assert the
anchor (hevc, isp0, g2d, displayport, deinterlace, gpu-render, camera, mimus);
h264, h264-encode and jpeg-encode guard only some anchors; board, monitor, rtc,
spi, adc, crypto, uart, pcie, pcie-test, thermal, storage-read, jpeg and
bluetooth guard none. `launch.c` already supports `-D` overrides for the
contract version and session size, so two mechanisms coexist. The breakage
has happened once already (commit 235e52b, corrected by 6407e26).

**Failure scenario.** Reformatting `20*A733_SMP_COUNTER_HZ` as `20u*` leaves
the monitor launcher at 20 s. The image's contract admission
([apps/board/admission.c:93](../../apps/board/admission.c#L93)) refuses it at
boot, before device work, so the session fails closed and one scarce board
attempt is wasted.

**Fix.** Extend the existing `-D` overrides to the request magic, request
version and supervised seconds (reusing `A733_BOARD_SUPERVISED_SECONDS`), and
compile each launcher with flags. Until then, route every substitution
through one helper that asserts exactly one match, including the substitution
in [checks/board-supervised.py](../../checks/board-supervised.py). Keep the
generated `launch.bin` byte-identical, or regenerate the launcher-case
evidence bound to each `launch.bin` hash.

**Verification.** Build every launcher with the old and new paths and require
identical bytes; mutate one anchor in a scratch copy of `launch.c` and confirm
every generator raises.

**Review corrections.** The two findings rate this medium and low. Emulated
launcher cases with exact deadline assertions catch many misses before
hardware, and the monitor has no launcher-case producer, so it cannot reach
the board. The impact is wasted board attempts; execution stays safe.

### build-system-4: loaded images embed absolute host paths

**Symptom.** The same sources built in another worktree, build directory or
home directory produce different loaded bytes and a different ELF digest, and
the developer's home path ships inside board images.

**Root cause.** `configASSERT` passes `__FILE__` to `platform_panic`
([include/FreeRTOSConfig.h:55](../../include/FreeRTOSConfig.h#L55)), which
stores each translation unit's absolute path in `.rodata`. The paths include
the source cache, patched copies in the build directory, the worktree and the
mimus worktree. All flag sets use `-g` with no prefix map
([CMakeLists.txt:50](../../CMakeLists.txt#L50)). On A733 platforms
`platform_panic` discards the file argument, so the strings are never used.

**Failure scenario.** An image qualified in one checkout cannot be reproduced
bit for bit in another, and final-ELF evidence reuse fails.

**Fix.** Add `-ffile-prefix-map=<root>=<tag>` once, with top-level
`add_compile_options`, for the source root, `CMAKE_BINARY_DIR`, every
build-directory patch root and every fetched source tree (FreeRTOS, TinyUSB,
newlib, mimus, TCP and RSN). Keep `__FILE__` in assertions, because the
QEMU platform prints it.

**Verification.** Build `sunstone-a733-storage-maintenance` from two source
copies at different paths into two build directories; `strings` shows no
`/home` paths and the ELFs are identical.

**Review corrections.** `-g` changes only non-loaded debug sections and the
ELF digest; the loaded-byte difference comes from `__FILE__`. The
content-hashed build ID does not depend on paths. `__FILE_NAME__` is also
available in GCC 12 and later.

### host-tools-4: evidence dependency closure drops static libraries

**Symptom.** The dependency manifest that evidence snapshots record for an
image omits archive members and linker scripts, including the whole mimus ASR
library.

**Root cause.** `collect_dependencies` reads `link.txt` and keeps only tokens
ending in `.o` or `.obj`
([evidence-snapshot.py:41](../../tools/evidence-snapshot.py#L41)), although
its docstring promises every linked object. Archives such as `mimus_asr_smp`
([cmake/mimus-smp.cmake:12](../../cmake/mimus-smp.cmake#L12)), libc,
libstdc++ and libgcc, and the `-T` linker script, are dropped silently, so the
"incomplete coverage" guard never sees them.

**Failure scenario.** A change to a mimus recognizer source changes the
recognition ELF, but the frozen dependency manifest written by
[checks/i2s-application.py](../../checks/i2s-application.py) or
[checks/board-session-seal-recognition.py](../../checks/board-session-seal-recognition.py)
does not change. Reviewers cannot tell which recognizer sources the evidence
covers.

**Fix.** Resolve each in-tree CMake archive to its
`CMakeFiles/<lib>.dir` objects and their dependency records; hash external
archives and `-T`/`--defsym` inputs as opaque files; fail on unknown link
tokens, with an allow list for ordinary flags.

**Verification.** In a configured Bluetooth build with mimus enabled, run
`collect_dependencies` and assert the ASR sources, `link.ld` and the libc and
libstdc++ hashes appear.

**Review corrections.** Other mechanisms partly cover the gap: the build ID
hashes the ASR inputs, the recognition seal pins earlier mimus and newlib
files, and some checks capture `*.ld` files separately. `checks/bluetooth-application.py`
does not build the mimus-enabled image, so the clearer examples are the two
checks cited above. Whether reuse checks accept stale evidence was not shown.

### checks-harness-1: UBSan recovers, so checks pass with undefined behavior

**Symptom.** About 100 host checks built with `-fsanitize=address,undefined`
report a pass when the code under test executes undefined behavior.

**Root cause.** No check passes `-fno-sanitize-recover` or sets
`UBSAN_OPTIONS=halt_on_error=1`. With both GCC and Clang defaults, a UBSan
diagnostic prints `runtime error:` and the program exits 0. Several runners
set `ASAN_OPTIONS=halt_on_error=1`
([checks/board-radio.py:62](../../checks/board-radio.py#L62)), which does not
affect UBSan. A one-line signed overflow built this way printed the diagnostic
and exited 0. Only `h264-high-bounds.py`, `gpu-display-preparation.py` and
`h264-sequence-oracle.py` inspect stderr.

**Failure scenario.** A driver change introduces signed overflow in a DMA
length or an over-wide shift in a register helper. The check logs the
diagnostic and records a pass.

**Fix.** Add `-fno-sanitize-recover=all` wherever `undefined` appears in a
sanitizer list, through one shared flag constant; set
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` as a backstop. Rerun the
affected checks once and grep existing evidence logs for `runtime error:`.

**Verification.** A one-line UB program built with the shared flags exits
nonzero; `rg` finds no `-fsanitize=...undefined` without the recover flag;
[checks/rail-leases.py](../../checks/rail-leases.py) and
[checks/i2s-reference.py](../../checks/i2s-reference.py) still pass.

**Review corrections.** Lowered from high: no undefined behavior was found in
project code.

### x-duplication-2 and build-system-5: build-time patches apply with default fuzz

**Symptom.** A configure-time source patch can apply at the wrong place or
with altered context without failing the build.

**Root cause.** `apply_build_patch` runs `patch --silent --output` with no
`--fuzz=0` ([CMakeLists.txt:19](../../CMakeLists.txt#L19)). GNU patch then
accepts hunks with up to two mismatched context lines and with offsets, and
`--silent` hides the warning. The function is redefined in about 31
CMakeLists files and applied at about 152 sites, and patches chain several
levels deep. Five single-core `main.patch` files already apply with fuzz 2 at
an offset of 26 lines against [apps/board/main.c](../../apps/board/main.c).
Those patches insert `msr daifset,#15` interrupt masking before the native USB
bind, the radio startup and the scheduler start. Base-file hash guards exist
only in the storage-maintenance, smp-supervised and smp-observe fragments.

**Failure scenario.** An edit to an unguarded intermediate patch such as
[apps/board/boot.patch](../../apps/board/boot.patch), or to a base near
repeated context, lets a downstream hunk land elsewhere. The image builds with
startup code no one reviewed, for example leaving interrupts unmasked during
single-core startup.

**Fix.** Refresh the drifted patches first. Then define `apply_build_patch`
once in a shared `cmake/` module, include it from every standalone project,
and add `--fuzz=0 --batch`. Offsets with exact context may stay allowed. The
same applies to `tools/run-native-startup-probe.py`. Moving variant
differences into configuration is a separate design decision.

**Verification.** Configure every profile and standalone project with the
shared helper and confirm all patches apply; insert a blank line into a hunk's
context in a scratch copy and confirm configuration fails.

**Review corrections.** The two findings rate this medium and low. The
original scenario (an edit to `platform/a733-smp-cached/boot.c`) is caught by
the existing hash guards; the real exposure is unguarded intermediate patches
and bases. Not every `.patch` file goes through `apply_build_patch`.

## Low severity

### cpu-startup-1: ROM-return recovery reads a loop bound from EL1-writable memory

**Symptom.** If EL1 corrupts the monitor's saved GIC state, the recovery path
that returns the board to FEL can write far outside the distributor's enable
registers.

**Root cause.** The monitor keeps its GIC snapshot in `.monitor_state`
([monitor.c:3](../../platform/a733/monitor.c#L3)), whose comment says EL1
never writes it. The section lies inside the image block that EL1 maps
read-write ([platform.c:71-73](../../platform/a733/platform.c#L71),
[link.ld:15](../../platform/a733/link.ld#L15)), a limitation documented in
[docs/a733-kernel.md](../a733-kernel.md). `finish()` uses `saved.banks` as a
loop bound without checking it against the 32-entry array
([monitor.c:127](../../platform/a733/monitor.c#L127)). Separately,
`platform_puts` stores the terminator at `log[size]` even when
`RESULT->log_length` is already past the buffer
([platform.c:19](../../platform/a733/platform.c#L19)).

**Failure scenario.** A wild EL1 store sets `saved.banks` to a very large
value. Because `4 * i` wraps in 32 bits, the restore loop can walk the
physical address space, including the monitor's own code and stack. The RMR
warm reset does not reset the GIC. A corrupt `log_length` makes
`platform_puts` write one zero byte to an arbitrary mapped address.

**Fix.** Clamp the restore loop to `min(saved.banks, 32)`. Return from
`platform_puts` before any store when `size >= RESULT_SIZE - LOG_OFFSET - 1`.
Reword the comment to say EL1 is trusted not to write the section. The durable
fix is to place the monitor image, state and stack in a region EL1 cannot
write; the SMP layouts point in that direction, but this audit did not confirm
their EL1 tables deny the writes.

**Verification.** In the monitor instruction model, corrupt `saved.banks` and
`log_length` and assert that MMIO writes stay within GICD+0x100 to 0x17c and
stores stay within the result block.

**Review corrections.** Lowered to low: it needs an earlier wild store into
about 168 specific bytes, and the monitor runs with caches off, so the
corruption is visible only once the dirty line reaches DRAM. The proposed
checksum on `saved` was dropped because the rest of the monitor sits in the
same writable mapping.

### cpu-startup-5: monitor cleanup never clears SPIs enabled during the run

**Symptom.** After a forced FEL return, an SPI that EL1 enabled during the run
would stay enabled.

**Root cause.** `finish()` restores the saved SPI masks through ISENABLER,
which only sets bits ([monitor.c:127-128](../../platform/a733/monitor.c#L127));
it clears PPIs first but not SPIs. `GICR_WAKER.ProcessorSleep` is cleared at
init ([line 62](../../platform/a733/monitor.c#L62)) and not restored, while
[docs/a733-kernel.md](../a733-kernel.md) says every modified redistributor
setting is restored.

**Failure scenario.** No current image that uses this monitor enables an SPI,
and interrupt-driven consumers use their own monitors with owner restore hooks
(for example `npu_irq_route_restore` in `apps/npu-finite/monitor.c`). The
hazard applies only if an interrupt-driven driver is later run on this
monitor.

**Fix.** In `finish()`, write ICENABLER all-ones for SPI banks 1 to
`banks-1` and wait for RWP with the existing bounded deadline before the
ISENABLER restore. Change the document to say WAKER is left awake on purpose.

**Verification.** In the monitor model, enable one SPI from EL1, trigger
`finish` through SMC 0x733, and assert the final enable words equal the
snapshot.

**Review corrections.** Lowered from medium. Restoring SPI group, priority and
routing, and restoring ProcessorSleep, were dropped as having no consumer.

### clock-power-pins-1: TWI11 shares the PMIC's I2C lock

**Symptom.** In the camera image, a sensor NACK on TWI11 retains the lock
that the PMIC on S_TWI0 uses, so later PMIC polls are refused.

**Root cause.** The native TWI11 adapter takes and retains
`A733_PLATFORM_I2C_LOCK`
([a733_twi_native.c:80](../../drivers/i2c/a733_twi_native.c#L80)), although
[a733_platform_locks.h](../../include/a733_platform_locks.h) assigns that lock
to the PMIC and S_TWI0 and gives S_TWI1 its own. TWI11 is a separate
controller.

**Failure scenario.** With no camera module connected, the address phase is
not acknowledged, the transaction faults and retains the I2C lock. The camera
has already finished its PMIC snapshots, so the effect is a blocked PMIC after
an already terminal camera failure.

**Fix.** Add `A733_PLATFORM_I2C11_LOCK` and use it in the adapter's enter,
leave, held and access paths. Update the "common I2C mutex" wording in
[docs/camera-native.md](../camera-native.md). Rebuild every embedder of
`struct a733_platform_locks` and update checks that assume three locks, such
as [checks/a733-pmic-consumers.c](../../checks/a733-pmic-consumers.c) and
[checks/a733-typec.c](../../checks/a733-typec.c). This does not keep the USB
evidence channel alive, because `native_usb.c` refuses on any retained bit;
that needs a separate change.

**Verification.** Inject a NACK on a TWI11 transaction and assert that
`a733_pmic_poll` still reaches READY and only the new lock bit is retained.

**Review corrections.** The original claims of NPU and GPU impact and of a
required power cycle were dropped.

### clock-power-pins-7 and x-concurrency-3: counter clock faults after 256 contended attempts

**Symptom.** If one caller loses 256 consecutive lock attempts, the shared
counter clock latches a permanent fault and every client that uses it
retains.

**Root cause.** `a733_counter_clock_now` masks interrupts and spins on a CAS
for at most 256 attempts with a `yield` hint, then sets `failed`
([counter_clock.c:20-42](../../platform/a733/counter_clock.c#L20)). The
header documents permanent faulting on bounded-lock failure. The bound counts
attempts, not time; A76 cores spin faster than A55 cores, and the LL/SC
exclusive monitor is not fair. Running out of attempts is not evidence that
the time source is wrong.

**Failure scenario.** At this commit every image that links this clock either
runs one core or runs only a few provider tasks separated by tick delays, so
the failure is latent. It becomes reachable when an image with several busy
pollers on different cores links the clock.

**Fix.** Keep the serialized sample history, which
[docs/dma-startup.md](../dma-startup.md) relies on to detect cross-core
counter regressions, and bound the wait by elapsed counter ticks (tens of
microseconds) instead of attempts; a fair ticket lock is optional. Place
`last_ticks` on a different cache line from the lock word. Update the header
and the document. A lock-free design is acceptable only if it still detects
cross-core regression, for example by re-reading the counter after an ISB when
a newer published value is seen.

**Verification.** A host pthread stress test with 8 to 16 threads and an
injected holder delay never latches `failed` on monotonic input and does latch
on an injected backward value or frequency change.

**Review corrections.** The original recommendation (lock-free with a
per-core regression check) drops a documented guarantee and was replaced. The
EL3-stall trigger and the camera example were not supported by the code.

### bluetooth-8: AT+BAC during an open audio link faults the SCO object

**Symptom.** A headset that sends AT+BAC while an audio link is open ends
capture and the controller lifetime.

**Root cause.** Accepting AT+BAC clears the selected codec
([bt_hfp_ag.c:100](../../drivers/bluetooth/bt_hfp_ag.c#L100)). SCO checks
every open link against `ag->selected_codec` on each step and faults on a
mismatch ([bt_sco.c:74](../../drivers/bluetooth/bt_sco.c#L74)). HFP allows
the headset to resend AT+BAC at any time; even an identical resend triggers
the fault.

**Fix.** In the BAC handler, keep the selected codec when the new list still
contains it, and clear it only when the codec was dropped (a dropped in-use
codec may still fault or close the link). Make `bt_sco.c:74` fault only when
the AG holds a different nonzero codec. The headset AG stage already reselects
before the next open or CVSD retry.

**Verification.** In the mSBC build of the headset check, inject
`AT+BAC=1,2` during capture and assert capture completes.

### audio-4: finite TX fails if only two periods are committed before START

**Symptom.** A finite TX stream with three or more periods stops as FAILED at
the boundary that reaches the first uncommitted slot.

**Root cause.** START requires only slots 0 and 1 to be queued
([a733_i2s_stream.c:231](../../drivers/audio/a733_i2s_stream.c#L231)), and
[docs/i2s-stream.md](../i2s-stream.md) says two committed periods suffice. GET
hands out free slots only while the stream is READY, and `arm()` requires the
next slot to be queued ([line 205](../../drivers/audio/a733_i2s_stream.c#L205)).

**Failure scenario.** No in-tree client is affected: every finite TX client
primes all four periods before START. A future client that follows the
document fails after two periods.

**Fix.** Make START require every catalogue slot, or every slot up to the END
slot, to be queued, as the cyclic path does, and correct the document and
header.

**Verification.** A four-period TX case in
[checks/i2s-stream.c](../../checks/i2s-stream.c) that commits two periods is
refused at START.

**Review corrections.** Lowered from medium. Letting GET issue free slots
while active was dropped as having no consumer.

### audio-12: cancelling a never-started stream enables the I2S clocks

**Symptom.** Cancelling a configured but never-started I2S stream briefly
starts the master bit and frame clocks on PB5 and PB6.

**Root cause.** Cancel accepts READY
([a733_i2s_stream.c:414](../../drivers/audio/a733_i2s_stream.c#L414)) and
calls `stop()`, which calls `control(s,1)` and forces the global enable on
([line 57](../../drivers/audio/a733_i2s_stream.c#L57),
[line 177](../../drivers/audio/a733_i2s_stream.c#L177)). The STOP poll clears
it again.

**Failure scenario.** An application that opens and cancels without starting
sends a clock burst to the codec. The effect is limited: START already enables
the clocks during priming, and Linux keeps the global enable on while resumed.

**Fix.** On a READY cancel, skip the control write (no DMA is owned and no
request is enabled), and check that a stream cancelled from READY can still be
closed.

**Verification.** A new case in `checks/i2s-stream.c` cancels in READY and
asserts the model never sees the global enable written as 1.

### camera-4: camera planes carry no fill pattern

**Symptom.** In the retained capture path, a region the DMA never wrote would
be published as part of the frame, with leftover DRAM contents.

**Root cause.** The native binding allocates planes and guard pages from the
device pool and never fills them
([a733_camera_native.c:374-377](../../drivers/camera/a733_camera_native.c#L374)).
The ISP0 application fills its extents with 0xa5 and re-checks them
([apps/isp0/memory.c:27](../../apps/isp0/memory.c#L27)).

**Failure scenario.** The DMA status check against `NORMAL=0xa3` already
faults on line-miss, pixel-miss and buffer-overflow bits, so a short frame is
caught if the hardware reports it. The remaining risk is hardware that signals
CAPTURE_DONE without raising those bits.

**Fix.** Fill payload and guard pages with 0xa5 before the allocation is
marked device-owned at line 377. Read the DMA line counter (0x258) and require
480 lines. A guard-page check belongs in the drain path, because
[docs/camera-dma.md](../camera-dma.md) limits retained access to the payload.
Do not reject a frame for having uniform first or last lines; the native
profile captures the live sensor, so that heuristic can reject valid images.

**Verification.** In [checks/a733-camera-native.c](../../checks/a733-camera-native.c),
model a 400-line DMA write and assert a fault before READABLE.

**Review corrections.** Lowered from medium; the original claim that a short
frame passes every check ignored the DMA error bits.

### camera-6: sensor-stop errors are not separated from captured-frame errors

**Symptom.** A late sensor stop could latch a CSI packet error that faults a
capture whose frame was already complete.

**Root cause.** Stop writes 0x300e (MIPI off) before 0x4202 (frame gate)
([ov5640.c:254-255](../../drivers/camera/ov5640.c#L254)), matching the Linux
reference driver. The retained path then samples CSI status, where any error
bit faults ([a733_csi_receiver.c:205](../../drivers/camera/a733_csi_receiver.c#L205)).

**Failure scenario.** A stall of 15 ms or more after CAPTURE_DONE lets stop
land during the next frame. Whether that latches an error is an unverified
hardware assumption.

**Fix.** Sample and clear CSI port and parser status once after CAPTURE_DONE
and before the sensor stop, so errors on the captured frame are judged
separately from stop-transition artifacts. Do not reorder the stop writes
without datasheet or hardware evidence. Reconcile
[docs/camera-capture.md](../camera-capture.md), which requires a
packet-quiescence step that the retained path skips.

**Verification.** Assert the new sample order in the capture model and that
the final register image in
[checks/ov5640-reference.py](../../checks/ov5640-reference.py) is unchanged.

**Review corrections.** The original fix (reorder and wait 33 ms) diverges
from the reference driver on unverified semantics and was replaced. Skipping
the quiet step is not part of the cause, because the error bits are latched.

### gpu-3: informational GPU firmware events fail the session

**Symptom.** If the GPU firmware emits UPDATE_STATS (107), DEBUG_DUMP (106) or
CORE_CLK_RATE_CHANGE (108), the GPU service latches FAILED and retains its
leases.

**Root cause.** The service sends only events 103 and 104 to the typed event
dispatcher and passes the rest to the backend
([pvr_gpu_service.c:177](../../drivers/gpu/pvr_gpu_service.c#L177)), which
fails unless the application supplies an event handler
([gpu_backend.c:349](../../platform/a733/gpu_backend.c#L349)); none does. Linux
logs unknown firmware events and continues. The fail-closed behavior is
documented in [docs/gpu-control.md](../gpu-control.md) as pending work.

**Failure scenario.** UPDATE_STATS after an out-of-memory partial render ends
the render session. Emission has not been observed; 108 needs firmware DVFS,
which is disabled.

**Fix.** Route UPDATE_STATS through the event dispatcher with a counting
handler and a registered process resource whose ID matches the firmware's
(effectively 0). Cover the path where the service has no free lists. Keep 106
diagnostic-only and 108 fatal. Update the GPU documents. Gather evidence that
the firmware emits 107 first.

**Verification.** Host service cases inject 107 during compute and render
jobs and assert the session stays READY; 105 and an unexpected 108 still fault.

**Review corrections.** Recategorized as a bring-up gap; the title "every
session" was narrowed to sessions that receive such an event.

### security-misc-2: thermal and ADC drivers retain on an expired caller deadline

**Symptom.** A read or stop called with a deadline that has already passed
retains the driver, and a GPADC or LRADC read before READY retains instead of
returning INVALID.

**Root cause.** `read()` and `stop()` store the deadline and call `lock()`,
which faults when it is no longer timely
([a733_thermal.c:171](../../drivers/thermal/a733_thermal.c#L171)). GPADC and
LRADC `read()` return `fault(p)` for `!deadline` or `stage!=ACTIVE`
([a733_adc.c:161](../../drivers/adc/a733_adc.c#L161),
[a733_lradc.c:168](../../drivers/adc/a733_lradc.c#L168)), while their own
`stop()` returns INVALID. The crypto engine returns INVALID for an expired
deadline.

**Failure scenario.** None today: every caller passes a session-wide deadline
and polls to READY before reading. Expired-deadline retention is asserted by
[checks/lradc-core.c](../../checks/lradc-core.c) and documented in
[docs/thermal.md](../thermal.md).

**Fix.** Make GPADC and LRADC `read()` return INVALID for `!deadline` and
`stage!=ACTIVE`, matching their `stop()`, while keeping `fault()` for a failed
`current()` check. Changing expired-deadline handling to INVALID changes a
documented contract and needs an owner decision.

**Review corrections.** Lowered from medium. One reviewer judged the
expired-deadline part not a defect.

### security-misc-3: LRADC read faults on a conversion that lands mid-acknowledge

**Symptom.** An LRADC read can fault permanently if the next conversion
completes between the W1C acknowledge and the readback.

**Root cause.** `read()` acknowledges DATA0 and requires the pending register
to read zero ([a733_lradc.c:85-91](../../drivers/adc/a733_lradc.c#L85)),
while the converter runs continuously at roughly a 31 ms period.

**Failure scenario.** The only consumer reads two samples and stops, so the
exposure is about 1e-4 per run; a long-running consumer would hit it.

**Fix.** On a nonzero readback showing only DATA0, repeat the acknowledge and
readback once. A second nonzero readback still faults, because two conversions
cannot complete within microseconds. Simply accepting the reasserted bit would
remove the only proof the acknowledge worked. Document the tolerance in
[docs/adc-application.md](../adc-application.md).

**Verification.** In `checks/lradc-core.c`, reassert DATA0 between the
acknowledge and readback and assert a sample, not retention.

### security-misc-4: watchdog compares RTC control including write-busy bits

**Symptom.** A maintenance-watchdog reload that coincides with an RTC calendar
write fails its clock-path check.

**Root cause.** `same()` compares the whole RTC control word
([maintenance_watchdog.c:35](../../platform/a733/maintenance_watchdog.c#L35)),
including bits 8:7, the calendar write-busy flags. After the kick, a mismatch
revokes the feed and retains the register lock, which leads to a watchdog
reset ([lines 82-85](../../platform/a733/maintenance_watchdog.c#L82)).

**Failure scenario.** No current image combines an RTC calendar writer with a
watchdog feeder; the hazard applies to a future persistent image.

**Fix.** Mask only bits 8:7, `(read32(0x07090000) ^ t->rtc_control) &
~0x180`, also when the value is captured, and document the mask beside the
`rtc_control` field. The driver's 0xc01f mask would drop bits the watchdog
is meant to protect. Alternatively, document that watchdog images exclude
calendar writers.

**Review corrections.** The busy bits last as long as the hardware busy time
(bounded by a 1 s poll); the 2 ms in the original text is a settle delay after
the clear.

### security-misc-5: RTC alarm arm retains on a benign second rollover

**Symptom.** Arming a one-second alarm occasionally retains the RTC.

**Root cause.** `a733_rtc_arm` computes `target = now + delay`, writes and
reads back the alarm registers, re-reads the calendar and calls `fail()` if
the target has already been reached
([a733_rtc.c:114](../../drivers/rtc/a733_rtc.c#L114)), before writing the
enable. Nothing is uncertain at that point. Both callers treat any 0 as
retention ([service.c:229](../../apps/rtc/service.c#L229),
[native.c:65](../../apps/rtc/native.c#L65)).

**Failure scenario.** The RTC service is a finite diagnostic; with
`alarm_seconds=1` a run occasionally ends with a spurious RETAINED outcome.

**Fix.** Require `delay >= 2` in admission
([apps/rtc/admission.c:16](../../apps/rtc/admission.c#L16)) and the driver,
and update [docs/rtc-application.md](../rtc-application.md). A distinct
refusal code with a verified register restore also works but adds steps that
can fail.

### buses-pcie-8: UART interrupts stay off after a DMA lease is released

**Symptom.** After releasing a UART DMA lease, interrupt-driven receive never
fires again on the same open controller.

**Root cause.** `dma_prepare` writes IER=0
([a733_uart.c:240](../../drivers/uart/a733_uart.c#L240)); `dma_release`
returns READY without restoring it, and `tx_irq` changes only bit 1. The
header implies service resumes after release. Polled `a733_uart_exchange`
still works.

**Failure scenario.** No current consumer: the UART DMA application closes
right after release.

**Fix.** Make release terminal (only drain and close afterwards; exchange,
interrupt and `tx_irq` return -1) and document it in
[a733_uart.h](../../include/a733_uart.h).

**Verification.** A case in [checks/uart-dma-core.c](../../checks/uart-dma-core.c)
releases the lease and asserts the refusal.

### supervisor-apps-13: radio profile hardcodes a 60 s session

**Symptom.** The radio profile duplicates the session lifetime as literals.

**Root cause.** `board_radio_profile_runtime` requires a 60,000,000 µs
deadline and derives 40 s and 50 s bounds
([radio_profile.c:12-19](../../apps/board/radio_profile.c#L12)), while the
lifetime is the macro `A733_BOARD_SUPERVISED_SECONDS`. `apps/bluetooth/bluetooth.c`
holds a fourth copy.

**Failure scenario.** No image combines a non-60 s lifetime with a radio
workload; all 20 s images admit only workload 2. This is a maintenance hazard.

**Fix.** Derive the check from the macro. Do not add an unconditional
`_Static_assert`, because the 20 s g2d, displayport, deinterlace and isp0
images compile this file; guard it with `#if
A733_BOARD_SUPERVISED_SECONDS==60` or compile the file only when radio is
admitted. In `apps/monitor/prepare.py`, substitute the macro as
`apps/board/prepare.py` does.

### build-system-3: build identity omits headers and compile flags

**Symptom.** Different binaries can share the build ID that board admission
compares.

**Root cause.** The ID hashes a hand-kept input list
([a733-board.cmake:133](../../cmake/a733-board.cmake#L133)). It misses
`network/include/FreeRTOSIPConfig.h`, `drivers/usb/*.h` and
`drivers/usb/host/tusb_config.h`, and represents flags only as literals in the
storage and smp-cached identities. Options such as `SUNSTONE_BOARD_USB2_IRQ`
also change the binary without changing the ID.

**Failure scenario.** Editing `FreeRTOSIPConfig.h` rebuilds an image with the
old ID and no reconfigure. Host tools also check the ELF hash, so admission of
the wrong binary through the documented flow is prevented; the build ID alone
is a misleading identity.

**Fix.** Add the missing headers to the input lists and hash the real flags,
target options, definitions and option values, as the single-core profiles
already do.

**Verification.** Editing `FreeRTOSIPConfig.h`, or adding a C flag, changes
`build-id.txt`.

### build-system-10: profile fragments force the supervised mode into the cache

**Symptom.** Configuring a supervised-only profile without the documented
`-DSUNSTONE_BOARD_SUPERVISED=ON` fails once and passes on rerun, and the
default board target then stays supervised after the profile is turned off.

**Root cause.** About 20 fragments write the option with `CACHE ... FORCE`
after the board target exists ([a733-thermal.cmake:5](../../cmake/a733-thermal.cmake#L5)),
and [CMakeLists.txt:3-10](../../CMakeLists.txt#L3) forces it for PWM and
storage-read even over an explicit OFF.

**Fix.** Delete every forced cache write, keep each fragment's existing guard
with a message that names the option, and add the same guard for PWM and
storage-read with matching document updates.

**Verification.** A fresh configure with a profile and the option succeeds in
one pass; turning the profile off removes the supervised definition.

### build-system-15: `fetch-sources.py` trusts partial extractions

**Symptom.** An interrupted extraction leaves a partial source tree that later
runs never repair.

**Root cause.** The script checks the archive hash but skips extraction when
the directory exists ([fetch-sources.py:32](../../tools/fetch-sources.py#L32)).
Download temporary names come from `with_suffix`, so about 50 `.c`/`.h` pairs
share one name.

**Fix.** Extract into a temporary sibling directory and rename it into place;
name downloads `archive.name + f".{pid}.download"` and remove them in
`finally`.

**Review corrections.** No caller runs fetches concurrently, and the toolchain
re-download in the original text is required because its directory is also
absent.

### host-tools-3: mimus-board admits more recognitions than 60 s allows

**Symptom.** The host accepts `--generations` up to 32, but only a few
recognitions fit in the 60 s native session.

**Root cause.** `MAX_GENERATIONS = 32`
([check-a733-mimus-board-supervised.py:31](../../tools/check-a733-mimus-board-supervised.py#L31));
the 59 s capture window follows the native session lifetime documented in
[apps/mimus-board/README.md](../../apps/mimus-board/README.md). An incomplete
count fails and keeps earlier results, as documented.

**Fix.** Lower `MAX_GENERATIONS`, or state in the README and CLI help how many
generations fit at current speed. Physical recognition time has not been
measured, so a computed budget is premature.

### host-tools-8: device-identity read races the expected FEL disconnect

**Symptom.** During the expected disconnect after `exe`, `observe()` can raise
`FileNotFoundError` and record the run as failed.

**Root cause.** `session_node()` maps ENOENT and ENODEV to absent
([usb-access.py:158](../../tools/usb-access.py#L158)), but callers then call
`access.identity(candidate)` again without that mapping
([check-a733-smp-supervised.py:346](../../tools/check-a733-smp-supervised.py#L346),
[a733-returning-host.py:50](../../tools/a733-returning-host.py#L50), and about
a dozen other tools).

**Failure scenario.** In the supervised path the board is held after every
outcome anyway, so the race loses the observation of a possibly valid result.
In `a733-returning-host.wait_return`, it turns an automatic ROM return into a
retained failure.

**Fix.** Make `session_node` return the node and its identity from one
`lstat`, and update every caller.

**Verification.** A fake access module whose `identity()` raises on the
second call; `observe` treats the candidate as absent.

### host-tools-9: dashboard keeps hand-copied physical-result anchors

**Symptom.** The progress dashboard decides "board tested" by matching
hardcoded text from [docs/hardware.md](../hardware.md); the DDR anchor no
longer matches anything, and RTC has no anchor.

**Root cause.** `PHYSICAL_SCOPES` in [progress.py:51-62](../../tools/progress.py#L51)
is a second copy of status text, which AGENTS.md forbids. DDR and RTC still
show as board tested through aliased bullets, so nothing is misreported today.

**Fix.** Warn or fail when an anchor matches no bullet, or use an explicit
marker in `hardware.md`; add a check in
[checks/progress.py](../../checks/progress.py) against the live matrix.

**Review corrections.** The original symptom (DDR and RTC shown as host-only)
is wrong.

### host-tools-10: board lock contention reports a raw errno

**Symptom.** When another process holds the port lock, runners report
`Stopped: [Errno 11] Resource temporarily unavailable` without naming the port
or lock file.

**Root cause.** `board_lock` takes a non-blocking `flock` and does not catch
`BlockingIOError` ([usb-access.py:40-41](../../tools/usb-access.py#L40)).

**Fix.** Catch `BlockingIOError` and raise `ValueError` naming the port and
lock path. Handling of cross-user lock files is optional, because no
documented flow runs runners as root.

### checks-harness-4: negative cases accept any exception

**Symptom.** A refusal case passes when any exception is raised, including a
`TypeError` from a regression.

**Root cause.** The H.264 and JPEG cases use `except Exception: pass`
([h264-application-cases.py:214-218](../../checks/h264-application-cases.py#L214)),
and other `refused()` helpers accept broad exception sets. Each case validates
the unmodified baseline first and changes one field, so deleting the targeted
check still makes the case fail.

**Fix.** Catch `ValueError` and assert a message substring per case, following
[checks/gpu-display-preparation.py](../../checks/gpu-display-preparation.py).
Keep the `OSError` catch where an injected durable-write failure needs it.

### checks-harness-12: resumed drivers overwrite failed-attempt records

**Symptom.** Rerunning a failed `--run-only` step loses the earlier failure
log and truncates the command record.

**Root cause.** [rtc-runtime.py:57-64](../../checks/rtc-runtime.py#L57),
`rtc-calendar.py`, `rtc-application.py` and `board-session-seal.py` reset
`commands` and write `<label>.log` unconditionally, while
[rtc-service.py](../../checks/rtc-service.py) appends and numbers attempts.

**Fix.** Load the existing commands file and number logs by attempt in the four
affected drivers, or move `rtc-service.py`'s semantics into a shared helper.

**Review corrections.** `a733-storage-maintenance.py` uses a fresh directory
per run and is unaffected.

### checks-harness-14: check outcomes rely on Python `assert`

**Symptom.** Running checks with `PYTHONOPTIMIZE=1` or `python3 -O` removes
every `assert`, including source-pin and build-result checks, and the checks
pass.

**Root cause.** Thousands of `assert` statements carry pass/fail decisions,
for example [sunxi-mmc.py:25](../../checks/sunxi-mmc.py#L25), and no script
refuses optimized mode. There is no shared module that every check imports.

**Fix.** Replace the pin and return-code asserts with explicit checks that
raise, and add `if not __debug__: raise SystemExit(...)` to each check or to
the runners.

### x-concurrency-4: pcie-test ISR faults on a legal level re-delivery

**Symptom.** The PCIe interrupt test can report failure although the device
behaved correctly.

**Root cause.** `gicv3_spi_mask()` does not wait for the distributor to apply
the disable ([gicv3_spi.h](../../include/gicv3_spi.h)), and the IRQ handler
signals end of interrupt right after the ISR returns. INTID 185 is level
triggered and stays asserted until the owner acknowledges the MSI. The ISR
faults on any second delivery ([apps/pcie-test/irq.c:27-34](../../apps/pcie-test/irq.c#L27)),
and the client also retains on a count above one
([a733_pcie_epf_test.c:163](../../drivers/pci/a733_pcie_epf_test.c#L163)).

**Fix.** In the ISR, treat a delivery while OBSERVED as benign: re-issue the
mask, set no fault and leave the event count unchanged. Keep faulting before
ARMED, after CLOSED or on a failed mask. Update
[checks/pcie-root-irq.c:45](../../checks/pcie-root-irq.c#L45) and the
duplicate-MSI wording in
[docs/pcie-test-application.md](../pcie-test-application.md). Do not add an
RWP spin in the ISR; [docs/gpu-interrupts.md](../gpu-interrupts.md) rules it
out.

**Review corrections.** Lowered from medium: the window is short and PCIe
cannot run without an identified endpoint.

### x-concurrency-9: single-core `platform_puts` is unsynchronized

**Symptom.** In the single-core mimus USB image, a USB mount or unmount
message during paced replay can overwrite part of a transcript line in the
result log.

**Root cause.** `platform_puts` reads `log_length`, copies and writes the
length back with no critical section
([platform.c:13-21](../../platform/a733/platform.c#L13)); the USB task logs
from mount callbacks ([diagnostic.c:88](../../drivers/usb/diagnostic.c#L88))
while the recognition task logs results. The SMP mimus runtime already uses a
critical section.

**Fix.** When `xTaskGetSchedulerState() == taskSCHEDULER_RUNNING` and not in
an interrupt, suspend the scheduler around the append; keep the raw append for
panic, exception and pre-scheduler callers. Keep truncation on a full log.

**Verification.** A host model interleaving two writers at every byte
boundary yields whole messages and a non-decreasing length.

**Review corrections.** The host reads the log only after the run is Done or
Failed through the mimus commands, so it cannot see the length move backward
there.
