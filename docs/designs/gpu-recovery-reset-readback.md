# GPU recovery completes the reset write before power-off

Status: qualified on the board for the injected-timeout recovery and the qualified recognition workload; arbitrary stuck-device recovery is not qualified.

The recovery path stops kernel traffic, waits for the Sidekick, BIF and SLC idle conditions, asserts the full soft-reset mask, completes one read of the reset register, then powers off. Full-mask readback equality is not a completion condition: the register does not read back the written mask.

## Reference

The pinned open driver writes the full feature-selected mask at the end of `pvr_fw_stop` and does not compare the readback (`pvr_fw_startstop.c:315–320`). The vendor DDK's `RGXResetSequence` writes that mask and reads the register once to complete the write through the SOCIF pipeline, then discards the value (`services/server/devices/rogue/rgxstartstop.c:795–802`). Both sources are at Linux board revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`; their source records are in `sources.lock.json` and `~/.cache/sunstoneos/sources/gpu-fw-start-ddk-2ac08e8c/manifest.json`.

## Behavior

- The path keeps the reset write, ordered 32-bit accesses, barriers, mandatory idle polls and absolute deadlines.
- One completed reset read fences the write through SOCIF. The native backend authorizes that read only after the exact reset write in RECOVERING. A successful read marks write completion; its value is diagnostic data.
- A failed access or an expired deadline retains ownership and prevents power-off.
- The queue publishes the failed job's record only after platform OFF, firmware-memory reclamation and the stopped-backend proof. Restart requires fresh firmware health and MMU-cache completion. The peer's queued job stays retained throughout recovery.
- Stop is bounded by 300 ms and READY by one second. The reset read is one ordered 64-bit access, implemented as two 32-bit accesses, with no extra delay.

## Verification

Host fixtures return the observed partial reset value after the exact write. They require one read before OFF and reject failed reads, missing or incorrect writes, late completion, failed idle checks and failed OFF. Native queue cases cover retained recovery and failure ownership (`checks/a733-gpu-backend.c`).

| Result | Figure | Record |
| --- | --- | --- |
| Injected-timeout recovery returns normally | one recovered HANG fault; peer and next owner job completed | [gpu.jsonl](../evidence/gpu.jsonl) `1793560094069288734-8923c447` |
| Qualified recognition with the completed-read recovery | 186 completed GPU jobs, reference CRCs, complete thermal capture | [gpu.jsonl](../evidence/gpu.jsonl) `1793560622000131271-53954059` |
| Equality-poll diagnostic | reset write low word `0xfffffc3d`, last read `0xe8000020`, stop phase 8 step 15 timeout | [gpu.jsonl](../evidence/gpu.jsonl) `1793559451655627386-914e388f` |

## Limitations

The finite inputs above do not qualify arbitrary stuck-device recovery. The meaning of each bit missing from the reset readback is not identified.
