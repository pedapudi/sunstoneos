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

## CE board probe

The first board result for the CE is small and goes in this order, one board run each.

1. **State at entry (read-only).** Every run now captures `CE_CLK_REG` (`0x02002ac0`) and `CE_BGR_REG` (`0x02002ac4`) in FEL before the image loads. If the CE is gated or held in reset (`CE_RST` bit 16 or `CE_SYS_RST` bit 17 low, gates in bits 1:0 clear), nothing before the image can have left CE DMA running, and the image's reset-and-gate cycle at admission is the exclusion. The CE's own registers are not read in FEL: a gated or reset-held CE can stall the bus.
2. **Secure-bank access from the image.** Map only the CE register page (`0x04603000`, the nonsecure bank at `+0x000` and the secure bank at `+0x800`) and read the bank registers the driver reads (`a733_ce_native_read`), with no write. This shows whether the OS, running nonsecure at EL1 under our monitor, can read the secure bank at all, or whether the peripheral firewall (SPC at `0x02054000`, whose register layout the manual does not give) must be opened first.
3. **CE-only probe.** Health tests programmed through the secure bank, one 32-byte TRNG request, a normal return. The record holds the byte count and the health status, never the bytes. It proves only that the CE runs on the board under the vendor clock (CE 400 MHz, `CE_CLK_REG` as in `a733_ce_clock.c`) and that the health tests accept a request. It does not qualify the entropy ([entropy](../entropy.md)).

Association follows once step 3 passes.

See [engine inventory](../security-engines.md), [entropy](../entropy.md), [identity](../board-application.md) and [source provenance](../../apps/crypto/security-sources.json).

## Evidence

Board results are records in `docs/evidence/security.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-security-20261101.md).

<!-- evidence-table:begin -->
1 records in `docs/evidence/security.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `security-host-checks` | 2026-11-01 |  | `` | not-run | passed | sid_id_words 4 words |
<!-- evidence-table:end -->
