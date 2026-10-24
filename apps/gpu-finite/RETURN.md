# Returning single-core GPU application

This composition is for maintainers reviewing the A733 single-core GPU return boundary. `return-build.py` links the accepted finite GPU client with an application-local monitor, exit shim and reset tail. It preserves the authenticated 288-byte contract, 592-byte session, full 768 MiB write-back pool and 16 MiB non-cacheable pool. The image remains physically ineligible until the coordinator reviews architectural return and hardware admission. No physical GPU execution or ROM return is established by this evidence.

The client copies both job results, completes normal CLOSE, suspends the GPU worker and parks the PMIC task. Its existing completion guard checks ownership, locks, requests and INT95 before entering a task critical section. The replacement `platform_exit` runs at EL1t. It requires successful completion, five exact DEVICE parent leases and no additional pool consumers. It checks only accessible CCU, PCK and GIC registers after GPU power-off. Firmware, BIF, SLC, wrapper and reset observations come from successful native CLOSE; the shim performs no powered-off GPU-window reads.

The five parents are firmware WB at `0x48000000..0x48022000`, compute payload WB at `0x48022000..0x4807a000`, compute tables WB at `0x4807a000..0x48086000`, firmware NC at `0x78000000..0x7802d000` and compute firmware NC at `0x7802d000..0x78033000`. Exact ledger slot, generation, owner, extent and pool geometry are checked. Firmware owner is 9; compute owner is 10. Allocation parents remain DEVICE-owned after CLOSE; the firmware memory adapter is OFFLINE. These intervals identify the fixed workload's allocated bytes and do not reduce authorization or host restoration of the complete admitted pools.

After device stop is established, EL1 masks exceptions and disarms its physical and virtual timers. It publishes a receipt in retained WB memory, then executes DC CVAC over the linked CPU-private interval and all 134 allocated WB pages through its existing Nonsecure EL1 mappings. Each line is bounded by a monotonic counter and a deadline capped by the admitted session. The NC parents require ordering rather than cache cleaning. A DSB completes publication. The assembly tail clears EL1 M/C/I without accessing a stack afterward, then issues SMC 0x733.

EL3 requires the actual EL1t caller profile, disabled EL1 M/C/I and a valid published receipt. It rechecks the protected admission, session digest, runtime copies, exact allocation ledger, finite stop state and accessible device-off predicates. Exception ESR, ELR and FAR remain local; the exception entry does not overwrite dirty Nonsecure log bytes through an uncached Secure alias. EL3 restores the inherited interrupt configuration within the same bounded interval and publishes the copied output packet at `0x42000100`. Failures retain ownership in the original stop loop. No failure authorizes restoration or another device start.

EL3 performs no VA cache clean of Nonsecure data. With EL3 translation disabled, that operation would address Secure physical cache tags and could leave Nonsecure dirty lines unpublished. The final reset tail calls the existing set/way `clean_data_cache` helper to retire late EL1 stack stores in both security spaces, invalidates instructions and requests the existing RVBAR/RMR reset sequence. Monitor translation tables were constructed at cacheoff EL3; they are not claimed as dirty Nonsecure EL1 tables. Secondaries remain admitted OFF throughout. The coordinator must review this architectural sequence before execution.

The corrected two-job candidate and its frozen inputs are identified in
[HOST.md](HOST.md#candidate-and-bounded-physical-procedure). Reuse that image
for preparation. The development builder below starts from the earlier finite
adapter; its retained objects do not include the later repeated-submission
callback correction. Rebuilding it does not reproduce the corrected candidate.

For development of the monitor composition, use a fresh evidence directory:

```sh
python3 apps/gpu-finite/return-build.py --evidence EVIDENCE
~/.cache/sunstoneos/venv/bin/python apps/gpu-finite/return-check.py --evidence EVIDENCE
~/.cache/sunstoneos/venv/bin/python apps/gpu-finite/return-record.py --evidence EVIDENCE
```

The builder binds reused objects to their original frozen source closure, including the accepted earlier runtime. It generates the monitor from a pinned frozen source plus `monitor.patch`; a generated platform copy replaces only the old exit function's symbol. Shared sources remain unchanged. New compilation dependencies and the final ELF have separate hash bindings. The checks run admission, retained BSS placement, actual EL1 platform initialization and allocator producers before modeling the accepted post-CLOSE state. A security-tagged cache model keeps dirty Nonsecure lines distinct from DRAM until Nonsecure EL1 CVAC. Secure-only maintenance and an omitted receipt line must retain. Final set/way maintenance accounts for late stack writes. GPU completion, hardware register effects, cache behavior and reset outcome remain modeled; the accepted eight device cases are reused.

The host proposal in the generated record requires coordinator-owned identity and physical admission, complete pre-launch snapshots of every declared dirty range, authenticated session preparation, one bounded launch and restoration only after a verified return packet and ROM availability. It creates no queue item. Timeout or retained failure requires supervised recovery and excludes automatic retry. Earlier shared-byte handoff results outside the `ns-publication` evidence directory are superseded: they did not model physical address security and cannot support publication correctness.
