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
4. Check the device page tables against the sealed session. The design in [one OS image](../designs/one-os-image.md#lifetime-word-and-sealed-session) places the sealed session and the EL1 lifetime copy in the read-only CODE block (0x41000000 to 0x41200000), and its page-table check covers only the EL1 regime. The GPU and NPU page tables are built at run time from the session's manifest, the registered graphs and device-memory leases, so no host check walks the tables an image builds. Their inputs exclude CODE by construction:
   - **NPU:** every buffer lies in the pool that `apps/os/npu.c` takes from the unified allocator, whose ranges start at `APP_REGION_FLOOR` (0x48000000, `include/app_region.h`). `a733_npu_memory_init` also requires a write translation at EL1 for every buffer the NPU writes.
   - **GPU:** manifest regions must be disjoint from 0x41000000 to `__device_pool_end` (`platform/a733/gpu_queue.c`). Firmware, payload and table pages are device-pool leases, and the link script places the device pool in the RAM region after `.bss`.

   The missing check walks the GPU and NPU tables that the builders produce from an image's actual inputs and requires no entry to map a sealed page. A mutation that adds such an entry must fail it. Device masters without an MMU (the DMA engine, USB and the SD controllers) take physical addresses from the same device-pool and allocator sources and are not covered either.

## CE board probe

1. **State at entry (read-only), done.** At FEL entry the CE reads `CE_CLK_REG` `0x81000000` and `CE_BGR_REG` `0x00030003` (the live-state capture of a 2026-11-03 scan run): enabled, out of reset, so the boot ROM or an earlier stage left it running. The CE's own registers are not read in FEL, so DMA activity is not excluded; the reset-and-gate cycle at admission is the exclusion (`drivers/crypto/a733_ce_clock.c`).
2. **Secure bank, dropped.** The vendor's non-secure Linux driver gets TRNG bytes from the non-secure bank alone and programs no health register ([crypto-engine.md](../crypto-engine.md)), so the probe neither maps nor reads the secure bank and no SPC layout is needed.
3. **CE entropy stage of the radio service.** The CE belongs to the radio service under the radio grant (Wi-Fi is its only user; it becomes a service of its own when a second user needs it). The profile's stage limit 4 (`BOARD_RADIO_STAGE_ENTROPY`) runs only this stage, before association: `apps/os/ce_probe.c` takes the engine through the vendor's bring-up with the reset-and-gate cycle, allocates the descriptor object from the unified allocator (device mask `UMEM_DEV_CRYPTO`), runs one 32-byte TRNG task, and the session returns normally. The radio module is not powered and the platform is not polled. The record (`include/a733_ce_probe.h`, decoded by `tools/ce_probe_result.py`, read through the radio's result descriptor) holds the state at entry and after the bring-up, the status, error and `CE_REG_LPC` words, the byte count, the measured task time, and whether the block was not all zero; never the bytes. It says the health tests are at hardware defaults and were not programmed. It proves only that the CE runs on the board under the vendor sequence and returns a block within the 2 s bound; it does not qualify the entropy ([entropy](../entropy.md)).

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
