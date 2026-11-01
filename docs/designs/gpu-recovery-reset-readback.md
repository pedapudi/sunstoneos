# GPU recovery completes the reset write before power-off

Status: implemented and host-tested; injected-timeout recovery returns normally
on port 3-3 in queue entry `1793560094069288734-8923c447`.

The recovery path stops kernel traffic, waits for the existing Sidekick, BIF
and SLC idle conditions, asserts the full soft-reset mask, then powers off.
Full-mask readback equality is outside its completion contract. A port 3-3 diagnostic of that equality condition
reaches the final read but times out: entry
`1793559451655627386-914e388f` records stop phase 8, step 15, timeout error 3
and the last read's low word `0xe8000020`. The written low word is
`0xfffffc3d`. This observation does not identify the meaning of each missing bit.

## The reference requires write completion

The pinned open driver writes the full feature-selected mask at the end of
`pvr_fw_stop`, without comparing its readback
(`pvr_fw_startstop.c:315–320`, Linux board revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`).
The vendor DDK's `RGXResetSequence` writes that mask and reads the register
once to complete the preceding write through the SOCIF pipeline. It discards
the value (`services/server/devices/rogue/rgxstartstop.c:795–802`, same
revision). Their source records are in `sources.lock.json` and
`~/.cache/sunstoneos/sources/gpu-fw-start-ddk-2ac08e8c/manifest.json`.
Neither sequence defines full-mask equality as a reset-completion condition.

## Complete one read without comparing its value

The path preserves the reset write, ordered 32-bit accesses, barriers,
mandatory idle polls and absolute deadlines. One completed reset read fences
the write through SOCIF. The native backend authorizes that read only after the
exact reset write in RECOVERING. A successful read marks write completion;
its returned value remains diagnostic data. A failed access or expired deadline
retains ownership and prevents power-off.

The queue publishes the failed job's record only after platform OFF,
firmware-memory reclamation and the existing stopped-backend proof. Restart
still requires fresh firmware health and MMU-cache completion. The peer's
queued job remains retained throughout recovery.

Voltage, clock, power-domain and firmware-start operations follow their
existing sequences. Stop is bounded by 300 ms and READY by one second. Reset
performs one ordered 64-bit read, implemented as two 32-bit accesses, without
an extra delay.
The acceptance budget is recovery within those bounds, exact peer and owner
outputs, and normal GPU/CPU retirement.

## Check the real completion boundary

Host fixtures return the observed partial reset value after the exact write.
They require one read before OFF and reject failed reads, missing or incorrect
writes, late completion, failed idle checks and failed OFF. The reset write's
value and all earlier register operations must still match the pinned source.
Native queue cases cover retained recovery and failure ownership. Board entry
`1793560094069288734-8923c447` returns normally with one recovered HANG fault,
the queued peer and the next owner job completed. Qualified recognition entry
`1793560622000131271-53954059` returns with 186 completed GPU jobs, reference
CRCs and transcript, and complete thermal capture. These finite inputs do not
qualify arbitrary stuck-device recovery.
