# Native SHA-256 streams share one crypto-engine owner

The A733 SHA-256 driver accepts a stream of bytes and returns one 32-byte digest.
It submits real hardware descriptors through the existing CE command controller.
Software adds SHA-256 padding; the engine performs the compression operations.
Empty messages, exact block multiples and messages spanning multiple DMA tasks
are implemented. Physical hashing remains unqualified.

The stream interface is `include/a733_ce_hash.h`, implemented in
`drivers/crypto/a733_ce_hash.c`. The common owner is
`include/a733_ce_session.h` and `drivers/crypto/a733_ce_session.c`.
[Entropy requests](entropy.md) and hashing share that actual session, its clock
token, its controller and one page from the existing linker memory pool.

## A session admits capabilities and owns the engine permanently

A session admits TRNG, SHA-256 or both when constructed. Its immutable capability
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

The first task uses the standard SHA-256 initial state. Later tasks use the prior
32-byte intermediate result as an opaque initialization vector. The driver
copies those bytes without interpreting or swapping them. Every task leaves the
last-package flag clear, so software supplies all padding and the engine processes
complete 64-byte blocks.

A 208-byte descriptor occupies 256 private bytes at the page start. Separate
cache lines hold the output at offset 256, the intermediate state at offset 320
and up to 2048 input bytes at offset 384. Native publication cleans descriptor,
state and input, then cleans and invalidates the output. Successful completion
invalidates the output before CPU acquisition. All referenced physical spans
must be admitted as CE-accessible RAM; encoding an address does not establish
its reachability or security permissions.

| Descriptor field | Implemented value |
| --- | --- |
| Channel | 2, under complete engine exclusion |
| Command | SHA-256 selector 3, without RNG or HMAC |
| IV enable | Clear for the first task; set for continuation |
| Last package | Clear for every software-padded task |
| Current length | This task's input length in bits, bytes 8–12 |
| IV address | Page base plus 320 when continuing |
| First source | Page base plus 384; byte count at offset 36 |
| First destination | Page base plus 256; 32 output bytes |
| Total length | Cumulative submitted bits in words 50 and 51 |
| Unused entries and chains | Zero |

The manual describes software-padded hashing in section 19.1.4.9 and the
shared descriptor in section 19.1.4.14. The pinned A733 CE v5 source supports
block tasks with a clear last-package flag and copies intermediate results
into the next IV. Its forced-last-package compatibility mode applies to a
different SoC. The implementation uses the documented software-padding path
without depending on the manual's inconsistent description of hardware padding.

## Updates copy input and finalization publishes one digest

`a733_ce_hash_begin` reserves the client slot and issues a generation number.
One absolute deadline covers the complete stream, including time between
updates. `a733_ce_hash_update` copies an accepted prefix of a chunk of at most
2048 bytes into private staging. The caller can change its input immediately
after the call. When staging fills, polling submits and completes a task before
accepting more input.

Finalization appends the marker byte, zero fill and original message bit length.
At the largest partial-buffer boundary, padding occupies 2112 bytes; the driver
submits a 2048-byte task followed by a 64-byte task. The maximum admitted message
length leaves enough space for padding within the 64-bit cumulative bit counter.

Polling returns the digest once, after all tasks complete and the final deadline
check passes. A later poll of that generation reports consumption without
copying again. New streams receive fresh generations. Invalid lengths, aliases,
stale handles and exhausted generations are refused before consuming input.

## Failures retain both the engine and the page

Completion checks the shared controller's actual task status and both security
banks' error words. Timeout, uncertain launch, changed binding or failed cache
operations prevent digest publication. Private CPU input and intermediate-state
staging are erased. Potentially active DMA memory remains quarantined.

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

The five CE objects contain 28,736 bytes of code and eight bytes of BSS for the
native owner pointer. The largest direct compiler-reported stack frame is
352 bytes; no whole-call-chain bound is claimed. Exact executed inputs and
artifacts are preserved in
`~/.cache/sunstoneos/evidence/ce-hash-20261016/batch1/`.

Cipher modes, other hashes, HMAC, asymmetric operations, key ladders and secure
key handling remain required engine coverage. Hashing does not resolve the
raw-source characterization and startup qualification required for production
security use of entropy output.
