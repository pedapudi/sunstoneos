# AES modes share the retained security engine

SunstoneOS implements bounded AES encryption and decryption through the A733
security engine with 128-, 192- and 256-bit keys in ECB, CBC, CTR and GCM
modes. ECB and CBC requests contain 16 to 2048 bytes in complete 16-byte
blocks; CTR requests contain 1 to 2048 bytes; GCM requests contain 0 to 2048
data bytes, up to 512 bytes of additional authenticated data (AAD) and a 12- to
16-byte tag. The controller submits at most 1024 bytes per hardware task.
Every AES block operation runs on the engine. Software chains IVs between tasks
and computes the GCM authentication hash. Padding schemes, the engine's other
modes, key ladders, hardware key selection and Wi-Fi CCMP remain separate
implementation work. Physical cipher output is not yet qualified.

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

The public API is [a733_ce_cipher.h](../include/a733_ce_cipher.h).
`a733_ce_cipher_start` takes the mode, key, IV, AAD, tag length and direction,
copies them and the input into private CPU staging and returns a generation.
`a733_ce_cipher_begin` is the same call for AES-128 ECB. Polling that generation
publishes output once, after every task and key cleanup succeeds. The output may
reuse the caller's original input storage. Invalid key, IV, AAD, tag or data
lengths, directions, aliases and stale generations cannot start a task.
The AES capability bit, named `A733_CE_CAP_AES128` and aliased
`A733_CE_CAP_AES`, admits every key size and mode, because they use the same
execution unit, memory and security authority.

## Symmetric descriptors use byte lengths and normal input keys

The source basis is the pinned A733 manual, sections 19.1.4.15 and 19.1.7, and
the board kernel's CE v5 implementation at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The relevant files are
`v5/sunxi_ce_proc.c`, `v5/sunxi_ce_reg.c`, `v5/sunxi_ce_reg.h` and
`sunxi_ce_cdev.h` in the cached CE source set.

The symmetric descriptor occupies 220 bytes in the existing 256-byte
reservation. Channel 2, interrupt enable, AES method zero and direction bit 8
select the task. The symmetric control word at byte 8 holds the key size in
bits 1:0 (0, 1 or 2 for 128, 192 or 256 bits), the counter width in bits 3:2
and the mode in bits 11:8 (0 ECB, 1 CBC, 2 CTR). Key select bits 23:20 stay zero,
which selects the input key; key-ladder and internal-key selectors remain
disabled. CBC and CTR tasks set the IV pointer at byte 21 to the IV line and
the counter pointer at byte 26 to the output line. ECB leaves both zero, and
descriptor-chain fields are always zero.

CTR tasks always use the forward direction, since CTR decryption equals
encryption. Plain CTR uses a 128-bit counter (width code 3), as the pinned
source's `ss_cnt_set` does; GCM's CTR tasks use a 32-bit counter (width code 1),
as `ss_gcm_cnt_set` does and as GCM requires. The manual describes the counter
pointer as the address of the next block's IV, so the engine writes there. The
driver never reads that value back: it computes each next IV itself. A CBC
task continues from the last ciphertext block of the previous task, which is
the last output block for encryption and the last input block for decryption.
A CTR task continues from the previous counter advanced by the number of
blocks processed, modulo the counter width.

The key pointer begins at byte 16. Data length at byte 32 and scatter lengths
at bytes 48 and 52 count bytes. Scatter source and destination pointers begin
at bytes 36 and 41. All pointers contain 40 address bits. Both descriptor
address registers are written before launch. Symmetric launch uses bit 0;
hash and entropy continue to use bit 10. Channel 2 completion uses status bits
5:4, with value 1 required for success.

The v5 execution branch sets aligned byte lengths. A legacy descriptor comment
still calls the field a word count; that comment does not describe this branch.
The implementation follows the active A733 path and manual. It also avoids the
source driver's magic-string key selection: every caller key byte is ordinary
key material.

## Cache ownership separates input, output and the software key

