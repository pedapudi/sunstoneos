# Cached SMP observer launch and result capture

The observer host adapter prepares a separate cached eight-core image, launches
it under the existing four-second watchdog interval, and captures its diagnostic
result over USB before reset. Physical launch remains blocked until the
coordinator supplies the independent recovery, protected FEL input, memory and
platform admission evidence. Source and model checks do not qualify the board.

The implementation is [the host tool](../tools/check-a733-smp-observe.py) and
`platform/a733-smp-observe/launch.{S,c,ld}`. The existing uncached launcher and
host tool remain separate. The observer imports only library functions from the
old host tool; import does not discover devices or execute its command line.

## Preparation and launch

Preparation requires the reviewed observer ELF and a manifest identifying target
`a733-smp-observe`, protocol version 1 and the exact final ELF SHA-256. The tool
extracts load segments and the compiled build identity, creates fresh epoch,
generation and lease values, and builds the observer-specific SRAM launcher.
The image manifest, segments, launch inputs, source and build commands are
preserved in the bundle. Validation reconstructs segments and launch records
before admitting a bundle.

The launcher uses the previously reviewed SRAM layout:

| Reservation | Half-open address range |
| --- | --- |
| AArch32 launcher | `0x52000..0x52c00` |
| Request | `0x52c00..0x52c80` |
| Refusal/arm report | `0x52d00..0x52d80` |
| Stack | `0x53000..0x54000` |
| FEL scratch | `0x71000..0x71080` |
| Image and its exclusive RAM lifetime | `0x41000000..0x42200000` |
| Retained result reservation | `0x42000000..0x42004000` |
| Immutable boot contract | `0x42004000..0x42005000` |

Request magic is `0x415337334f4c4331`, with version 2 and 128 bytes. The copied
224-byte version-1 image contract is published with observer magic
`0x415337334f424331`. The launcher compiles against the cached image's guarded
boot header with that explicit override. The observer's different contract
prevents admission of the original cached or uncached image.

With the watchdog disabled, the host backs up SRAM, uploads exact image segments,
zeros the result/report reservations, and uploads the unpublished contract,
request and launcher. Each source is checked against its admitted length/hash
and sealed in a bounded memory file before FEL can read it. Every upload is
read back against the admitted hash. A changing ordinary source path cannot
change an in-progress transfer or enlarge its reserved extent.

The coordinator revalidates its queue claim and reads the protected input before
one FEL execution request. The native launcher validates identity, disabled
watchdog, RTC32K source and protection before mutation. It configures the
qualified ROM redirect, arms the four-second whole-system watchdog, verifies
readback and publishes the absolute startup and diagnostic deadlines. The
arm-to-RMR path has the unchanged 24,000-tick software bound at 24 MHz. This
one-millisecond bound depends on the qualified counter and its continuity
through the AArch32-to-AArch64 transition. It cannot bound a stalled MMIO
instruction; independent hardware recovery remains mandatory.

Ordinary pre-mutation refusals return through the preserved FEL ABI with a fresh
epoch/generation/lease report. Unsafe architecture, uncertain MMIO and
post-mutation failures park under the existing conservative contract. A returned
original ROM is classified as refusal only after the one permitted report probe
succeeds and the watchdog is confirmed disabled. There is no automatic retry or
host USB reset.

## Session-bound observation

The native observer uses VID/PID `1f3a:efe8`, revision `0101`, product
`sunstoneos cached SMP observer`, serial `sunstoneos-a733-smp-observe`, interface
0, bulk OUT `02` and bulk IN `81`. The host requires the same protected physical
port, a distinct accessible USB instance and those descriptors before sending
observer commands. The reconnect access rule also covers the eventual ROM
instance. No permission failure triggers another reset.

Each 64-byte request contains the fixed message magic, increasing sequence,
command `0x31`, epoch, generation and a fresh nonzero 32-byte challenge. Successful
pending responses permit another bounded request. Every request in the session
uses the same challenge. The protocol exposes no arbitrary register, memory,
watchdog-feed or reset command.

