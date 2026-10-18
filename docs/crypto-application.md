# Native SHA-256 and AES diagnostic

The supervised crypto image composes the existing hardware crypto-engine (CE)
clients with the native FreeRTOS board providers and copied USB output. It runs
SHA-256 over `abc`, encrypts the four AES-128 ECB blocks from NIST SP800-38A
section F.1.1, then decrypts that ciphertext. Every byte must match the fixed
reference before success becomes visible. This diagnostic does not use TRNG,
qualify entropy, expose a general cryptographic service or establish key erasure.

One retained CE session owns the actual singleton cacheable pool page and an
exclusive CE token in the shared clock registry. The token also holds PERI0,
reference, oscillator and fabric clocks stable. The constructor runs before the
scheduler; one application task at priority 4 subsequently serializes every CE
client call and retention attempt with its static pool mutex. The register
callbacks use the board's actual common register lock and counter. USB and PMIC
providers continue to run independently.

The image adopts an already enabled CE400 clock state. It does not program
clocks, assert reset or invent a drain operation. The coordinator's read-only
snapshot in `~/.cache/sunstoneos/evidence/crypto-clocks-20261018-082/session.json`
observed module word `0x81000000`, bus/reset word `0x00030003` and CE MBUS gate
bit 2 enabled. The existing CE driver checks those local controls, shared PLLs,
output gates and the admitted fabric configuration on each operation. Cold
clock provisioning remains separate work.

The 824-byte immutable session extends the existing USB0-only supervised board
session. Its complete digest covers the CE owner and authority, physical evidence
identities and fixed input/reference identities. Host preparation requires
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

The application keeps the digest, ciphertext and recovered plaintext private until
all three comparisons and a final authority/deadline check pass. It then publishes
one immutable 160-byte result using release/acquire synchronization. Any task
failure, mismatch, stop or expiry revokes success and permits only retention
retries under the pool lock. The DMA page, clock token and uncertain register
ownership remain reserved. No failure clears potentially active DMA backing,
retries a command or releases resources. Successful AES cleanup covers the
software key cache line; internal CE key-register erasure remains unqualified.

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
