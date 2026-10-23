# Native SHA streams share one crypto-engine owner

The A733 hash driver accepts a stream of bytes and returns one SHA-1, SHA-224,
SHA-256, SHA-384 or SHA-512 digest, or the HMAC of the stream under any of
those hashes. It submits real hardware descriptors through the existing CE
command controller. Software adds SHA padding and forms the HMAC key blocks;
the engine performs every compression operation. Empty messages, exact block
multiples and messages spanning multiple DMA tasks are implemented. Physical
hashing remains unqualified.

The stream interface is `include/a733_ce_hash.h`, implemented in
`drivers/crypto/a733_ce_hash.c`. The common owner is
`include/a733_ce_session.h` and `drivers/crypto/a733_ce_session.c`.
[Entropy requests](entropy.md) and hashing share that actual session, its clock
token, its controller and one page from the existing linker memory pool.

## A session admits capabilities and owns the engine permanently

A session admits TRNG, hashing or both when constructed. The hash capability
bit, named `A733_CE_CAP_SHA256` and aliased `A733_CE_CAP_HASH`, admits every
SHA method because each method uses the same execution unit, memory and
security authority. Its immutable capability
profile participates in the existing platform authorization and binding checks.
SHA-only admission requires clock, memory, security, IRQ, remote-exclusion,
prior-drain and recovery authority. It requires neither a TRNG health profile
nor an entropy-quality or startup-test assertion. It does not access the TRNG
health controls, source-mode register or RC16M gate requirement.

The native boot constructors reserve the one physical CE instance before
allocating a page. A second construction is refused, including one using a
different token. Failed construction can undo that reservation only while the
session remains uninitialized. An initialized or fault-retained owner persists
for its entire lifetime. Both constructors use the actual singleton linker pool.
The entropy constructor checks its TRNG capability and object placement, then
calls the session constructor and attaches the entropy client to that session.
If attachment fails after the session is bound, the session is retained.
Portable binding requires equivalent exclusion of every other controller or
session borrowing the same token or physical engine.

A native SHA-only application constructs a session with
`a733_ce_session_native_init`, then attaches a hash client with
`a733_ce_hash_bind`. A mixed application constructs the existing entropy service
with both capabilities and attaches hashing to its `engine` pointer. It does not
construct a second CE session. Existing entropy entry points retain their
request, generation, complete-output and fault behavior.

The common pool service lock serializes every session and client operation.
A typed client slot identifies the active request or hash stream. Hashing holds
that slot across all updates, task completions and finalization. Entropy requests
return busy while the stream owns it. Completing a digest releases only the
software client slot; the physical page, registry token and platform authority
remain reserved.

## Descriptors process padded blocks and preserve intermediate state

The first task uses the engine's standard initial state for the selected
method. Later tasks use the prior intermediate result as an opaque
initialization vector. The driver copies those bytes without interpreting or
swapping them; manual section 19.1.4.13 states that a digest output can be
configured directly as the next IV. Every task leaves the last-package flag
clear, so software supplies all padding and the engine processes complete
blocks.

| Method | Selector | Block | State written per task | Digest |
| --- | --- | --- | --- | --- |
| SHA-1 | 1 | 64 bytes | 20 bytes | 20 bytes |
| SHA-224 | 2 | 64 bytes | 32 bytes | first 28 bytes |
| SHA-256 | 3 | 64 bytes | 32 bytes | 32 bytes |
| SHA-384 | 4 | 128 bytes | 64 bytes | first 48 bytes |
| SHA-512 | 5 | 128 bytes | 64 bytes | 64 bytes |

SHA-224 and SHA-384 tasks request the full 32- or 64-byte state, because a
continuation needs the complete state. The pinned CE v5 driver does the same:
it initializes SHA-224 and SHA-384 contexts with the SHA-256 and SHA-512 state
sizes and requests that size for every package that is not the last. Because
software padding keeps every task a non-last package, the driver truncates the
final state to the digest length in software. SHA-384 and SHA-512 padding uses a
128-bit length field whose upper 64 bits are zero.

A 208-byte descriptor occupies 256 private bytes at the page start. Separate
cache lines hold the output at offset 256, the intermediate state at offset 320
and up to 2048 input bytes at offset 384. Native publication cleans descriptor,
state and input, then cleans and invalidates the output. Successful completion
invalidates the output before CPU acquisition. After the controller copies the
digest and stops the task, it zeros the output, intermediate state and input
lines and cleans them under the register mutex. The input, digest and an
HMAC-style intermediate state therefore do not remain in the retained page.
A failed scrub withholds the digest and retains the session. Cache maintenance
uses the shared AArch64 device-cache helper, which refuses an unknown or
larger-than-64-byte writeback granule, a caller outside EL1 and a disabled MMU
or data cache. All referenced physical spans
must be admitted as CE-accessible RAM; encoding an address does not establish
its reachability or security permissions.

| Descriptor field | Implemented value |
| --- | --- |
| Channel | 2, under complete engine exclusion |
| Command | Method selector from the table above, without RNG or hardware HMAC |
| IV enable | Clear for the first task; set for continuation |
| Last package | Clear for every software-padded task |
| Current length | This task's input length in bits, bytes 8–12 |
| IV address | Page base plus 320 when continuing |
| First source | Page base plus 384; byte count at offset 36 |
| First destination | Page base plus 256; state bytes from the table above |
| Total length | Cumulative submitted bits in words 50 and 51 |
| Unused entries and chains | Zero |

