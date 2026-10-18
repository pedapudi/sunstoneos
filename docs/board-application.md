# Native board application

The board application composes the actual shared resource providers, USB0 device
output, independently scheduled Type-C service and power tasks, and the USB2
read-only host. It reserves private non-cacheable DMA memory before scheduling.
Its first client reads logical sector zero once and publishes all 512 copied
bytes only after data and command-status completion. Physical operation remains
unqualified.

The image derives from the cached eight-core maintenance image. It reuses the
cached boot, continuation, privilege routing, fatal hook, watchdog readback and
native USB0 provider initialization. Storage grants, maintenance workers and
storage protocol entry points are excluded. The application retains one shared
counter, lock service, clock registry, pin manager, rail ledger, PMIC and power
manager. The native radio coordinator uses the same providers, plus one retained CE
source and common pool mutex. Camera startup can use these providers later.

## Immutable startup inputs

`a733_board_session` defines the fixed session at `0x41f00000`. The cached boot
contract uses the board-specific contract magic and binds the complete session
SHA256 in its final four reserved words. Its first two reserved words identify
the session address and size. Epoch, generation, lease, build identity and
absolute watchdog deadlines must agree. The session contains no pointers or
storage authority.

The caller supplies reviewed wiring, memory, recovery and secure SID evidence,
exact provider owners, power profiles, expected pin configurations, Type-C
source limits, comparator bound, FUSB302 identity and controller revision.
Nonzero evidence identities refer to externally qualified facts; creating a
session cannot qualify hardware. There is no usable default session or launcher
that invents those facts.

Primary EL3 reads the four source-defined SID words under explicit session
secure ownership while translation and caches remain disabled. It publishes the
immutable typed identity before starting secondary cores. Its read-only access
does not supply wireless entropy.

The image uses the existing compact cached mapping profile with a two MiB
non-cacheable pool at `0x41c00000`. The USB2 constructor validates translations
and permanently reserves 128 KiB from the actual allocator before concurrency.
The complete controller object graph and private DMA ledger remain retained on
failure. Independent recovery and absence of remote aliases remain admission
requirements.

## Task ownership and copied output

Separate tasks own PMIC service, USB0 device output, Type-C I2C service, Type-C
GPIO power supervision and USB2 host progress. The admitted radio workload adds
its owner task, copied TCP client and watchdog supervisor. Only the USB2 task accesses its
TinyUSB host stack. Host failure revokes Type-C attachment but leaves both
Type-C tasks scheduled for bounded shutdown. Type-C tasks run at higher priority
and delay one tick between calls; deployment requires the existing one
millisecond scheduler tick and adequate scheduling margin.

The device and host each keep their original TinyUSB configuration and logical
port zero. The host object compilation renames the common TinyUSB exports and
time API, so their singleton state and owner-specific timing remain separate.
The actual component sources are unchanged.

USB0 uses the existing native provider and patched programmed-I/O DCD. A host
sends one zero byte to the vendor interface. The 72-byte response header contains
six little-endian 32-bit words: magic `0x52414f42`, version one, USB state, sector
length, radio state and verified radio-byte count. Two little-endian 64-bit
values identify the session epoch and generation; the final 32 bytes identify
the admitted radio profile SHA256.

USB state zero means pending, one means a completed copied sector, and two means
retained failure. Only USB state one carries a 512-byte payload after the header.
Radio state zero means disabled, one means pending, two means a full response
comparison followed by successful TCP CLOSE, and three means failure. Only
radio state two carries a nonzero verified-byte count. The host observer
`apps/board/observe.py` validates these states, lengths and identities against the
admitted session and profile before saving the copied sector. Successful sector
bytes remain immutable until the session expires.

The immutable workload selector chooses USB only or USB with native radio.
USB-only execution uses one externally armed watchdog window. Radio firmware
contains 391,000 bytes, requiring at least 7.82 seconds at the initial one-bit
400 kHz SDIO rate before protocol overhead. The four-second watchdog therefore
requires renewal during radio startup.

