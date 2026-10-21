# AES-128 block transforms share the retained security engine

SunstoneOS implements bounded AES-128 ECB encryption and decryption through the
A733 security engine. Each request contains 16 to 2048 bytes in complete
16-byte blocks. The controller submits at most 1024 bytes per hardware task.
This is an unauthenticated block primitive. Padding, other cipher modes, key
ladders, authentication and Wi-Fi CCMP remain separate implementation work.
Physical cipher output is not yet qualified.

## One session owns all security-engine work

The cipher service binds the actual retained session used by SHA and entropy.
Its capability profile must include AES. A native constructor reserves the one
physical engine and obtains one page from the existing linker memory pool.
The service does not allocate another engine token or invent a second exclusion
provider. The existing pool service lock serializes all service calls.

A request owns the session's client slot until complete output publication.
Other clients return BUSY during its tasks and key cleanup. The native platform
must authenticate clocks, memory mappings, both security worlds, IRQ exclusion,
remote-client exclusion, independent prior DMA drain and recovery. These are
physical prerequisites. An AES-only profile needs no entropy health assertion.
A mixed profile retains the existing TRNG qualification requirements.

The public API is [a733_ce_cipher.h](../include/a733_ce_cipher.h). Begin copies
the caller's key and input into private CPU staging and returns a generation.
Polling that generation publishes output once, after every task and key cleanup
succeeds. The output may reuse the caller's original input storage. Invalid
lengths, directions, aliases and stale generations cannot start a task.

## Symmetric descriptors use byte lengths and normal input keys

The source basis is the pinned A733 manual, sections 19.1.4.15 and 19.1.7, and
the board kernel's CE v5 implementation at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The relevant files are
`v5/sunxi_ce_proc.c`, `v5/sunxi_ce_reg.c`, `v5/sunxi_ce_reg.h` and
`sunxi_ce_cdev.h` in the cached CE source set.

The symmetric descriptor occupies 220 bytes in the existing 256-byte
reservation. Channel 2, interrupt enable, AES method zero and direction bit 8
select the task. The symmetric control word is zero: AES-128, ECB and normal
input-key selection. Key-ladder and internal-key selectors remain disabled.
IV, counter and descriptor-chain fields are zero.

The key pointer begins at byte 16. Data length at byte 32 and scatter lengths
at bytes 48 and 52 count bytes. Scatter source and destination pointers begin
at bytes 36 and 41. All pointers contain 40 address bits. Both descriptor
address registers are written before launch. Symmetric launch uses bit 0;
hash and entropy continue to use bit 10. Channel 2 completion uses status bits
5:4, with value 1 required for success.

The v5 execution branch sets aligned byte lengths. A legacy descriptor comment
still calls the field a word count; that comment does not describe this branch.
The implementation follows the active A733 path and manual. It also avoids the
source driver's magic-string key selection: all 16 caller bytes are ordinary
key material.

## Cache ownership separates input, output and the software key

The complete memory object occupies 3520 bytes of the retained 4096-byte page.
Every region begins on a separate 64-byte cache line.

| Offset | Bytes | Purpose |
| --- | --- | --- |
| 0 | 256 | Descriptor reservation |
| 256 | 64 | SHA or entropy output |
| 320 | 64 | SHA continuation state |
| 384 | 2048 | Input reservation |
| 2432 | 1024 | Cipher task output |
| 3456 | 64 | Software key reservation |

Native publication cleans descriptor, input and key lines, then cleans and
invalidates cipher output lines. Completion invalidates those output lines
before the controller copies task bytes into private service staging. The
caller receives no partial result if a later task fails.

A consumed successful AES task permits cleanup of its key, input and cipher
output lines. The controller checks the actual token and deadline, takes the
common mutex, zeros those lines and completes cache cleaning and barriers.
Plaintext, decrypted output and the key therefore do not remain in the retained
page after a successful task, including while later TRNG or SHA tasks run. The session refuses
reuse or client release until cleanup succeeds. Final token checks follow all
cleanup and publication callbacks. Busy metadata preserves completed cleanup
and staged output without resubmitting a task. The source driver's successful
completion followed by key unmapping supports this task-local reuse boundary.
It does not establish global engine drain.

Internal CE key-register erasure is not proven. Failed or uncertain tasks retain
DMA key, input and output backing under the same reservation. Private CPU
staging is cleared on failure; quarantined DMA memory is not cleared after
uncertain activity. A failed cleanup also retains the session and
prevents caller output. No reset, cancellation or resource release API is
provided.

## Focused checks cover the new cipher transitions

The focused model runs the actual controller, session, cipher, SHA, entropy,
clock registry and memory pool. A separate model cipher executes the submitted
AES descriptors. Expected output comes from NIST SP 800-38A Appendix F.1.1 and
F.1.2, with an independent OpenSSL comparison. The
[NIST publication](https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38a.pdf)
defines the fixed AES-128 ECB vectors.

Selected cases cover block and task boundaries, both directions, copied inputs,
shared client exclusion, deferred entropy provisioning, cleanup and publication.
A failed second task, uncertain command acceptance, output-cache failure,
key-clean failure, late authority loss and a final deadline with busy retention
must leave caller output untouched. Source checks authenticate the cached manual and CE files;
native compilation includes the actual MMIO and cache implementation. Existing
lower-layer checks are reused. These checks do not qualify physical AES output,
DMA visibility, security-world exclusion or recovery.

The focused sanitizer model, source checks, NIST/OpenSSL comparisons and native
composition passed on 2026-10-16. The native build compiled the changed CE
controller, native adapter, session, cipher service and affected entropy/SHA
consumers. It also compiled the current clock registry and reused authenticated
memory-pool, reservation and translation objects. No older driver suite or
physical test was repeated. Final authority checks introduced here protect AES
cleanup and publication; they do not change SHA or entropy publication rules.

Exact sources, commands, executables, native objects and logs are preserved in
`~/.cache/sunstoneos/evidence/ce-cipher-20261016/`. A dependency preflight refused
an older clock-registry object before any compilation or model execution;
the successful batch compiled the current registry once.
