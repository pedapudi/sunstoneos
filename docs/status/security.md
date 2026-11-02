# Security, entropy and identity

**Status: implemented.**

CE hashing, SHA and HMAC, MD5 and SM3, AES ECB, CBC and CTR, GCM through CE AES plus CPU GHASH, conditioned TRNG requests, authenticated inputs and CPU-staging erasure have software evidence. SID reads four ID words.

**Board-qualified scope:** None.

CE cryptographic and entropy operations have no physical qualification; read-only clock and identity observations do not qualify engine execution.

## Open work

1. Unblock launch: account for predecessor DMA, exclude future secure and nonsecure clients, grant secure-bank access, reach private memory, and add IRQ, drain and stable ownership. The current application polls once per tick with INTIDs 200 and 201 masked, parks until power removal and has no CE-specific returning watchdog contract, so it is ineligible for returning runs.
2. Establish entropy: no A733 raw-source export or startup completion counter is known, and other `sunxi-trng` layouts do not establish the selected CE node. Entropy assessment, raw-source checks and current-enable startup tests are missing.
3. Add native CE GCM and further AES modes, DES, 3DES, SM4, RC4, RSA, ECC, SM2, PRNG and DRBG, key-ladder operations, eFuse provisioning and mutable secure storage. Internal key erasure is unproven.

See [engine inventory](../security-engines.md), [entropy](../entropy.md), [identity](../board-application.md) and [source provenance](../../apps/crypto/security-sources.json).

## Evidence

Board results are records in `docs/evidence/security.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-security-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/security.jsonl`.
<!-- evidence-table:end -->