The radio supervisor uses the existing hardware reload mechanism. Only an
increased count of successfully completed nonempty SDIO transactions earns a
reload. A coherent publication supplies the current absolute operation deadline;
expired operations, failures and unchanged counts never earn one. Each renewal
retains the fixed session deadline and externally qualified reset margin. An
ordinary radio failure disables further renewal and requests Type-C shutdown.
Scheduler activity, USB polls and empty status reads earn no renewal.

Type-C shutdown begins ten milliseconds before provider admission expires,
leaving time for the existing bounded GPIO shutdown. Expiry closes provider
admission; independent recovery owns faults that stop processors or stall the
interconnect.

## Validation and qualification limits

The complete native image links the actual shared providers, CE and radio
startup, authenticated TCP/RSN sources and both TinyUSB roles. The focused runner
`checks/board-application.py` passed nine AArch64 instruction cases. Client cases
cover continued polling after a copied sector, retained host failure and
attachment loss after a copy. Watchdog cases cover refusal without progress,
renewal after a completed transfer and expired-operation refusal. Output cases
cover completed radio, failed radio and a pending sector. A separate admission executable passed with address and
undefined-behavior sanitizers. The host observer accepted matching output and
refused a changed session identity.

The client and watchdog cases model provider returns; the output cases execute
the actual frame code with modeled endpoint I/O. Existing Type-C and USB2
component evidence supplies their recorded protocol checks. Full native startup,
electrical operation, independent recovery and a physical transfer remain
untested.

Evidence is preserved under
`~/.cache/sunstoneos/evidence/board-application-20261017/batch1/`. It retains source
snapshots, authenticated dependency archives, generated boot/port sources,
compiled dependency bytes and hashes, command logs, the native ELF and every
executed fixture. Failed preparation logs remain separate. The runner resumes
unfinished steps and preserves processor state if an instruction allowance is
exhausted. Successful instruction cases were not repeated after the final
static symbol assertion was corrected for an unused function removed by the
linker.

Physical execution requires qualified boot, memory and power admission,
independent watchdog recovery, the populated switch identity, a suitable
high-speed storage device on TYPEC24 and observable USB0 output. Radio additionally requires the [native radio profile](board-radio.md), matching
read-only firmware spans, qualified CE raw-source characterization and startup
for the actual silicon and health-enable epoch, an access point and a fixed TCP
peer. Missing qualification refuses construction. No board operation, reset or
persistent write is authorized by this image's existence.

The central `sunstone-a733-board` target also compiles and links successfully.
Its 156 repository dependencies match the preserved worker snapshot. Central
artifacts, generated sources and actual dependency records are preserved in
`central-snapshot/` beside the original batch; `central-integration.json`
identifies the image and reused external inputs. The original focused runtime
checks were not repeated.

## Supervised RAM-only entry

`SUNSTONE_BOARD_SUPERVISED=ON` selects the same complete board application with
an explicitly different supervised contract. It reuses the cached all-core
foundation's supervised boot patch. The watchdog and reset redirects must be
disabled. The image neither admits an armed timer nor reloads one. The original
armed build remains the default and rejects supervised sessions.

Supervised selections zero and one retain the complete USB2 host path, with
selection one also starting radio. Selection two starts shared providers and
USB0 only; selection three adds radio to that path. The last two selections
construct no Type-C source or USB2 host and require no powered TYPEC24 accessory.
The full host path remains a separate required function. Its qualified source,
switch, comparator, attachment and accessory cutoff requirements still apply.
USB0 reports state three for a disabled host and never implies a sector result.

The supervised session remains 592 bytes at `0x41f00000`, with magic
`0x4153373342535332` and version two. All uploaded timer, RTC, timestamp and
deadline fields are zero. The separate 224-byte contract uses magic
`0x4153373342534332` and version two. Its last six words bind the session address,
size and complete SHA256. The SRAM launcher derives session start from the real
counter, allows one second for startup and sets a sixty-second terminal bound.
This initial bound accommodates the known firmware transfer minimum; it is not
a measured claim that radio startup will finish within it. EL1 preserves the
immutable admitted session and derives a private lifetime from cached startup.

