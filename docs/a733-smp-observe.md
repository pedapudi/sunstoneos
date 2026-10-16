# Cached SMP results over USB

The separate `a733-smp-observe` image sends an immutable eight-core diagnostic
result over USB before the independent watchdog resets the board. It combines
the [cached CPU diagnostic](a733-smp-cached.md) with a bounded USB0 programmed-I/O
service. Native compilation, result encoding and focused instruction checks
pass. Physical enumeration, coherent execution and independent recovery remain
unqualified.

The host must receive and durably save a complete matching result while this
image is running. SRAM and DRAM contents after whole-system reset cannot serve
as the result channel. A later ROM observation records recovery separately.
Missing or partial output leaves scheduler completion unobserved.

## Task ownership and privilege

The existing eight-core cache-adoption and shared-atomic barriers finish before
the primary creates the USB task. That task has priority four and affinity to
core zero. The original sixteen floating-point and background tasks retain
their priorities and checks. The primary freezes a result only after all eight
workers complete 32 rounds, show background and SGI progress, and have no failure
record. At least 32 primary timer ticks are required. Completed workers suspend;
the USB task remains available until the original diagnostic deadline.

All tasks remain at EL0. A target-private vector patch sends SVC 120 to one fixed
EL1 service entry. The entry authenticates core zero, the exact return address,
the USB task's current TCB, its reserved aligned stack, the saved EL0 state and
masked privileged IRQ state. It accepts no caller pointers or register-selected
operation. An invalid caller enters the existing non-returning fault monitor.

The handler uses the trusted EL1 stack and preserves x0–x18, x30, ELR and SPSR.
The C calling convention preserves x19–x29. Every reachable C source, including
TinyUSB, compiles with `-mgeneral-regs-only`; the service cannot overwrite task FP
registers. IRQ remains masked during the service call. ERET restores the task's
original interrupt masks and condition flags. The existing branch-history
workaround runs before the new branch.

Each call handles at most four TinyUSB events and one bounded request or response
step. It does not call blocking scheduler APIs. Initial USB preparation includes
150 ms detach and 1 ms reset delays using the physical counter.
These delays run with IRQ masked and consume the existing deadline. Pending
timer interrupts resume afterward. Physical enumeration and host capture within
the four-second watchdog interval still require qualification. The image never
feeds, disarms, extends or explicitly resets that watchdog.

## Memory publication

A private boot patch copies the admitted epoch, generation, lease and absolute
times into ordinary RAM before the existing cache publication. It runs after
contract and watchdog readback admission. EL1 never reads the Secure contract or
monitor reservation.

Result data, USB FIFOs and task storage use the existing shared WBWA mappings.
The primary publishes the completed snapshot with a release store; the service
uses an acquire load before copying it. All transmitted responses have private
storage and remain unchanged until transfer completion. The USB driver uses PIO,
so no new DMA ownership or cache-maintenance contract is introduced. The
instruction model verifies publication instructions and ordering in shared
memory; it does not model physical caches or the interconnect.

## Session and wire format

The image retains the 224-byte version-one boot contract and existing memory
layout. Its contract magic is `0x415337334f424331`; its monitor result magic is
`0x415337334f425231`. The original cached image's constants remain the defaults.
Private patches require exact reviewed boot/vector input hashes at configure
time. The build identity includes the observer, patched sources, TinyUSB inputs,
configuration, linker layout and compiler identity.

The device uses USB ID `1f3a:efe8`, revision `0101`, product string
`sunstoneos cached SMP observer`, and serial `sunstoneos-a733-smp-observe`.
Interface zero has bulk OUT endpoint `02` and bulk IN endpoint `81`. High-speed
packets are 512 bytes; full-speed descriptors use 64 bytes. The personality is
distinct from ROM and the existing RAM diagnostic.

Only one request may be outstanding. Each request is exactly 64 bytes:

| Offset | Field |
| --- | --- |
| 0 | Four little-endian uint32 values: magic `53534f53`, sequence, command `31`, body length 48 |
| 16 | Admitted epoch and generation, two little-endian uint64 values |
| 32 | Nonzero 32-byte host challenge |

Sequence starts at one and increases by one. The challenge remains unchanged
throughout the session. A malformed, stale, excessive or reordered request
retains failure. A partial request is bounded by the diagnostic deadline.

Each response is 528 bytes: a 16-byte header with matching magic and sequence,
command `80000031`, and body length 512, followed by this body:

| Body offset | Field |
| --- | --- |
| 0 | uint64 magic `415337334f425331` |
| 8 | uint32 version 1 and uint32 length 512 |
| 16 | uint64 status: zero means completed, one means pending |
| 24 | Epoch, generation, lease, start counter, end counter, diagnostic deadline and completion mask; seven uint64 values |
| 80 | Copied 32-byte challenge |
| 112 | Compiled 32-byte build identity |
| 144 | Sixteen reserved zero bytes |
| 160 | Eight rows of five uint64 values: rounds, background progress, SGIs, failure and ticks |
| 480 | SHA-256 of body bytes 0–479 |

Pending responses have zero start/end counters, completion mask and core rows.
A completed response has mask `ff`, all required per-core results, and an end
counter before the diagnostic deadline. The digest detects incomplete or changed
frames; it does not authenticate hardware or provide remote attestation.

The host must bind the frame to its current prepared image, epoch, generation,
lease and challenge, validate every criterion, and save the exact bytes with
durable file and directory flushes before classifying recovery. USB transfer
queue acceptance is insufficient; the service waits for completion before
accepting another request. A short transfer, disconnect or expired deadline
cannot produce a successful host result.

## Admission and evidence

The separate launcher must enforce the cached image's CPU, dual-PAS DRAM,
dirty-alias, clock, rail and independent-watchdog prerequisites. It additionally
requires exclusive USB0 controller, PHY, pin and shared clock ownership and the
inherited reference/interconnect state required by the exercised USB adapter.
An OS result cannot establish those prerequisites retrospectively. Early CPU or
interconnect faults may prevent any USB result from becoming available.

The observer uses the existing TinyUSB MUSB patches and USB0 reset sequence.
Its adapter keeps trace storage out of the Secure monitor and replaces scheduler
delays with bounded counter delays. Controller initialization and FIFO logic
have prior single-core physical evidence in [the USB record](usb.md); that
evidence does not qualify this image's cached, multicore or watchdog integration.

Focused evidence is under
`~/.cache/sunstoneos/evidence/smp-observe-20261016/`. The first build preserves the
executed ELF and exact inputs. Sanitized host checks compare SHA-256 output with
Python's independent implementation and check result admission. Native
instruction checks cover the actual exception frame, caller authentication,
publication, fragmented input/output, pending-to-completed responses, changed
challenges, deadline failures and bounded native USB preparation. USB FIFO and
task-event APIs are modeled for service composition. The complete scheduler
suite is reused from the cached image; no board operation or performance trial
was run for these checks.