Each response contains a 16-byte header and a 512-byte payload. The host checks
exact length, sequence, response command, schema, epoch, generation, lease,
compiled build identity, challenge, reserved fields and payload SHA-256. These
checks bind a result to the admitted session and detect malformed data; an
unkeyed digest is not device authentication.

A completed result must report all eight completion bits, exactly 32 rounds on
every core, positive background and SGI progress, no per-core failure, at least
32 primary timer ticks, and counters within the admitted diagnostic interval.
Pending results must contain zero completion, counters and per-core rows. The
reported deadline cannot change between responses.

The host writes each exact response to a new file, synchronizes that file and its
containing directory, and only then records diagnostic success in the durable
session journal. A cached descriptor, a successful FEL execution request or a
new ROM enumeration cannot produce a scheduler success result. No result is
retrieved from DRAM or SRAM after reset, and no memory retention across reset is
assumed.

The host separately observes a fresh accessible ROM instance and obtains its
A733 version response. That records ROM recovery. Establishing the watchdog as the reset cause
requires independent evidence. A complete session requires both the durable successful
scheduler frame and the fresh ROM response. A recovered session without the
frame keeps the scheduler outcome unobserved. The coordinator retains the queue
claim for review.

## Admission and failure ownership

The host requires a claimed and reviewed queue entry, the exclusive board flock,
USB health and reconnect access. Its prerequisites are:

- The already qualified independent four-second RTC32K watchdog, whole-system
  reset, protected ROM return, stalled-interconnect recovery and physical counter
  continuity. This evidence is independent of the new diagnostic's success.
- Stable asserted physical FEL protection and restored SRAM workspace evidence
  for the current ROM instance.
- Initialized, exclusively reserved image RAM, publication with caches disabled,
  and exclusion of remote writers for the complete reservation.
- Source-backed CPU/DSU transitions and established cold cache/dirty-alias,
  dual-address-space DRAM, CPU/DMA ownership, USB clock/power/reset, Device
  mapping and counter conditions.
- Exact software evidence for the prepared image, launcher, host tool and imported
  helper, including the composed observer session and launcher equivalence.

The first physical coherence trial does not require its own successful result
as a prerequisite. The entry and recovery conditions above are separate from
physical coherence and scheduler behavior, which remain outcomes under test.
No supplied record can replace the missing physical recovery qualification.

Exhausting the local capture window after complete valid replies stops observer
requests and permits only passive instance observation followed by a fresh ROM
response. The scheduler result stays unobserved. USB timeouts, short replies and
malformed frames are never reclassified as local expiry.

A transport failure, changed identity, malformed response or uncertain operation
forbids further device requests. The tool records and quarantines the instance,
retains its board lock and claim after attempted execution, and waits for
coordinator recovery review. Successful result capture never feeds or disarms
the watchdog. A failure to persist evidence also retains ownership.

## Validation evidence

The composed host case uses the actual observer C frame encoder and request
parser. It covers immutable preload, one execution request, pending-to-completed
responses, session/length/criteria rejection, real file and directory
synchronization, separate recovery classification and one-call transport failure.
The native launcher compiles within its existing SRAM reservation. Source
comparison confirms its instruction sequence matches the reviewed launcher;
only request and contract identities differ.

Evidence is preserved outside Git under
`~/.cache/sunstoneos/evidence/smp-observe-host-20261016/composed-abi/`.
The earlier `composed/` attempt stopped while copying the boot header, before any
compile or behavior check. The corrected case copies the shared cached header
and supplies the same observer override used by the image build.

The `local-expiry/` follow-up reuses a preserved pending frame to check recovery
observation after the local capture budget expires. The `late-deadline/` case
checks that a changed session deadline remains fatal even after that budget
expires. Both reuse the encoder and native launcher artifacts; neither reruns
the earlier cases.

No old scheduler, SGI or launcher instruction suite was replayed. No board or USB
operation ran. These checks establish software behavior only; physical startup,
cache coherence, USB result delivery and independent reset remain unqualified.