The source-owned PIO USB0 and radio providers may retain all authority until
confirmed removal of every board power source. The CE session similarly keeps
its DEVICE page, descriptor and data backing after uncertainty. No uncertain
path releases memory, resets the engine or grants a new lifetime. Copied-output
admission closes before metadata retention. Manual power removal supplies no
live DMA-drain proof. Initial quiescence, prior CE DMA drain, private mappings,
cache-line ownership, remote/secure-master exclusion, masked IRQs and actual
entropy characterization/startup for the admitted silicon and health epoch
remain prerequisites.

CPU-scheduled Type-C service and GPIO cutoff continue independently of a failed
host worker. CPU or interconnect stalls can prevent software deadlines and GPIO
access. The recovery plan must account for every board and accessory power
source; software cannot claim autonomous VBUS cutoff after such a stall. The
image retains resources while stopped. Reconnection begins a new session only
after confirmed power removal and fresh initialization, preserving the microSD.

`apps/board/prepare.py` performs offline preparation and exposes
`validate_bundle(bundle)` for the root-owned live tool. It requires a reviewed
`a733-board-supervised-entry-v2` record and a binary session template; it supplies
no physical defaults. Inputs are snapshotted once for parsing, hashing and
copying. The bundle seals its image, manifest, session, optional radio profile,
firmware and evidence snapshots. Ordinary DDR admission covers
`0x41000000..0x42200000`; radio additionally requires a reviewed
`0x43000000..0x43100000` reservation. No radio asset is uploaded outside those
reviewed ranges. Preparation does not open a device. `observe.py` validates
captured bytes; live transport and device ownership belong to the coordinator.

The supervised descriptor uses PID `0xefe8`, device revision `0x0100`, product
`sunstoneos native board` and serial `sunstoneos-a733-board`. The root tool must
require these complete descriptors on the admitted port. A one-byte zero request
returns the existing 72-byte header and optional copied sector. Fresh epoch,
generation and lease identify each prepared session; the response binds epoch,
generation and the profile hash. There is no separate challenge field.

Supervised radio input uses profile version two with the same 1856-byte layout.
Uploaded platform/CE handoffs and absolute deadline fields must be zero. After
validating the raw session/profile hash, boot copies the profile into private
lifetime storage. It derives handoffs from the admitted generation, a forty-second
bootstrap bound, a fifty-second startup bound, and a client lifetime ending ten
milliseconds before the sixty-second session deadline. The private copy uses the
existing native constructor version and its own computed digest. Physical
qualification, chip identity, health epoch, firmware spans and peer data remain
unchanged. USB output identifies the original reviewed raw-profile digest.

The coordinator's live command is `tools/check-a733-board-supervised.py` with
`--bundle`, `--fel`, `--evidence`, `--database`, `--claim`, `--owner` and
`--supervised-manual-reconnect`. Its bounded capture remains separate from offline
preparation and file decoding. Native frame receipt establishes only the
selected operation: selection two reports USB0/provider service; selection three
also requires the verified TCP response; selections zero and one require the
copied sector. Independent watchdog behavior is not part of this experiment.

The software-only supervised checkpoint is retained under
`~/.cache/sunstoneos/evidence/board-supervised-20261017/batch1/`. The complete
native image built successfully with SHA256
`9cd129422d349a0a3d6738aea265211a304371122ddc6431592f2f949d1438fc`.
Focused checks passed for immutable admission, the sixty-second SRAM handoff,
exact firmware upload spans, private radio-profile derivation, selected task
construction, terminal software cutoff and radio metadata retention. ELF checks
confirmed the permitted USB descriptor and absence of hardware-watchdog admit
and reload symbols. Constructor boundaries in startup checks were modeled;
physical provider admission remains untested. Existing armed,
provider, protocol and scheduler evidence was reused. No board was accessed.

