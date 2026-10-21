# Native finite SHA-256 and AES operations

The supervised crypto image accepts one caller-supplied SHA-256 message or
AES-128 ECB operation per admitted session. It composes the existing hardware
crypto-engine (CE) clients with native FreeRTOS providers and copied USB output.
Session version 1 retains the fixed diagnostic. It runs
SHA-256 over `abc`, encrypts the four AES-128 ECB blocks from NIST SP800-38A
section F.1.1, then decrypts that ciphertext. Every byte must match the fixed
reference before success becomes visible. This diagnostic does not use TRNG,
qualify entropy, provide a persistent cryptographic service or establish key erasure.

One retained CE session owns the actual singleton cacheable pool page and an
exclusive CE token in the shared clock registry. The token also holds PERI0,
reference, oscillator and fabric clocks stable. The constructor runs before the
scheduler; one application task at priority 4 subsequently serializes every CE
client call and retention attempt with its static pool mutex. The register
callbacks use the board's actual common register lock and counter. USB and PMIC
providers continue to run independently.

Session versions 1 and 2 adopt an already enabled CE400 clock state.
Version 3 provisions the fixed CE clocks from an independently admitted
reset-held, gated state. No version asserts reset or treats reset as drain. The coordinator's read-only
snapshot in `~/.cache/sunstoneos/evidence/crypto-clocks-20261018-082/session.json`
observed module word `0x81000000`, bus/reset word `0x00030003` and CE MBUS gate
bit 2 enabled. The existing CE driver checks those local controls, shared PLLs,
output gates and the admitted fabric configuration on each operation. The cold initialization path is described below.

The 824-byte immutable session extends the existing USB0-only supervised board
session. Its complete digest covers the CE owner and authority, physical evidence
identities and the selected input commitment. Version 1 binds fixed input and
reference identities. Version 2 binds a finite operation request and an optional
output oracle. Host preparation requires
separate reviewed records for both-world ownership, independently resolved prior
CE DMA, boot lineage, secure-bank read permission, masked IRQ routes 200 and 201,
private reachable cacheable RAM without aliases, stable clocks and permanent
retention until whole-board power removal. Startup validates actual device
translations and the visible IRQ masks before CE access. Those observations do
not establish the independently required physical facts.

An authenticated predecessor handoff can establish initial quiescence if it
accounts for every submitted task and excludes further launches in both worlds.
Proven absence of submissions since a qualified reset is another possible basis.
Neither basis requires a global reclamation primitive because this session keeps
its memory and authority permanently.

No reviewed record currently establishes that predecessor handoff, exclusion
and secure status-bank access for the actual ROM/DDR boot lineage. The ready clock snapshot does not close those
gaps. A physical request must remain blocked until the coordinator obtains that
evidence. Source inspection found no independent global drain primitive. Task
status zero and reset readback cannot establish the missing predecessor handoff.

The version-1 application keeps the digest, ciphertext and recovered plaintext private until
all three comparisons and a final authority/deadline check pass. It then publishes
one immutable 160-byte result using release/acquire synchronization. Any task
failure, mismatch, stop or expiry revokes success and permits only retention
retries under the pool lock. The DMA page, clock token and uncertain register
ownership remain reserved. No failure clears potentially active DMA backing,
retries a command or releases resources. Successful AES cleanup covers the
key, input and cipher output cache lines, and successful SHA completion covers
the output, intermediate state and input lines; internal CE key-register
erasure remains unqualified.

USB uses product `sunstoneos native CRYPTO`, serial `sunstoneos-a733-crypto` and
the existing PID `0xefe8`. A zero byte requests a 128-byte header; success appends the 160 copied
bytes. The header binds epoch, generation, fixed inputs/reference and the complete
payload hash. The host checks exact reference bytes, preserves each frame and
makes the copied output durable before recording success. Existing supervised
transport, queue claim, board lock, target identity and failure handling remain
in force. The native lifetime is 60 seconds and host capture ends before it.

Configure `SUNSTONE_A733_CRYPTO=ON` and `SUNSTONE_BOARD_SUPERVISED=ON`, then
build `sunstone-a733-crypto`. Prepare an
admitted package with `apps/crypto/prepare.py`; the coordinator uses
`tools/check-a733-crypto-supervised.py` only after queue review. A package built
from modeled authority facts is a software fixture and must never be deployed.
The existing CE controller, SHA/AES client and shared supervisor evidence is
reused. New checks target application admission, sequential orchestration,
publication revocation, copied framing and host acceptance. Native linkage and
modeled instruction execution do not qualify physical CE behavior.

