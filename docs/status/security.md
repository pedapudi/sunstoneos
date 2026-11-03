# Security, entropy and identity

**Status: implemented.**

CE hashing, SHA and HMAC, MD5 and SM3, AES ECB, CBC and CTR, GCM through CE AES plus CPU GHASH, conditioned TRNG requests, authenticated inputs and CPU-staging erasure have software evidence. SID reads four ID words.

**Board-qualified scope:** None.

CE cryptographic and entropy operations have no physical qualification; read-only clock and identity observations do not qualify engine execution.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| CE digests, HMAC, AES ECB, CBC, CTR and GCM, conditioned TRNG requests | host-tested | software evidence only | `security-host-checks` in [security.jsonl](../evidence/security.jsonl) |
| SID read | implemented | four ID words | [identity](../board-application.md) |
| Launch of the CE application on the board | open | blocked on DMA accounting, secure-bank access and ownership | open work item 1 |
| Entropy assessment of the TRNG | open | no raw-source export known | [entropy](../entropy.md) |

## Open work

1. Unblock launch: account for predecessor DMA, exclude future secure and nonsecure clients, grant secure-bank access, reach private memory, and add IRQ, drain and stable ownership. The current application polls once per tick with INTIDs 200 and 201 masked, parks until power removal and has no CE-specific returning watchdog contract, so it is ineligible for returning runs.
2. Establish entropy: no A733 raw-source export or startup completion counter is known, and other `sunxi-trng` layouts do not establish the selected CE node. Entropy assessment, raw-source checks and current-enable startup tests are missing.
3. Add native CE GCM and further AES modes, DES, 3DES, SM4, RC4, RSA, ECC, SM2, PRNG and DRBG, key-ladder operations, eFuse provisioning and mutable secure storage. Internal key erasure is unproven.

See [engine inventory](../security-engines.md), [entropy](../entropy.md), [identity](../board-application.md) and [source provenance](../../apps/crypto/security-sources.json).

## Evidence

Board results are records in `docs/evidence/security.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-security-20261101.md).

<!-- evidence-table:begin -->
1 records in `docs/evidence/security.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `security-host-checks` | 2026-11-01 |  | `` | not-run | passed | sid_id_words 4 words |
<!-- evidence-table:end -->