The central supervised build also passes; its 154 repository dependencies match
the application snapshot. Later shared-header edits affect comments only.
The live host tool passes 25 focused software cases covering shared transport,
exclusive ownership, selected workloads, malformed or stale replies, transfer
failures and durable result publication. Its seven source files matched the final
snapshot under `board-supervised-host-20261017/batch1/` at integration. A later
addition accepts an NPU result field in the shared tool without changing the
board path; `integration/npu-selector/` records that source comparison.
These checks use modeled
USB and FEL boundaries and a private host lock; they establish no physical result.

## Boot-CPU execution preserves the complete application

`apps/board-single-core` builds `sunstone-a733-board-single-core.elf` with the
existing board application, shared providers, USB0 output, Type-C service,
USB2 host, radio coordinator, CE entropy adapter, SDIO transport and network
service. The distinct profile is `board-single-core-supervised-v1`. It uses
the accepted single-core monitor and FreeRTOS SRE port. All seven secondary
CPUs must remain off; this target links no secondary startup or SMP barrier.

All four supervised workload selectors remain available. Selections zero and
one construct Type-C and USB2 host service, with selection one also constructing
radio. Selection two constructs shared providers and USB0; selection three adds
radio. The radio owner, client and network stack retain their existing identities,
queues, entropy qualification, firmware authentication and completion rules.
No radio operation is replaced by a separate demonstration.

The monitor authenticates and retains the complete board session before provider
construction. It reads the four admitted SID shadow words for radio selections
and retains their typed identity outside EL1-cleared BSS. The monitor explicitly
clears this retained state before each admission. EL1 receives a separate bounded
runtime contract and cannot read secure monitor state. Actual page tables map
radio and CE registers as outer-shareable Device memory, radio profiles/firmware
as normal cached memory, and the USB2 pool as normal non-cacheable memory.
The cached CE pool remains separate from USB2 DMA allocations. Provider binding,
radio construction and scheduler entry restore DAIF masking after FreeRTOS
static object construction.

New preparation for both board profiles places the linked `a733_smp_build_id`
bytes in `session.image`. The retained-session consumer compares this field with
the authenticated contract's linked identity. The ELF file hash remains the
separate admission and bundle identity. Previously the producer placed the ELF
hash in this session field. That value fails retained-session admission even
when its uploaded session digest is consistent. Existing frozen artifacts remain
unchanged. The RTC adapter preserves the board validator's default arguments
when cloning its frozen-evidence callback.

The single-core host profile checks exact ELF geometry, entry, excluded SMP
symbols, linked identity and ROM USB takeover mode two. The existing board
profile retains mode one. Image-bound platform condition files remain sealed
in the bundle, including physical instruction-publication and resource-ownership
evidence. Firmware upload addresses, sizes and hashes use the existing radio
profile and four pinned firmware artifacts. Preparation and capture continue
through the existing board tools and coordinator claim.

Fifteen startup cases, nine host cases and one native session-binding check pass
under `~/.cache/sunstoneos/evidence/board-single-core-20261018/`. Startup executes
the real constructors for every workload selector, including CE entropy, radio
session, Type-C and USB2. Descriptor walks, retained SID/session state, input
refusals and scheduler masking are checked. System registers, counters, SID and
GIC responses are modeled; the host completion frame is synthetic. The accepted
board, radio, protocol and entropy service evidence is reused without replay.
One affected RTC frozen-bundle validation also passes. Focused source/ELF review
found no remaining substantive blocker.

The frozen ELF SHA256 is
`423f3ca1c4fe5ca284263fbc390a2cae9c57a1b03eecc336ceab7c854f0216e7`.
Physical USB, radio, entropy, network and powered-host operation remain
unqualified. The smallest radio proposal is one coordinator-owned RAM-only
selection-three session with the existing one-second startup and 60-second
terminal bound. It requires actual image-bound memory/publication, USB takeover,
SDIO/module/pin/rail ownership, SID, CE prior DMA drain/private mappings,
characterized entropy and authorized firmware/network-peer evidence. Success
requires a captured radio state two with the expected verified response-byte
count; USB0 output alone does not establish radio completion. Full host
selections additionally require the existing powered accessory and cutoff
admission. Stop communication at the first failure or disappearance, retain
all resources, and recover only through confirmed whole-board power removal.
Software fixture bundles provide no physical admission.