The focused software batch is preserved at
`~/.cache/sunstoneos/evidence/crypto-native-20261018/`. Both sanitizer fixtures,
the linked task/output cases, host integrity/refusal cases and offline package
model passed. The first admission fixture exposed undefined pointer arithmetic
in its test mutation code. Its zero exit code was rejected; the original source,
object and diagnostic remain preserved. Only the corrected fixture unit was
rebuilt, linked and rerun with fail-fast sanitizers. Its emitted session stayed
identical, so the other successful results and native image were reused.

The native image is
`~/.cache/sunstoneos/build/crypto-native-20261018/sunstone-a733-crypto.elf`, with
SHA-256 `073e2b0c7c792db92bb418697fe84d162db394307a9369f80c104fb066af5464`.
The dependency record covers every linked object and the actual imported headers.
The package model also compiles the ARM launcher and exercises the real host
entry point, access/discovery selectors and PyUSB lookup against local fixtures.
Its authority mock prevents deployment through the unmodified validator. No CE
hardware operation or cold-start qualification was performed.

## Retained admission and terminal USB failure

A crypto-local boot patch now seals the entire 824-byte session during primary
EL3 admission. It rehashes the copied bytes against the private admitted
contract, validates the complete session and publishes the retained copy before
provider construction. A failed attempt cannot be retried, and failed coherence
publication leaves the snapshot unavailable. The fixed USB0-only image replaces
the unnecessary live board identity read with this hook; shared boot sources
remain unchanged.

Provider setup and application startup both compare the sealed board prefix
with the board's admitted prefix. Startup copies only the sealed extension.
A disagreement refuses provider or CE construction, and later mutations at the
live session address cannot replace admitted CE owner, authority or evidence.
The fixed SHA/AES inputs, reference bytes and arithmetic paths are unchanged.

Every crypto USB terminal stop now calls `board_fail` before parking. Shared
liveness and the existing read-only crypto output gate therefore close before
any further queued transmission. The owner task observes failure and follows
its existing retained-session path; no healthy CE operation or resource release
is introduced. This correction does not establish physical CE admission.

The focused correction passed its sanitizer seal fixture and six linked
consumer/revocation cases. The instruction checks model cache publication,
CE construction, USB provider observations and scheduler boundaries. They execute
the actual sealed-session checks, board failure/liveness and output gate, without
running SHA/AES operations. The target links only its crypto boot object, whose
seal hook runs before cached setup. Evidence is preserved at
`~/.cache/sunstoneos/evidence/crypto-sealing-20261018/`.

The first external batch script stopped before configuration because it retained
a JPEG-only fixture variable. That script and diagnosis are preserved. Its
successful compile/import and sanitizer results were reused; only configuration,
the affected native link and remaining instruction cases resumed. The fixed
vectors, CE clients and prior arithmetic/reference evidence were unchanged and
were not replayed. The corrected image supersedes the earlier mutable-session
handoff; physical admission remains separately blocked.

## Independent boot-CPU image

The target `sunstone-a733-crypto-single-core` runs the same finite SHA-256/AES
application on the boot CPU. Configure the standalone
`apps/crypto-single-core` project with the existing pinned toolchain, FreeRTOS,
TinyUSB and board-provider dependencies, or select the crypto application from
the repository root. The all-core crypto target remains available.

Crypto-local patches reuse the accepted retained single-core monitor, runtime
and board-provider startup. They seal both the board prefix and the complete
824-byte crypto session at EL3. Retained EL1 copies survive BSS clearing while
the actual EL1 translation tables deny access to the monitor contract, saved
state and tables. Startup checks every secondary power/control/mode word before
provider initialization. The SRE kernel runs the existing trusted tasks on the
boot CPU and restores the preparation exception mask after static task creation.

CE registers retain outer-shareable Device mappings. The retained cacheable CE
page belongs to the compact 2 MiB write-back pool; the board USB provider retains
its separate 2 MiB non-cacheable pool. The monitor disables all SPIs, including
CE interrupts 200 and 201. The unchanged CE client polls completion and preserves
its cache maintenance, both-world admission, shared-clock token, common register
mutex and permanent failure retention. No CE operation, TRNG feature or entropy
claim is added by the startup change.

Host preparation binds `crypto-single-core-supervised-v1` to immutable ELF
geometry, the boot entry and the absence of secondary-startup symbols. It requires
USB ROM-device takeover mode 2 and the corresponding primary-cache, secondary-OFF,
ROM PIO, IRQ and supply evidence. The all-core profile retains mode 1. Existing
crypto-specific predecessor-DMA, secure-bank, clock, memory and supervision
requirements still apply. Bundle validation repeats the profile checks against
frozen inputs before capture.

