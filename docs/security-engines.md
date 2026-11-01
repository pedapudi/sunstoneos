# A733 security-engine implementation and qualification

The crypto application erases private CPU buffers when its session expires or
the board authority is revoked. This includes copied request data, plaintext,
decrypted output, intermediate hashes and cipher state. Its mutex serializes
erasure with service operations. Controller metadata and DMA backing remain
retained after uncertainty; erasing CPU staging does not establish engine drain.

`checks/crypto-staging.py --output DIR` compiles the actual orchestration and
request code with AddressSanitizer and UndefinedBehaviorSanitizer. It checks
complete CPU erasure, unchanged session state, normal orchestration, operation
failures and final publication refusal. The fixture does not instantiate DMA
memory. The scrub performs no controller access, cache operation or callback.
The initial result is under
`~/.cache/sunstoneos/build/security-1031/staging-check/`.

The [engine](crypto-engine.md), [hash service](crypto-hash.md),
[cipher service](crypto-cipher.md), [entropy service](entropy.md) and
[application](crypto-application.md) records describe the existing interfaces
and their physical evidence limits. No CE cryptographic operation is physically
qualified by these host checks.