The manual describes software-padded hashing in section 19.1.4.9 and the
shared descriptor in section 19.1.4.14. The pinned A733 CE v5 source supports
block tasks with a clear last-package flag and copies intermediate results
into the next IV. Its forced-last-package compatibility mode applies to a
different SoC. The implementation uses the documented software-padding path
without depending on the manual's inconsistent description of hardware padding.

## Updates copy input and finalization publishes one digest

`a733_ce_hash_start` selects the method and optional HMAC key, reserves the
client slot and issues a generation number. `a733_ce_hash_begin` is the same
call for plain SHA-256.
One absolute deadline covers the complete stream, including time between
updates. `a733_ce_hash_update` copies an accepted prefix of a chunk of at most
2048 bytes into private staging. The caller can change its input immediately
after the call. When staging fills, polling submits and completes a task before
accepting more input.

Finalization appends the marker byte, zero fill and original message bit length.
At the largest partial-buffer boundary, padding occupies at most 2176 bytes; the
driver submits a 2048-byte task followed by a task of one or two blocks. The maximum admitted message
length leaves enough space for padding within the 64-bit cumulative bit counter.

Polling returns the digest once, after all tasks complete and the final deadline
check passes. `a733_ce_hash_poll_digest` takes the method's digest length;
`a733_ce_hash_poll` accepts only 32-byte digests. A later poll of that generation reports consumption without
copying again. New streams receive fresh generations. Invalid lengths, aliases,
stale handles and exhausted generations are refused before consuming input.

## HMAC runs as consecutive engine hashes within one stream

HMAC follows RFC 2104. With a key of at most one block, the driver zero-pads the
key to the block length, privately stores it and queues the key combined with
the inner pad byte `0x36` ahead of the message. After the inner digest, the
stream restarts from the default initial state, queues the key combined with
the outer pad byte `0x5c` followed by the inner digest, and hashes that. Only
the outer digest is published. A key longer than one block, up to 1024 bytes,
is first hashed by the same stream: `a733_ce_hash_start` then returns PENDING,
and polling completes that hash and returns READY before any message input is
accepted. An empty key is valid.

The engine's own HMAC mode is not used. Its key handling is not described in
enough detail to verify without hardware, and software key blocks let one
engine path serve all five methods. The key and the inner digest occupy only
private service staging and the input line of the task that hashes them. Every
successful task zeros and cleans that input line, and the private key copy is
zeroed when the digest is published or the stream fails.

## Failures retain both the engine and the page

Completion checks the shared controller's actual task status and both security
banks' error words. Timeout, uncertain launch, changed binding or failed cache
operations prevent digest publication. Private CPU input, intermediate-state
and HMAC key staging are erased. Potentially active DMA memory remains quarantined.

A fault affects the shared session, so another client cannot resume the engine.
Registry contention during retention retries metadata only. No failure path
releases the client slot or resets hardware. Successful task completion permits
reuse of the same reserved memory; global drain, shutdown and reclamation remain
separate requirements.

## Focused evidence covers composition and stream boundaries

`checks/a733-ce-hash.c` runs the actual session, entropy, hash, controller, pool
and registry code against a descriptor-driven SHA compression model. Its cache
model separates published input from mutable caller storage. Python's independent
`hashlib` implementation checks the returned digests in `checks/a733-ce-hash.py`.
The model covers empty input, padding boundaries, multiple tasks, both client
orders, deferred TRNG provisioning, native construction exclusion and retained
failures. Modeled digest equality does not qualify physical hardware output.

The focused ASan/UBSan model, independent digest comparison, pinned-source
checks and native AArch64 compilation pass. The existing entropy fixture also
compiles with the extracted private fields; its full suite was not rerun. A
relocatable native link resolves the actual session, pool, registry and translation
implementations. Final linker symbols, runtime `memset` and the pool module's
separate DMA constructor remain external image inputs. This is not a boot check.

`checks/a733-ce-modes.c`, driven by `checks/a733-ce-modes.py`, runs the same
real code against a descriptor-driven model engine for all five methods. The
model's SHA round constants and initial values come from the script, which
derives them from prime square and cube roots with exact integer arithmetic.
The script compares 155 streams with Python `hashlib` and `hmac`: 13 message
lengths from 0 to 5003 bytes for each method, and HMAC keys of 0, 20, 64, 100,
131 and 1024 bytes with three message lengths for each method. The model also
checks each descriptor's selector, state length and continuation state. It
covers refusal of unknown methods, wrong block multiples, over-long keys,
aliased keys and wrong digest lengths, and retention after a failed HMAC task.
These checks, run on 2026-10-21 under ASan and UBSan, pass. They do not qualify
physical hash output. In the board image build with these methods, the six CE
objects contain 37,680 bytes of code and eight bytes of BSS.

For the initial SHA-256 stream, the five CE objects contained 28,736 bytes of
code and eight bytes of BSS for the native owner pointer. The largest direct
compiler-reported stack frame was 352 bytes; no whole-call-chain bound is claimed. Exact executed inputs and
artifacts are preserved in
`~/.cache/sunstoneos/evidence/ce-hash-20261016/batch1/`.

MD5, SM3, the engine's hardware padding and HMAC modes, asymmetric operations,
key ladders and secure key handling remain required engine coverage. Hashing does not resolve the
raw-source characterization and startup qualification required for production
security use of entropy output.