The frozen image is
`~/.cache/sunstoneos/evidence/crypto-single-core-20261018/image.elf`, with SHA-256
`59625fdc1fe6a375ad6f196b9fc5009d1e110b9ee51d5018ab5330fec221d7e7`.
Its compiler closure preserves 415 source/header inputs and 161 linked objects.
Twelve focused startup checks cover sealed copies, EL1 BSS clearing, actual
Device/write-back/non-cacheable descriptor walks, protected-monitor refusal,
native CE/provider/task construction, exception masks and startup refusals.
Six host groups cover package/profile binding, frozen-condition retention and
copied output capture. The accepted SHA/AES output frame is reused without
running arithmetic again. CPU registers, counter, GIC and cache publication
remain modeled; the software admission facts establish no physical authority.
Focused source/ELF review found no software blocker.

The preserved `host/MODEL-NOT-DEPLOYABLE-bundle` is a software fixture and must
never deploy. The coordinator must establish the unresolved CE predecessor-DMA,
both-world exclusion and secure-bank permissions before preparing an actual
bundle. Bind that bundle to the selected board's current DDR/cache/alias,
secondary-OFF, USB ROM PIO, supply, clock and recovery evidence. Use the existing
crypto preparation and supervised capture tools for one RAM-only attempt. Success
requires the complete matching 160-byte copied result. Stop after the first
transport failure and retain all resources until confirmed removal of every
board power source, including after success. Physical CE execution remains
unqualified; this independent startup does not close the existing admission gaps.

## One caller-supplied request per admitted session

Session version 2 keeps the 824-byte session layout. Its `fixture` field commits
the SHA-256 digest of the exact serialized operation. Its `reference` field is
zero for ordinary execution. A nonzero reference optionally commits the output
SHA-256 digest for qualification. Ordinary output needs no preknown answer.
The retained session still binds the image, epoch, generation, owners, physical
entry evidence and lifetime before USB input is accepted.

`apps/crypto/request.py` creates the finite operation from a message file and an
optional 16-byte AES key file. SHA-256 accepts zero through 2,048 bytes. AES-128
ECB encryption and decryption accept 16 through 2,048 bytes in complete 16-byte
blocks. These limits match the existing CE clients. No padding, IV, authenticated
encryption or entropy operation is introduced.

The request has a 48-byte little-endian header followed by the message:

| Offset | Field |
| --- | --- |
| 0 | 64-bit magic `0x4153373343525131` |
| 8 | 32-bit version 1 |
| 12 | 32-bit operation: SHA-256 1, AES encrypt 2, AES decrypt 3 |
| 16 | Nonzero 64-bit caller request ID |
| 24 | 32-bit input byte count |
| 28 | Reserved 32-bit zero |
| 32 | 16-byte AES key; all zero for SHA-256 |
| 48 | Exact message bytes |

The request tool can copy a session template to version 2, set its input
commitment and clear its output oracle. `--oracle` supplies an expected output
file only when qualification needs that comparison. Physical admission must
cover the resulting template and selected image. Preparation's `--request`
argument seals the complete operation as `operation.bin` in the existing bundle.
Bundle validation checks its exact hash and shape against the admitted template.

A USB upload consists of command byte 1, a 32-bit serialized-request length and
the request bytes. The USB task accumulates bounded fragments in private staging.
Every fragment and final commit require the retained current epoch, generation
and deadline. The task copies the full operation to private owner storage before
checking its hash and shape, then publishes it with release synchronization.
The CE task acquires that publication before reading the request. Further uploads
are terminal refusals and cannot replace the published bytes. Command byte zero
continues to request status and copied output.

For version 2, boot creates only the private mutex and task. Partial, malformed,
uncommitted or expired uploads cannot acquire a CE clock token or DMA page.
After authentication the CE owner constructs the existing native session under
its mutex, then runs the selected finite client. This fixed USB-only image has
one cacheable device-pool consumer; board USB uses the separate non-cacheable
pool. No other task allocates from the CE pool during deferred construction.
These ownership assumptions are required for the existing pool interface.

AES acceptance copies the key into the existing cipher client before clearing
the application's request-key bytes. Successful output still requires the
client's software key-line cleanup. Terminal failure clears the owner task's
copied CPU key staging under its mutex. Unpublished request cleanup remains with
the USB producer. Uncertain DMA backing, clock tokens and active ownership stay
quarantined; cleanup does not touch the DMA key line after uncertainty. Internal
CE key-register erasure and complete removal of all external key copies remain
unqualified.

Version-2 responses use a 144-byte header. The first 32 bytes retain magic,
version, status, output size, epoch and generation. Offsets 32, 40 and 44 contain
the 64-bit request ID, 32-bit operation and 32-bit input size. Offsets 48, 80 and
112 contain the request, complete retained-session and copied-output SHA-256
digests. Success appends exactly 32 SHA-256 bytes or the admitted AES extent.
Non-success responses carry no payload and a zero output digest. The host checks
all identities and sizes before making output durable. It preserves partial
bulk reads and accepts up to eight empty reads per frame within the existing
deadline, covering TinyUSB's intervening zero-length packets.

