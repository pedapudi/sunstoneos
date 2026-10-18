# Board providers consume retained authenticated inputs

The board boot path seals the session before SID access, device-pool admission
or provider construction. Primary EL3 copies the contract-declared span from
the reserved session page into private image memory. The declared length must
cover the 592-byte board prefix and fit within the 4,096-byte page.

The helper hashes that complete private span against the already validated,
copied boot contract. It also validates the board prefix and its epoch,
generation, lease and image identity. Only successful cache publication exposes
the retained prefix. Failure closes the getter and stops boot; the helper does
not retry or reclaim its inputs.

Shared board startup and SID admission read this prefix rather than the
uploaded session. Supervised runtime deadlines are derived in the existing
working session; the admitted prefix remains unchanged. JPEG, crypto and H.264
retain their separate full-session seals. Their boot patches still compose
after the shared board patch without source changes.

Bluetooth copies its bounded versioned profile into private image memory and
validates the copied length, digest and board policy before SID access. It
publishes that profile before reading the four admitted chip-ID words.
Successful identity publication exposes the same profile to pin admission and
application startup. Uploaded profile changes cannot replace those inputs.

The network path copies its profile before hashing it. Both armed and
supervised paths use those private bytes; supervised runtime derivation receives
the same bytes whose digest matched the retained session.

These changes bind software inputs. They do not establish physical cache
coherence, SID exclusion, DMA isolation, transport reliability or recovery.
The loader and board authority must still establish their existing physical
admission prerequisites.

One affected supervised board image and one headset-capable Bluetooth image
link successfully. Fifteen new native binding cases pass. Actual session,
contract and Bluetooth boot-policy validators accept the derived nominal
inputs before the behavior cases execute. The fixtures model cache publication,
EL3 state, SID values, address translation and provider work.

Evidence is retained under
`~/.cache/sunstoneos/evidence/board-session-seal-20261018`.
It includes the initial host-only baseline refusal, the cache-hook dispatch
failure trace, reviewed input corrections and exact producer identities.
The final setup recipe regenerates the passed session/profile bytes exactly.
Base, supervised, JPEG, crypto and H.264 boot patches compose successfully.
Existing codec, Bluetooth protocol and recognition evidence was reused.