The complete memory object occupies 3520 bytes of the retained 4096-byte page.
Every region begins on a separate 64-byte cache line.

| Offset | Bytes | Purpose |
| --- | --- | --- |
| 0 | 256 | Descriptor reservation |
| 256 | 64 | SHA or entropy output; CBC/CTR next-counter write |
| 320 | 64 | SHA continuation state; CBC/CTR IV |
| 384 | 2048 | Input reservation |
| 2432 | 1024 | Cipher task output |
| 3456 | 64 | Software key reservation |

Native publication cleans descriptor, IV, input and key lines, then cleans and
invalidates the cipher output lines and the next-counter line. Completion
invalidates those lines before the controller copies task bytes into private
service staging. The
caller receives no partial result if a later task fails.

A consumed successful AES task permits cleanup of its key, IV, next-counter,
input and cipher output lines. The controller checks the actual token and deadline, takes the
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

## GCM uses engine CTR tasks and a software authentication hash

GCM follows NIST SP 800-38D. One engine ECB task encrypts a zero block to form
the hash subkey H. The counter block J0 is the IV followed by `00000001` for a
12-byte IV; for other IV lengths, from 1 to 64 bytes, software computes J0 with
GHASH over the zero-padded IV and its bit length. Engine CTR tasks with a 32-bit
counter then start at J0 and process a zero block followed by the data, so the
first output block is E(K, J0), and the remaining blocks are the CTR result.
Software computes GHASH over the AAD and the ciphertext, then forms the tag as
GHASH xor E(K, J0). The GF(2^128) multiplication uses masks instead of
data-dependent branches.

Encryption outputs the ciphertext followed by the tag. Decryption takes the
ciphertext followed by the tag, recomputes the tag from the input ciphertext
and compares the leading tag bytes in constant time. On a match it outputs the
plaintext. On a mismatch, polling returns `A733_CE_CIPHER_AUTH`, the caller's
output is unchanged, private staging is zeroed and the client slot is released
as for a successful request. A mismatch is a property of the data, so the
session remains usable.

The engine's own GCM mode is not used. Its tag, AAD and length fields are
described in the manual and source, but not in enough detail to program
correctly without a board to test against. Building GCM from the engine's CTR
mode keeps every AES operation on the engine and uses only descriptor fields
that the modes above already use.

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

`checks/a733-ce-modes.c`, driven by `checks/a733-ce-modes.py`, runs the real
controller, session and services against a model engine that implements
AES-128/192/256 in ECB, CBC and CTR with both counter widths. The model writes
an unusable value to the next-counter line, so correct chained output shows that
the driver computes its own IVs. The script compares 87 results:

- ECB, CBC and CTR for each key size against the NIST SP 800-38A vectors and
  OpenSSL, in both directions.
- CBC at 1040 and 2048 bytes, and CTR at 1, 15, 17, 1023, 1025 and 2048 bytes,
  against OpenSSL. These lengths cross task boundaries and partial blocks, and
  one CTR request carries the counter through the 128-bit wrap.
- GCM encryption and decryption for the McGrew–Viega test cases 1–8 and 13–16,
  which include 8- and 60-byte IVs, against both the published values and an
  independent Python GHASH over OpenSSL AES blocks.
- GCM at 2048 data bytes with 512 AAD bytes, 1-, 12- and 64-byte IVs and 12-,
  13- and 16-byte tags against the independent implementation.
- Three GCM decryptions with a changed tag, ciphertext or AAD bit, which must
  return `A733_CE_CIPHER_AUTH` and leave the output unchanged.

The model checks every symmetric descriptor's control word, IV and counter
pointers and key bytes, and checks that the IV, next-counter, input, output and
key lines are zero after every key cleanup. It also covers refusal of invalid
key, IV, AAD, tag and data lengths, aliased buffers and wrong output sizes, and
retention after a failed GCM data task. A test build with a broken CBC
decryption chain makes the comparison fail. These checks, run on 2026-10-21 under ASan and
UBSan, pass. They do not qualify physical AES output.

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