The focused software checks cover arbitrary copied input, empty and maximum
SHA-256 requests, both AES directions and maximum extents, intermediate hash
polling, deferred construction, upload refusal, 64-bit request identity,
publication revocation, CPU key cleanup and optional oracle comparison. Native
USB frames feed the host-copy tests. Diagnostic orchestration and host capture
remain compatible. The package check uses actual linked-image identities,
producer-derived request records and accepted provider fixture values; its
physical authority remains explicitly modeled. Focused Astra review has no
unresolved findings.

Exact inputs, linked objects, images, frames and results are preserved at
`~/.cache/sunstoneos/evidence/crypto-runtime-input-20261018/`. The standalone
native target links without changing CPU startup, CE drivers, shared providers
or entropy policy. No hardware operation or queue entry was created. CE execution,
secure-bank access, independently resolved prior DMA and physical drain remain
unqualified.


## Fixed cold clock provisioning for caller requests

Session version 3 preserves the 824-byte layout and the version-2 operation and
response formats. It requires a complete authenticated SHA-256 or AES request
before acquiring the CE token or constructing its memory. Existing diagnostic
and inherited-clock caller sessions retain their original behavior.

The [clock source manifest](../apps/crypto/clock-sources.json) pins the A733
manual and effective kernel clock, reset and CE sources. The fixed provider
runs on the FreeRTOS CPU. It uses the existing CE exclusive token and stable
oscillator, normalized reference, PERI0 and fabric leases. It neither changes
parent PLLs nor enables entropy features.

Cold admission uses the existing image-bound clock evidence field. The record
must establish `ce_local_reset_held_gated`,
`ce_mbus_gate_off_critical_neighbors_on`,
`ce_local_clock_reset_write_authorized` and `mbus_ce_gate_write_authorized`,
alongside stable parents and exclusion of competing clock/reset writers.
Both-world exclusion, independently resolved prior DMA, memory authority and
permanent retention remain separate requirements. The existing ready-clock
snapshot does not satisfy this cold admission.

Under the board register mutex, the provider checks CE clock fields at CCU
`0xac0` against mask `0x8700001f` with value zero, and local gates/resets at
`0xac4` against mask `0x30003` with value zero. MBUS bit 2 must be clear, while
neighboring gate bits selected by `0x40302` must already be set. Those bits are
real neighboring gates, despite the kernel helper's key naming. Every write
preserves them and every other field outside the CE mask.

The provider selects PERI0-400 with divider one while the module gate is off.
It then releases CE and SYS reset, enables the CE bus gate, module gate, MBUS
bit 2 and SYS gate in the pinned CE initialization order. Each write has full
readback, authority and deadline checks. Final local state must match CE400
before the existing memory session and SHA/AES clients are constructed. The
ordinary operation writer continues to refuse CCU writes; a separate native
adapter admits only these fixed monotonic transitions.

An ambiguous initial state, lost authority, expiry or uncertain write ends the
one attempt. The provider retains its token and any acquired register ownership;
it performs no rollback, reset or release. Successful initialization also keeps
all clock ownership reserved for the existing retained application lifecycle.
Software readback establishes configuration consistency. Physical frequency,
security permission, electrical timing and predecessor DMA require independent
qualification.


The focused cold-clock check passed 18 cases on the retained native image. It
executes the linked clock provider and native writer, followed by one existing
SHA request, allocator, CE client, cache adapter and copied response. Refusal
cases cover ambiguous local state, a missing neighboring gate, invalid parent
state, readback failure, lost authority, expiry and unauthorized native writes.
The host accepts the actual version-3 response and refuses each missing cold
clock fact. The prepared bundle preserves the original request and session.
Existing arithmetic evidence is reused because the CE arithmetic sources are
unchanged.

These checks use an EL1h function-call model with modeled RTOS APIs,
translations, inherited parent clocks and device completion. The SHA hardware
result is supplied by the fixture. Actual task-context construction, CPU
startup, SRAM launcher execution, PMIC and USB controller operation are outside
this check. Two setup failures and their checkpoints remain preserved; the
successful continuation starts from the authenticated request checkpoint.
There was no hardware access. Physical permission, timing, cryptographic
execution and DMA drain remain unqualified, and retained resources stay owned.

The image, actual compiler-input closure, original request/session, model
checkpoints, copied response and explicitly nondeployable bundle are preserved
at `~/.cache/sunstoneos/evidence/ce-clock-20261020/`. The `native-frozen`
manifest records the compiled image; `operation-frozen` records the composed
check and its source inputs.
