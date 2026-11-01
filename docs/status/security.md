# Security, entropy and identity

**Status: implemented.**

CE hashing, SHA/HMAC, MD5/SM3, AES ECB/CBC/CTR,
GCM through CE AES plus CPU GHASH, conditioned TRNG requests, authenticated
inputs and CPU-staging erasure have software evidence. SID reads four ID words.

**Board-qualified scope:** None.

CE cryptographic and entropy operations have no physical qualification.
Read-only clock and identity observations do not qualify engine execution.

Digest, staging, native input and upload-erasure checks pass under
`~/.cache/sunstoneos/build/security-1031/`. Earlier mode evidence is in
`crypto-modes-20261028/` and fixed CE400 provisioning evidence is in
`ce-clock-20261020/root-integration.json`, relative to
`~/.cache/sunstoneos/evidence/`.

Launch is blocked on predecessor-DMA accounting, exclusion of future secure
and nonsecure clients, secure-bank access, reachable private memory, IRQ/drain
and stable ownership. The current application polls once per tick with
INTIDs200/201 masked, parks until power removal and has no CE-specific returning
watchdog contract. It is ineligible for returning runs.

Only conditioned TRNG output is documented. No A733 raw-source export or startup
completion counter is established. Other `sunxi-trng` layouts do not establish
the selected CE node's behavior. Entropy assessment, raw-source checks and
current-enable startup tests remain missing.

Remaining functions include native CE GCM and further AES modes,
DES/3DES/SM4/RC4, RSA/ECC/SM2, PRNG/DRBG, hardware-key/key-ladder operations,
eFuse provisioning and mutable secure storage. Internal key erasure is
unproven. Do not infer write authority from implemented software.

See [engine inventory](../security-engines.md), [entropy](../entropy.md),
[identity](../board-application.md) and
[source provenance](../../apps/crypto/security-sources.json). The older component
statement that MD5/SM3 preparation is refused is superseded by its integrated
producer section and the current digest checks.
